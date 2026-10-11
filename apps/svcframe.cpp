// svcframe —— libmini 应用 6：RPC 服务骨架（可观测性样板）
//
// 这个应用没有「业务」，它的价值是「一个常驻服务该长什么样」的完整起手式，
// 后面任何一个真实服务都可以从这里抄：
//
//   1. 分层配置：代码默认值 → INI 文件 → 环境变量；/api/config 暴露 source_of
//   2. 配置热加载：文件 mtime 变化即重载；运行期可改的项（日志级别、队列/重试）
//      立刻生效，启动期专属项（并发上限、工作线程）标注为需重启
//   3. 结构化日志 + 运行期动态调级
//   4. 指标：每方法调用/错误计数、耗时直方图、在途数、热加载次数；Prometheus 文本导出
//   5. 追踪：每个 RPC 一个 span（含方法名与参数长度），/api/traces 导出
//   6. 健康检查与优雅停机：先撤存活/就绪 → 等在途数归零（或到窗口）→ 停 RPC → 停控制面
//
// 用法：
//   svcframe demo                              自检（CI 用，绑 0 端口 + 临时目录）
//   svcframe serve --config svc.ini            前台服务（Ctrl+C 优雅停机）
//   svcframe call add "2 3" --port 8810        一次性调用（调试/脚本用）

#include <spdlog/spdlog.h>  // 调用 LogFacade::logger()->info() 需要完整类型

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app_common.h"
#include "libmini.h"
#include "utils/ui_panels.h"

using namespace libmini;

namespace {

using app::DemoReport;

const char* kEnvPrefix = "SVCFRAME_";

// ---------------- 默认配置层 ----------------

// 键名一律 `<模块>.<参数>`；RPC 相关的键与 RpcServer::apply_config 的前缀约定
// 对齐（rpc.port / rpc.host / rpc.max_in_flight ...），配置可直接灌给服务器。
// INI 里就是 `[rpc]` 段下的 `port = 0`。
void apply_defaults(ConfigFacade* cfg)
{
    cfg->set_default("service.name", "svcframe");
    cfg->set_default("service.http_port", "0");
    cfg->set_default("rpc.port", "0");  // 0 = 自动分配
    cfg->set_default("rpc.host", "127.0.0.1");
    cfg->set_default("rpc.worker_threads", "4");
    cfg->set_default("rpc.max_in_flight", "64");
    cfg->set_default("rpc.drain_timeout_ms", "1500");
    cfg->set_default("rpc.queue_wait_ms", "10000");
    cfg->set_default("rpc.retry_after_seconds", "1");
    cfg->set_default("log.level", "info");
    cfg->set_default("log.file", "");
}

// 生效设置：从门面一次性读出的普通结构体。服务内部只认它，不直接读门面，
// 这样「配置从哪来」与「服务怎么跑」彻底解耦。
struct Settings
{
    std::string name = "svcframe";
    int http_port = 0;
    int rpc_port = 0;
    std::string rpc_host = "127.0.0.1";
    std::size_t worker_threads = 4;
    std::size_t max_in_flight = 64;
    int drain_timeout_ms = 1500;
    int queue_wait_ms = 10000;
    int retry_after_seconds = 1;
    std::string log_level = "info";
    std::string log_file;
};

LogLevel parse_log_level(const std::string& text)
{
    const std::string low = to_lower(trim(text));
    if (low == "trace") {
        return LogLevel::Trace;
    }
    if (low == "debug") {
        return LogLevel::Debug;
    }
    if (low == "warn" || low == "warning") {
        return LogLevel::Warn;
    }
    if (low == "error" || low == "err") {
        return LogLevel::Error;
    }
    if (low == "critical") {
        return LogLevel::Critical;
    }
    return LogLevel::Info;
}

Settings read_settings(const ConfigFacade& cfg)
{
    Settings s;
    s.name = cfg.get("service.name", "svcframe");
    s.http_port = cfg.get_int("service.http_port", 0);
    s.rpc_port = cfg.get_int("rpc.port", 0);
    s.rpc_host = cfg.get("rpc.host", "127.0.0.1");
    s.worker_threads = static_cast<std::size_t>(cfg.get_int("rpc.worker_threads", 4));
    s.max_in_flight = static_cast<std::size_t>(cfg.get_int("rpc.max_in_flight", 64));
    s.drain_timeout_ms = cfg.get_int("rpc.drain_timeout_ms", 1500);
    s.queue_wait_ms = cfg.get_int("rpc.queue_wait_ms", 10000);
    s.retry_after_seconds = cfg.get_int("rpc.retry_after_seconds", 1);
    s.log_level = cfg.get("log.level", "info");
    s.log_file = cfg.get("log.file", "");
    return s;
}

// ---------------- 分层配置仓库 ----------------

// 配置仓库：持有当前门面与快照，支持「全量重建」（默认层 → 文件 → 环境变量）。
// 重建后原子替换，并给出与上一版相比发生变化的键，供调用方决定怎么应用。
// 它只负责取值与比对，不负责应用——应用策略是服务的事（见 reload_and_apply）。
class ConfigStore
{
public:
    ConfigStore(const std::string& file_path, const std::string& env_prefix)
        : file_path_(file_path), env_prefix_(env_prefix), revision_(0)
    {
    }

    // 重建一次：默认层 → 文件层（存在才加载）→ 环境变量层
    std::vector<std::string> reload(bool* file_loaded, std::string* error)
    {
        std::shared_ptr<ConfigFacade> fresh(new ConfigFacade());
        apply_defaults(fresh.get());
        bool loaded = false;
        if (!file_path_.empty() && file_exists(file_path_)) {
            loaded = fresh->load_file(file_path_);
            if (!loaded && error != NULL) {
                *error = "配置文件解析失败: " + file_path_;
            }
        }
        fresh->set_env_prefix(env_prefix_);

        const std::vector<std::string> changed = diff_keys(fresh.get());
        std::vector<std::string> result;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            facade_ = fresh;
            settings_ = read_settings(*fresh);
            changed_keys_ = changed;
            ++revision_;
            result = changed_keys_;
        }
        if (file_loaded != NULL) {
            *file_loaded = loaded;
        }
        return result;
    }

    Settings settings() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return settings_;
    }

    std::shared_ptr<const ConfigFacade> facade() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return facade_;
    }

    std::vector<std::string> changed_keys() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return changed_keys_;
    }

    std::int64_t revision() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return revision_;
    }

    const std::string& file_path() const { return file_path_; }

private:
    // 与上一版逐键比对（键集合取并集），值不同即计入变化
    std::vector<std::string> diff_keys(const ConfigFacade* fresh)
    {
        std::vector<std::string> changed;
        std::shared_ptr<const ConfigFacade> previous;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            previous = facade_;
        }
        if (!previous) {
            return changed;  // 首轮加载：全部算「新增」反而吵，这里不报
        }
        std::vector<std::string> keys = fresh->keys();
        const std::vector<std::string> old_keys = previous->keys();
        for (std::size_t i = 0; i < old_keys.size(); ++i) {
            if (std::find(keys.begin(), keys.end(), old_keys[i]) == keys.end()) {
                keys.push_back(old_keys[i]);
            }
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (fresh->get(keys[i]) != previous->get(keys[i])) {
                changed.push_back(keys[i]);
            }
        }
        return changed;
    }

    std::string file_path_;
    std::string env_prefix_;
    mutable std::mutex mutex_;
    std::shared_ptr<ConfigFacade> facade_;
    Settings settings_;
    std::vector<std::string> changed_keys_;
    std::int64_t revision_;
};

// 文件监视：轮询 mtime，两次读到同一个值才算稳定（避免读到半截写入）。
// 配置热加载的最小可靠实现——对「编辑器原子替换一个文件」这类场景，
// 轮询 mtime 比事件通知更省事，也不会被临时文件刷屏。
class ConfigWatcher
{
public:
    ConfigWatcher(const std::string& path, int poll_ms)
        : path_(path), poll_ms_(poll_ms), running_(false), last_mtime_(0)
    {
    }

    ~ConfigWatcher() { stop(); }

    void set_on_change(std::function<void()> callback) { on_change_ = callback; }

    void start()
    {
        if (running_.exchange(true)) {
            return;
        }
        last_mtime_ = file_mtime_ms(path_);
        thread_ = std::thread([this]() { loop(); });
    }

    void stop()
    {
        if (!running_.exchange(false)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void loop()
    {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms_));
            const std::int64_t mtime = file_mtime_ms(path_);
            if (mtime == 0 || mtime == last_mtime_) {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            if (file_mtime_ms(path_) != mtime) {
                continue;  // 还在写，下一轮再看
            }
            last_mtime_ = mtime;
            if (on_change_) {
                on_change_();
            }
        }
    }

    std::string path_;
    int poll_ms_;
    std::atomic<bool> running_;
    std::int64_t last_mtime_;
    std::thread thread_;
    std::function<void()> on_change_;
};

// ---------------- 指标 ----------------

class ServiceMetrics
{
public:
    ServiceMetrics()
    {
        latency_ = registry_.histogram("svcframe_rpc_latency_ms",
                                       std::vector<double>{1, 5, 10, 25, 50, 100, 250, 500, 1000},
                                       "RPC 方法处理耗时（毫秒）");
        in_flight_ = registry_.gauge("svcframe_rpc_in_flight", "正在处理的 RPC 请求数");
        reloads_ = registry_.counter("svcframe_config_reloads_total", "配置热加载次数");
        uptime_ = registry_.gauge("svcframe_uptime_seconds", "进程已运行时长（秒）");
    }

    MetricRegistry& registry() { return registry_; }

    void record_call(const std::string& method, double duration_ms, bool ok)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        counter_for(calls_, "svcframe_rpc_calls_total", "RPC 方法调用次数", method)->inc();
        if (!ok) {
            counter_for(errors_, "svcframe_rpc_errors_total", "RPC 方法失败次数", method)->inc();
        }
        latency_->observe(duration_ms);
    }

    void set_in_flight(double value) { in_flight_->set(value); }
    void inc_reload() { reloads_->inc(); }
    void set_uptime(double seconds) { uptime_->set(seconds); }
    double reload_count() const { return reloads_->value(); }

private:
    // 标签值兜底清洗：方法名来自调用方，脏字符会破坏 Prometheus 文本格式
    static std::string sanitize(const std::string& raw)
    {
        std::string out;
        for (std::size_t i = 0; i < raw.size() && i < 64; ++i) {
            const char c = raw[i];
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            out.push_back(safe ? c : '_');
        }
        return out.empty() ? std::string("unknown") : out;
    }

    std::shared_ptr<MetricCounter>
    counter_for(std::map<std::string, std::shared_ptr<MetricCounter> >& map, const std::string& base,
                const std::string& help, const std::string& method)
    {
        std::map<std::string, std::shared_ptr<MetricCounter> >::iterator it = map.find(method);
        if (it != map.end()) {
            return it->second;
        }
        const std::string name = base + "{method=\"" + sanitize(method) + "\"}";
        std::shared_ptr<MetricCounter> metric = registry_.counter(name, help);
        map[method] = metric;
        return metric;
    }

    MetricRegistry registry_;
    std::shared_ptr<MetricHistogram> latency_;
    std::shared_ptr<MetricGauge> in_flight_;
    std::shared_ptr<MetricCounter> reloads_;
    std::shared_ptr<MetricGauge> uptime_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<MetricCounter> > calls_;
    std::map<std::string, std::shared_ptr<MetricCounter> > errors_;
};

// ---------------- RPC 方法 ----------------

// 方法体遵循 libmini RPC 的线上契约：**入参与返回值都是 JSON 文本**。
// 服务端把返回的文本解析后塞进 {"result": ...}，客户端拿到的就是这个 result
// 的 dump；入参则是客户端请求里 "params" 的原文（因此 echo 能逐字节回显）。
// 六个方法各演示一种典型处理：纯返回 / 回显 / 解析对象参数 / 抛异常（错误路径）/
// 阻塞（观察在途数与排空）/ 自述。
class RpcMethods
{
public:
    RpcMethods(ServiceMetrics* metrics, Tracer* tracer) : metrics_(metrics), tracer_(tracer) {}

    void register_on(RpcServer* server)
    {
        static const char* kMethods[] = {"ping", "echo", "add", "boom", "sleep", "stats"};
        for (std::size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); ++i) {
            const std::string method = kMethods[i];
            server->register_method(
                method, [this, method](const std::string& params) { return call(method, params); });
        }
    }

    // 统一入口：所有 RPC 都过这里 —— 指标与追踪一次做全，业务方法只写业务
    std::string call(const std::string& method, const std::string& params)
    {
        InFlightGuard guard(this);
        std::shared_ptr<Span> span = tracer_->start_span("rpc." + method);
        span->set_attribute("rpc.method", method);
        span->set_attribute("rpc.params_bytes", static_cast<long long>(params.size()));
        SpanScope scope(span);

        const std::int64_t started = app::now_ms();
        try {
            const std::string result = invoke(method, params);
            span->set_status(SpanStatus::Ok);
            metrics_->record_call(method, static_cast<double>(app::now_ms() - started), true);
            return result;
        } catch (const std::exception& ex) {
            span->set_status(SpanStatus::Error, ex.what());
            metrics_->record_call(method, static_cast<double>(app::now_ms() - started), false);
            throw;  // 交给 RpcServer 变成错误响应（客户端 last_error 可见）
        }
    }

    // 在途请求数（停机排空判断的依据；指标只是它的对外投影）
    int in_flight() const { return in_flight_.load(); }

    // 在线程池里运行的 handler 数量上限等启动期选项改不了，这里只用于展示
    std::size_t method_count() const { return 6; }

private:
    // 在途数同时进原子计数（排空判断）与指标（外部观测）
    struct InFlightGuard
    {
        explicit InFlightGuard(RpcMethods* owner) : owner_(owner)
        {
            owner_->metrics_->set_in_flight(static_cast<double>(++owner_->in_flight_));
        }
        ~InFlightGuard() { owner_->metrics_->set_in_flight(static_cast<double>(--owner_->in_flight_)); }
        RpcMethods* owner_;
    };

    std::string invoke(const std::string& method, const std::string& params)
    {
        if (method == "ping") {
            return to_json_string(JsonValue("pong"));
        }
        if (method == "echo") {
            ensure_json(params, "echo");  // 校验但不改动：逐字节回显
            return params;
        }
        if (method == "add") {
            return add(params);
        }
        if (method == "boom") {
            throw std::runtime_error("boom: " +
                                     (params.empty() ? std::string("(no reason)") : params));
        }
        if (method == "sleep") {
            return sleep_ms(params);
        }
        if (method == "stats") {
            JsonValue out = JsonValue::object();
            out["methods"] = static_cast<std::int64_t>(method_count());
            out["in_flight"] = static_cast<std::int64_t>(in_flight_.load());
            return to_json_string(out);
        }
        throw std::runtime_error("unknown method: " + method);
    }

    // 入参必须是合法 JSON（客户端已保证，手工 curl 时未必）：这一步让错误
    // 变成清晰的 InvalidArgument 而不是后续解析处的莫名其妙的异常
    static JsonValue ensure_json(const std::string& params, const std::string& method)
    {
        try {
            return parse_json(params);
        } catch (const std::exception&) {
            throw std::invalid_argument(method + " 的参数必须是合法 JSON 文本：{\"a\":2,\"b\":3}");
        }
    }

    // 取数值字段：整数按整数算（返回 {"sum":5} 而不是 5.0），浮点退回浮点
    static std::string add(const std::string& params)
    {
        const JsonValue node = ensure_json(params, "add");
        if (!node.is_object() || !node.contains("a") || !node.contains("b")) {
            throw std::invalid_argument("add 需要 JSON 对象参数：{\"a\":2,\"b\":3}");
        }
        if (!node["a"].is_number() || !node["b"].is_number()) {
            throw std::invalid_argument("add 的 a/b 必须是数字");
        }
        JsonValue out = JsonValue::object();
        if (node["a"].is_number_integer() && node["b"].is_number_integer()) {
            out["sum"] = node["a"].get<long long>() + node["b"].get<long long>();
        } else {
            out["sum"] = node["a"].get<double>() + node["b"].get<double>();
        }
        return to_json_string(out);
    }

    static std::string sleep_ms(const std::string& params)
    {
        const JsonValue node = ensure_json(params, "sleep");
        if (!node.is_object() || !node.contains("ms") || !node["ms"].is_number_integer()) {
            throw std::invalid_argument("sleep 需要 JSON 对象参数：{\"ms\":300}");
        }
        int ms = node["ms"].get<int>();
        if (ms < 0) {
            throw std::invalid_argument("sleep 的 ms 不能为负");
        }
        if (ms > 5000) {
            ms = 5000;  // 上限保护：骨架不欢迎超长占用
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        JsonValue out = JsonValue::object();
        out["slept_ms"] = ms;
        return to_json_string(out);
    }

    ServiceMetrics* metrics_;
    Tracer* tracer_;
    std::atomic<int> in_flight_{0};
};

// ---------------- 控制面（HTTP） ----------------

const char* kControlPage =
    "<!doctype html><html lang=\"zh\"><meta charset=\"utf-8\"><title>svcframe</title>"
    "<style>body{font:14px/1.5 system-ui,Segoe UI,sans-serif;margin:0;padding:24px;"
    "background:#12141a;color:#d8dbe6}h1{font-size:18px;margin:0 0 4px}"
    ".sub{color:#8b93a7;margin-bottom:16px}button{background:#2b6cb0;color:#fff;border:0;"
    "border-radius:6px;padding:7px 12px;margin-right:8px;cursor:pointer}"
    "button:hover{background:#3182ce}"
    "pre{background:#0c0e13;border:1px solid #232733;border-radius:8px;padding:12px;"
    "overflow:auto;max-height:320px}section{margin-bottom:18px}"
    "h2{font-size:13px;color:#8b93a7;margin:0 0 8px;text-transform:uppercase;"
    "letter-spacing:.06em}</style>"
    "<h1>svcframe 控制台</h1><div class=\"sub\" id=\"meta\">加载中…</div>"
    "<section><h2>调用</h2>"
    "<button onclick=\"inv('ping')\">ping</button>"
    "<button onclick=\"inv('echo',{k:'v',n:[1,2]})\">echo</button>"
    "<button onclick=\"inv('add',{a:2,b:3})\">add</button>"
    "<button onclick=\"inv('boom',{reason:'演示失败'})\">boom</button>"
    "<button onclick=\"inv('sleep',{ms:300})\">sleep 300</button>"
    "<button onclick=\"reload()\">热加载配置</button>"
    "<pre id=\"out\">（点一个按钮）</pre></section>"
    "<section><h2>配置（含来源）</h2><pre id=\"cfg\"></pre></section>"
    "<section><h2>指标</h2><pre id=\"metrics\"></pre></section>"
    "<section><h2>追踪（最近完成的 span）</h2><pre id=\"traces\"></pre></section>"
    "<script>"
    "function $(id){return document.getElementById(id);}"
    "function post(url,body){return fetch(url,{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):'{}'})"
    ".then(function(r){return r.json();});}"
    "function inv(method,params){var p=JSON.stringify(params===undefined?null:params);"
    "post('/api/rpc',{method:method,params:p})"
    ".then(function(d){$('out').textContent=JSON.stringify(d,null,2);refresh();});}"
    "function reload(){$('out').textContent='热加载中…';post('/api/reload')"
    ".then(function(d){$('out').textContent=JSON.stringify(d,null,2);refresh();});}"
    "function refresh(){fetch('/api/config').then(function(r){return r.json();})"
    ".then(function(d){$('meta').textContent='配置版本 r'+d.revision+' · 已热加载 '+d.reloads"
    "+' 次 · 文件 '+d.file;$('cfg').textContent=JSON.stringify(d.keys,null,2);});"
    "fetch('/metrics').then(function(r){return r.text();})"
    ".then(function(t){$('metrics').textContent=t;});"
    "fetch('/api/traces').then(function(r){return r.json();})"
    ".then(function(t){$('traces').textContent=JSON.stringify(t,null,2);});}"
    "refresh();setInterval(refresh,2000);</script></html>";

// 控制面：把 /metrics、/api/config、/api/traces 与一个「本机自测」/api/rpc 摆出来。
// /api/rpc 走真实的 RpcClient 往返（不是直接调方法），因此控制台里看到的结果与
// 外部客户端完全同一条路径。
class ControlApi
{
public:
    ControlApi(ConfigStore* store, ServiceMetrics* metrics, Tracer* tracer, const std::string& host,
               int rpc_port)
        : store_(store), metrics_(metrics), tracer_(tracer), host_(host), rpc_port_(rpc_port),
          started_ms_(app::now_ms()), ready_(NULL)
    {
    }

    void set_reload_handler(std::function<std::vector<std::string>()> handler)
    {
        on_reload_ = handler;
    }

    void register_routes(HttpServer* server, std::atomic<bool>* ready)
    {
        ready_ = ready;
        server->get("/", [this](const HttpRequest&) { return page(); });
        server->get("/metrics", [this](const HttpRequest&) { return prometheus(); });
        server->get("/api/config", [this](const HttpRequest&) { return config(); });
        server->get("/api/traces", [this](const HttpRequest&) { return traces(); });
        server->post("/api/rpc", [this](const HttpRequest& req) { return rpc_call(req); });
        server->post("/api/reload", [this](const HttpRequest&) { return reload(); });
        server->add_readiness_check("rpc", [this]() { return readiness(); });
    }

private:
    HttpReply page()
    {
        HttpReply reply = HttpReply::text(200, kControlPage);
        reply.headers["Content-Type"] = "text/html; charset=utf-8";
        return reply;
    }

    HttpReply prometheus()
    {
        metrics_->set_uptime(static_cast<double>(app::now_ms() - started_ms_) / 1000.0);
        return HttpReply::text(200, metrics_->registry().render_prometheus());
    }

    std::string readiness() const
    {
        if (ready_ != NULL && !ready_->load()) {
            return "正在优雅停机（已撤流量）";
        }
        return std::string();
    }

    HttpReply config()
    {
        std::shared_ptr<const ConfigFacade> cfg = store_->facade();
        JsonValue root = JsonValue::object();
        root["file"] = store_->file_path().empty() ? std::string("(无)") : store_->file_path();
        root["revision"] = store_->revision();
        root["reloads"] = static_cast<std::int64_t>(metrics_->reload_count());
        root["changed_keys"] = json_keys(store_->changed_keys());
        JsonValue keys = JsonValue::object();
        if (cfg) {
            const std::vector<std::string> names = cfg->keys();
            for (std::size_t i = 0; i < names.size(); ++i) {
                JsonValue entry = JsonValue::object();
                entry["value"] = cfg->get(names[i]);
                entry["source"] = cfg->source_of(names[i]);  // default / file / env
                keys[names[i]] = entry;
            }
        }
        root["keys"] = keys;
        return app::json_ok(root);
    }

    HttpReply traces()
    {
        JsonValue root = JsonValue::object();
        root["count"] = static_cast<std::int64_t>(tracer_->finished_count());
        root["dropped"] = static_cast<std::int64_t>(tracer_->dropped_count());
        root["spans"] = parse_json(tracer_->finished_json());
        return app::json_ok(root);
    }

    HttpReply rpc_call(const HttpRequest& req)
    {
        JsonValue body = JsonValue::object();
        if (!req.body.empty()) {
            try {
                body = parse_json(req.body);
            } catch (const std::exception& ex) {
                return app::json_error(400, std::string("请求体不是合法 JSON: ") + ex.what());
            }
        }
        const std::string method = app::jstr(body, "method");
        if (method.empty()) {
            return app::json_error(400, "缺少 method");
        }
        // params 按 RPC 线上契约传「JSON 文本」：字符串直接用，对象/数组则序列化
        std::string params = "null";
        if (body.is_object() && body.contains("params")) {
            params = body["params"].is_string() ? body["params"].get<std::string>()
                                                : to_json_string(body["params"]);
        }
        try {
            parse_json(params);
        } catch (const std::exception&) {
            return app::json_error(400, "params 必须是合法 JSON 文本");
        }
        return app::json_ok(call_rpc(method, params));
    }

    // 同一个进程里既当服务端又当客户端：最省事也最诚实的自测
    JsonValue call_rpc(const std::string& method, const std::string& params)
    {
        RpcClient client(host_, rpc_port_);
        const std::int64_t started = app::now_ms();
        const std::string result = client.call(method, params);
        JsonValue node = JsonValue::object();
        node["method"] = method;
        node["params"] = params;
        // 注意别用 "ok" 这个名字：app::json_ok 会给每个响应盖上 "ok": true。
        node["call_ok"] = client.last_error() == RpcError::OK;
        node["result"] = result;
        if (client.last_error() != RpcError::OK) {
            node["error"] = client.last_error_message();
        }
        node["latency_ms"] = static_cast<std::int64_t>(app::now_ms() - started);
        return node;
    }

    HttpReply reload()
    {
        if (!on_reload_) {
            return app::json_error(503, "热加载不可用（未配置监听文件）");
        }
        const std::vector<std::string> changed = on_reload_();
        JsonValue root = JsonValue::object();
        root["revision"] = store_->revision();
        root["changed"] = json_keys(changed);
        return app::json_ok(root);
    }

    static JsonValue json_keys(const std::vector<std::string>& keys)
    {
        JsonValue out = JsonValue::array();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            out.push_back(keys[i]);
        }
        return out;
    }

    ConfigStore* store_;
    ServiceMetrics* metrics_;
    Tracer* tracer_;
    std::string host_;
    int rpc_port_;
    std::int64_t started_ms_;
    std::atomic<bool>* ready_;
    std::function<std::vector<std::string>()> on_reload_;
};

// ---------------- 配置重载：取值 → 应用 ----------------

// 一次重载的完整动作：重建配置 → 应用的运行期可改项 → 记账 → 打日志。
// 启动期专属项（并发上限、工作线程）只记录不应用，并在日志里说明需要重启——
// 这比悄悄忽略要诚实，也比假装支持热改安全。
std::vector<std::string> reload_and_apply(ConfigStore* store, RpcServer* rpc,
                                         ServiceMetrics* metrics, bool verbose)
{
    std::string error;
    bool file_loaded = false;
    const std::vector<std::string> changed = store->reload(&file_loaded, &error);
    const Settings settings = store->settings();

    LogFacade::set_level(parse_log_level(settings.log_level));  // 运行期可改
    if (rpc != NULL) {
        rpc->apply_config(*store->facade(), "rpc.");  // 运行期可改项（队列/重试/超时）立即生效
    }
    if (metrics != NULL) {
        metrics->inc_reload();
    }
    if (spdlog::logger* log = LogFacade::logger()) {
        if (!error.empty()) {
            log->warn("配置加载有问题: {}", error);
        }
        log->info("配置重载完成：revision={} 变化键={} 日志级别={} 需重启才能生效的项={}",
                  store->revision(), changed.size(), settings.log_level,
                  "rpc.max_in_flight/worker_threads");
    }
    if (verbose) {
        for (std::size_t i = 0; i < changed.size(); ++i) {
            std::cout << "  配置变更: " << changed[i] << "\n";
        }
    }
    return changed;
}

// ---------------- demo 小工具 ----------------

// 重载一次并打印变化键（demo 用；服务里走 reload_and_apply）
void storage_reload(ConfigStore& store, const char* label)
{
    std::string error;
    bool file_loaded = false;
    const std::vector<std::string> changed = store.reload(&file_loaded, &error);
    std::cout << "  [" << label << "] revision=" << store.revision()
              << " 文件已加载=" << (file_loaded ? "是" : "否")
              << " 变化键=" << changed.size();
    if (!error.empty()) {
        std::cout << " 错误=" << error;
    }
    std::cout << "\n";
}

// ---------------- 优雅停机 ----------------

// 顺序很重要：先撤存活/就绪（外部不再送新请求进来）→ 等在途数归零（或到窗口）
// → 停 RPC → 停控制面。控制面最后停，停机过程本身仍然可观测。
void graceful_shutdown(RpcServer* rpc, HttpServer* http, RpcMethods* methods,
                       std::atomic<bool>* ready, int drain_ms)
{
    std::cout << "\n[停机] 撤流量（存活/就绪转为 503），排空窗口 " << drain_ms << "ms\n";
    ready->store(false);
    const std::int64_t deadline = app::now_ms() + (drain_ms > 0 ? drain_ms : 0);
    while (methods != NULL && methods->in_flight() > 0 && app::now_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (methods != NULL && methods->in_flight() > 0) {
        std::cout << "[停机] 排空窗口内仍有 " << methods->in_flight() << " 个在途请求，放弃等待\n";
    }
    if (rpc != NULL) {
        rpc->stop();
    }
    if (http != NULL) {
        http->stop();
    }
    std::cout << "[停机] 完成\n";
}

// ---------------- 演示（自检） ----------------

const char* kDemoConfig =
    "[service]\n"
    "name = svcframe-demo\n"
    "http_port = 0\n"
    "\n"
    "[rpc]\n"
    "port = 0\n"
    "host = 127.0.0.1\n"
    "max_in_flight = 8\n"
    "queue_wait_ms = 10000\n"
    "drain_timeout_ms = 1500\n"
    "\n"
    "[log]\n"
    "level = debug\n";

int run_demo()
{
    DemoReport report("svcframe 自检（配置分层 / 热加载 / 指标 / 追踪 / 健康 / 优雅停机）");
    const std::string workspace = app::make_workspace("svcframe_demo");
    if (workspace.empty()) {
        report.check(false, "创建临时工作目录");
        return report.finish();
    }
    const std::string config_path = app::workspace_file(workspace, "svc.ini");
    const bool written = write_file_atomic(config_path, kDemoConfig);
    report.check(written, "写出 INI 配置文件");

    // ---- 1) 分层配置：默认层 / 文件层 / 环境变量层 ----
    ConfigStore store(config_path, kEnvPrefix);
    storage_reload(store, "首次加载");
    Settings settings = store.settings();
    report.check(settings.max_in_flight == 8 && settings.log_level == "debug",
                 "文件层覆盖默认层（max_in_flight=8, log.level=debug）");

    std::shared_ptr<const ConfigFacade> facade = store.facade();
    report.check(facade && facade->source_of("rpc.max_in_flight") == "file",
                 "source_of(rpc.max_in_flight) == file");
    report.check(facade && facade->source_of("service.http_port") == "file",
                 "source_of(service.http_port) == file");
    report.check(facade && facade->source_of("rpc.drain_timeout_ms") == "file",
                 "source_of(rpc.drain_timeout_ms) == file");
    report.check(facade && facade->source_of("rpc.host") == "file",
                 "source_of(rpc.host) == file");

    // 环境变量层优先于文件层
    env_set("SVCFRAME_RPC_MAX_IN_FLIGHT", "3");
    storage_reload(store, "环境变量覆盖");
    settings = store.settings();
    facade = store.facade();
    report.check(settings.max_in_flight == 3 && facade &&
                     facade->source_of("rpc.max_in_flight") == "env",
                 "环境变量层压过文件层（SVCFRAME_RPC_MAX_IN_FLIGHT=3 → source=env）");
    env_remove("SVCFRAME_RPC_MAX_IN_FLIGHT");
    storage_reload(store, "移除环境变量");
    settings = store.settings();
    report.check(settings.max_in_flight == 8, "移除环境变量后回落到文件层（8）");

    // ---- 2) 起服务：RPC 面 + 控制面 ----
    ServiceMetrics metrics;
    Tracer tracer(settings.name, 256);
    std::atomic<bool> ready(true);

    RpcServer rpc(settings.rpc_port);
    rpc.set_logger(LogFacade::logger());
    rpc.apply_config(*store.facade(), "rpc.");  // 端口/地址/并发上限/排空窗口都从配置来
    RpcMethods methods(&metrics, &tracer);
    methods.register_on(&rpc);
    rpc.start_background();
    const bool rpc_ready = rpc.wait_until_ready(5000);
    report.check(rpc_ready && rpc.is_running() && rpc.port() > 0,
                 "RPC 服务启动并自动分配端口 " + lexical_cast<std::string>(rpc.port()));
    if (!rpc_ready) {
        remove_tree(workspace);
        return report.finish();
    }

    ControlApi api(&store, &metrics, &tracer, settings.rpc_host, rpc.port());
    api.set_reload_handler([&store, &rpc, &metrics]() {
        return reload_and_apply(&store, &rpc, &metrics, false);
    });

    HttpServer control;
    control.enable_health_endpoints();
    // 存活探针翻转 503 = 让负载均衡/K8s 摘流量；这是优雅停机的第一步
    control.set_liveness_handler([&ready](const HttpRequest&) {
        return ready.load() ? HttpReply::text(200, "ok")
                            : HttpReply::text(503, "draining");
    });
    api.register_routes(&control, &ready);
    const bool control_ready = control.start_background(0) && control.wait_until_ready(5000);
    report.check(control_ready, "控制面启动并自动分配端口");
    if (!control_ready) {
        rpc.stop();
        remove_tree(workspace);
        return report.finish();
    }
    const std::string base =
        "http://" + settings.rpc_host + ":" + lexical_cast<std::string>(control.port());
    report.info("控制台: " + base + "/");
    HttpClient http(base);

    // ---- 3) RPC 往返：成功 / 参数解析 / 错误路径 ----
    RpcClient client(settings.rpc_host, rpc.port());
    const std::string ping = client.call("ping", "null");
    if (ping != "\"pong\"") {
        report.info("ping 失败原因: " + client.last_error_message());
    }
    report.check(ping == "\"pong\"", "ping → \"pong\"（返回值是 JSON 文本）");
    const std::string echo = client.call("echo", R"({"k":"v","n":[1,2]})");
    report.check(echo == R"({"k":"v","n":[1,2]})", "echo 逐字节回显对象参数");

    const std::string sum = client.call("add", R"({"a":2,"b":3})");
    bool sum_ok = false;
    if (client.last_error() == RpcError::OK) {
        try {
            sum_ok = parse_json(sum)["sum"].get<int>() == 5;
        } catch (const std::exception&) {
        }
    }
    report.check(sum_ok, "add {\"a\":2,\"b\":3} → {\"sum\":5}");

    const std::string bad = client.call("add", R"({"a":1})");
    report.check(bad.empty() && client.last_error() != RpcError::OK &&
                     client.last_error_message().find("add 需要") != std::string::npos,
                 "参数缺字段 → 业务错误（原因可读）");

    const std::string boom = client.call("boom", R"({"reason":"演示"})");
    report.check(boom.empty() && client.last_error() != RpcError::OK &&
                     client.last_error_message().find("boom") != std::string::npos,
                 "boom 走错误路径（last_error 非 OK，原因含 boom）");

    // ---- 4) 指标 ----
    Result<HttpResponse> prometheus = http.try_get("/metrics");
    const std::string text = prometheus.ok() ? prometheus.value().body : std::string();
    report.check(prometheus.ok() && text.find("svcframe_rpc_calls_total{method=\"ping\"}") !=
                                       std::string::npos,
                 "/metrics 输出按方法分列的调用计数");
    report.check(text.find("svcframe_rpc_errors_total{method=\"boom\"}") != std::string::npos,
                 "/metrics 输出失败计数（boom）");
    report.check(text.find("svcframe_rpc_latency_ms_bucket") != std::string::npos &&
                     text.find("svcframe_rpc_latency_ms_count") != std::string::npos,
                 "/metrics 输出耗时直方图（bucket + count）");
    report.check(text.find("svcframe_config_reloads_total") != std::string::npos,
                 "/metrics 输出热加载次数");

    // ---- 5) 追踪 ----
    Result<HttpResponse> traces = http.try_get("/api/traces");
    bool traces_ok = false;
    if (traces.ok()) {
        try {
            const JsonValue doc = parse_json(traces.value().body);
            traces_ok = doc["count"].get<std::int64_t>() >= 4 && doc["spans"].is_array() &&
                        traces.value().body.find("\"rpc.ping\"") != std::string::npos &&
                        traces.value().body.find("\"rpc.boom\"") != std::string::npos;
        } catch (const std::exception&) {
        }
    }
    report.check(traces_ok, "/api/traces 导出已完成的 span（含 rpc.ping 与失败的 rpc.boom）");

    // ---- 6) /api/config 展示来源 ----
    Result<HttpResponse> config_reply = http.try_get("/api/config");
    bool config_ok = false;
    if (config_reply.ok()) {
        try {
            const JsonValue doc = parse_json(config_reply.value().body);
            config_ok = doc["file"].get<std::string>() == config_path &&
                        doc["keys"]["rpc.max_in_flight"]["value"].get<std::string>() == "8" &&
                        doc["keys"]["rpc.max_in_flight"]["source"].get<std::string>() == "file";
        } catch (const std::exception&) {
        }
    }
    report.check(config_ok, "/api/config 列出每个键的值与来源（file/default/env）");

    // ---- 7) /api/rpc：控制台走真实 RPC 往返 ----
    Result<HttpResponse> api_call = http.try_post_json(
        "/api/rpc", std::string("{\"method\":\"add\",\"params\":\"{\\\"a\\\":40,\\\"b\\\":2}\"}"));
    bool api_call_ok = false;
    if (api_call.ok()) {
        try {
            const JsonValue doc = parse_json(api_call.value().body);
            api_call_ok = doc["call_ok"].get<bool>() &&
                          parse_json(doc["result"].get<std::string>())["sum"].get<int>() == 42;
        } catch (const std::exception&) {
        }
    }
    report.check(api_call_ok, "POST /api/rpc 经真实 RPC 客户端往返得到 42");

    // 失败必须被如实上报：call_ok=false + error 文本（"ok" 是响应封装的保留键）
    Result<HttpResponse> api_fail = http.try_post_json(
        "/api/rpc", std::string("{\"method\":\"boom\",\"params\":\"null\"}"));
    bool api_fail_ok = false;
    if (api_fail.ok()) {
        try {
            const JsonValue doc = parse_json(api_fail.value().body);
            api_fail_ok = doc["call_ok"].is_boolean() && !doc["call_ok"].get<bool>() &&
                          doc["error"].get<std::string>().find("boom") != std::string::npos;
        } catch (const std::exception&) {
        }
    }
    report.check(api_fail_ok, "POST /api/rpc 如实上报失败（call_ok=false + error 原因）");

    // ---- 8) 热加载：改文件即生效 ----
    ConfigWatcher watcher(config_path, 100);
    watcher.set_on_change([&store, &rpc, &metrics]() {
        reload_and_apply(&store, &rpc, &metrics, false);
    });
    const std::int64_t before_revision = store.revision();
    const double before_reloads = metrics.reload_count();
    watcher.start();

    // 原子替换配置文件：只改运行期可改的三项（日志级别 / 队列等待 / 重试等待）
    const std::string updated =
        "[service]\n"
        "name = svcframe-demo\n"
        "http_port = 0\n"
        "\n"
        "[rpc]\n"
        "port = 0\n"
        "host = 127.0.0.1\n"
        "max_in_flight = 8\n"
        "queue_wait_ms = 250\n"
        "retry_after_seconds = 4\n"
        "drain_timeout_ms = 1500\n"
        "\n"
        "[log]\n"
        "level = warn\n";
    const bool rewritten = write_file_atomic(config_path, updated);
    const std::int64_t deadline = app::now_ms() + 6000;
    while (store.revision() == before_revision && app::now_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    report.check(rewritten && store.revision() > before_revision &&
                     store.settings().log_level == "warn",
                 "文件变化被监视线程捕获并重载（log.level → warn）");
    report.check(rpc.config().retry_after_seconds == 4,
                 "运行期可改项立即生效（rpc.retry_after_seconds → 4）");
    report.check(rpc.config().queue_wait_ms == 250,
                 "运行期可改项立即生效（rpc.queue_wait_ms → 250）");
    report.check(metrics.reload_count() > before_reloads,
                 "热加载次数进入指标（svcframe_config_reloads_total 增长）");
    const std::vector<std::string> changed = store.changed_keys();
    report.check(!changed.empty(), "重载报告出变化的键（" +
                                       lexical_cast<std::string>(changed.size()) + " 个）");
    watcher.stop();

    // ---- 9) 健康检查 ----
    Result<HttpResponse> health = http.try_get("/healthz");
    Result<HttpResponse> readyz = http.try_get("/readyz");
    report.check(health.ok() && health.value().status == 200 && readyz.ok() &&
                     readyz.value().status == 200,
                 "存活/就绪探针均 200");
    ready.store(false);
    const Result<HttpResponse> health_down = http.try_get("/healthz");
    const Result<HttpResponse> readyz_down = http.try_get("/readyz");
    report.check(health_down.ok() && health_down.value().status == 503 &&
                     readyz_down.ok() && readyz_down.value().status != 200,
                 "撤流量后 /healthz 503 且 /readyz 不就绪");
    ready.store(true);
    report.check(http.try_get("/healthz").value().status == 200, "恢复就绪后 /healthz 回到 200");

    // ---- 10) 优雅停机：等在途请求跑完 ----
    std::string slow_result;
    std::atomic<bool> slow_done(false);
    RpcClient slow_client(settings.rpc_host, rpc.port());
    std::thread slow([&slow_client, &slow_result, &slow_done]() {
        slow_result = slow_client.call("sleep", R"({"ms":300})");
        slow_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    graceful_shutdown(&rpc, &control, &methods, &ready, settings.drain_timeout_ms);
    slow.join();
    bool slow_ok = false;
    try {
        slow_ok = parse_json(slow_result)["slept_ms"].get<int>() == 300;
    } catch (const std::exception&) {
    }
    report.check(slow_done.load() && slow_ok, "排空窗口内让在途的 sleep 请求正常跑完");
    RpcClient after(settings.rpc_host, rpc.port());
    after.call("ping", "");
    report.check(!rpc.is_running() && after.last_error() != RpcError::OK,
                 "停机后 RPC 不再接受请求（连接失败）");

    report.info("演示工作目录: " + workspace);
    remove_tree(workspace);
    return report.finish();
}

// ---------------- 原生窗口模式 ----------------

// `--ui`：把本应用的服务拉起来（子进程，与启动器托管同一套机制），再开一个
// 原生窗口盯着它的 /api/config、/api/traces 与 /metrics，并提供三个动作按钮。
// selftest = true 时跑满固定帧数、截图并打印结论（CI 用，无需人眼看窗口）。
int run_ui(const std::string& argv0, const int rpc_port, const int http_port, bool selftest)
{
    ui::AppWindowSpec spec;
    spec.title = "svcframe 控制台";
    spec.subtitle = "分层配置 / 追踪 / 指标 / RPC 动作";
    spec.port = http_port;
    spec.child_args.push_back(app::self_exe(argv0));
    spec.child_args.push_back("--port");
    spec.child_args.push_back(lexical_cast<std::string>(rpc_port));
    spec.child_args.push_back("--http-port");
    spec.child_args.push_back(lexical_cast<std::string>(http_port));
    spec.child_args.push_back("serve");

    spec.panel.json_views.push_back("/api/config");
    spec.panel.json_views.push_back("/api/traces");
    spec.panel.metric_filters.push_back("svcframe_rpc_calls_total");
    spec.panel.metric_filters.push_back("svcframe_rpc_errors_total");
    spec.panel.metric_filters.push_back("svcframe_rpc_in_flight");
    spec.panel.metric_filters.push_back("svcframe_config_reloads_total");

    ui::PanelAction reload;
    reload.label = "热加载配置";
    reload.method = "POST";
    reload.path = "/api/reload";
    reload.body = "{}";
    spec.panel.actions.push_back(reload);

    ui::PanelAction ping;
    ping.label = "RPC ping";
    ping.method = "POST";
    ping.path = "/api/rpc";
    ping.body = "{\"method\":\"ping\",\"params\":\"null\"}";
    spec.panel.actions.push_back(ping);

    ui::PanelAction add;
    add.label = "RPC add 2+3";
    add.method = "POST";
    add.path = "/api/rpc";
    add.body = "{\"method\":\"add\",\"params\":\"{\\\"a\\\":2,\\\"b\\\":3}\"}";
    spec.panel.actions.push_back(add);

    if (selftest) {
        spec.frames = 30;
        spec.report = true;
        spec.shot_path = app::workspace_file(app::make_workspace("svcframe_ui"), "window.bmp");
    }

    std::string error;
    const int code = ui::run_app_window(spec, &error);
    if (code == 2) {
        std::cerr << "无法打开原生窗口（" << error << "），请改用 `svcframe serve`。\n";
    }
    return code;
}

// ---------------- 正常模式 ----------------

// 一次性调用：给脚本/调试用，也让「命令行客户端」这条路径有真实出口
int run_call(const std::vector<std::string>& rest, const Settings& settings)
{
    if (rest.size() < 2) {
        std::cerr << "用法: svcframe call <方法> [参数...] [--port N]\n";
        return 2;
    }
    const std::string method = rest[1];
    // 参数按 JSON 文本拼起来（调用方自己给引号），不给就是 null
    std::string params = "null";
    if (rest.size() > 2) {
        params.clear();
        for (std::size_t i = 2; i < rest.size(); ++i) {
            params += rest[i];
        }
    }
    try {
        parse_json(params);
    } catch (const std::exception&) {
        std::cerr << "参数必须是合法 JSON 文本，例如: svcframe call add '{\"a\":2,\"b\":3}'\n";
        return 2;
    }
    RpcClient client(settings.rpc_host, settings.rpc_port);
    const std::string result = client.call(method, params);
    if (client.last_error() != RpcError::OK) {
        std::cerr << "调用失败(" << static_cast<int>(client.last_error())
                  << "): " << client.last_error_message() << "\n";
        return 1;
    }
    std::cout << result << "\n";
    return 0;
}

int run_service(const Settings& initial, ConfigStore* store)
{
    LogFacade::Options log_options;
    log_options.level = parse_log_level(initial.log_level);
    log_options.file_path = initial.log_file;
    log_options.console = true;
    LogFacade::init(log_options);

    ServiceMetrics metrics;
    Tracer tracer(initial.name, 2048);
    std::atomic<bool> ready(true);

    RpcServer rpc(initial.rpc_port);
    rpc.set_logger(LogFacade::logger());
    // 端口优先级：命令行 > 配置文件 > 默认值。apply_config 以门面里的端口为准，
    // 所以先把「最终决议的端口」回写成文件层的一个键，再整包灌给服务器——
    // 这样其余键（地址/并发上限/排空窗口/队列）仍然享受 apply_config 的一键接线。
    // （注：按门面规则，环境变量仍高于这一切。）
    ConfigFacade effective = *store->facade();
    effective.load_text("{\"rpc.port\": " + lexical_cast<std::string>(initial.rpc_port) + "}",
                        "json");
    rpc.apply_config(effective, "rpc.");
    RpcMethods methods(&metrics, &tracer);
    methods.register_on(&rpc);
    rpc.start_background();
    if (!rpc.wait_until_ready(5000)) {
        std::cerr << "RPC 服务启动失败\n";
        LogFacade::shutdown();
        return 1;
    }

    ControlApi api(store, &metrics, &tracer, initial.rpc_host, rpc.port());
    api.set_reload_handler([store, &rpc, &metrics]() {
        return reload_and_apply(store, &rpc, &metrics, false);
    });

    HttpServer control;
    control.enable_health_endpoints();
    control.set_liveness_handler([&ready](const HttpRequest&) {
        return ready.load() ? HttpReply::text(200, "ok") : HttpReply::text(503, "draining");
    });
    api.register_routes(&control, &ready);
    if (!control.start_background(initial.http_port) || !control.wait_until_ready(5000)) {
        std::cerr << "控制面启动失败\n";
        rpc.stop();
        LogFacade::shutdown();
        return 1;
    }

    // 配置文件存在才起监视线程；没有配置文件时 /api/reload 仍可手动触发
    ConfigWatcher watcher(store->file_path(), 150);
    if (!store->file_path().empty()) {
        watcher.set_on_change([store, &rpc, &metrics]() {
            reload_and_apply(store, &rpc, &metrics, true);
        });
        watcher.start();
    }

    const std::string rpc_addr = "http://" + initial.rpc_host + ":" +
                                 lexical_cast<std::string>(rpc.port()) + "/";
    const std::string control_addr = "http://" + initial.rpc_host + ":" +
                                     lexical_cast<std::string>(control.port()) + "/";
    app::print_banner(initial.name + " 已启动",
                      "RPC " + rpc_addr + " · 控制台 " + control_addr +
                          " · 配置 " +
                          (store->file_path().empty() ? std::string("(无文件，仅默认层)")
                                                      : store->file_path()));
    std::cout << "按 Ctrl+C 优雅停机（撤流量 → 排空 → 停服）\n";

    ConsoleExit console;
    console.set_handler([&]() {
        graceful_shutdown(&rpc, &control, &methods, &ready, initial.drain_timeout_ms);
    });
    console.wait();  // 等到回调执行完毕再退出

    watcher.stop();
    LogFacade::shutdown();
    return 0;
}

int run_cli(Args& args)
{
    const std::string config_path = trim(args.get_string("config"));
    ConfigStore store(config_path, kEnvPrefix);
    std::string error;
    bool file_loaded = false;
    store.reload(&file_loaded, &error);
    if (!error.empty()) {
        std::cerr << error << "\n";
    }
    Settings settings = store.settings();
    // 命令行 > 配置文件 > 默认值：端口这种部署差异更适合命令行临时改
    if (args.get_int("port") > 0) {
        settings.rpc_port = args.get_int("port");
    }
    if (args.get_int("http-port") > 0) {
        settings.http_port = args.get_int("http-port");
    }

    const std::vector<std::string> rest = args.remaining();
    const std::string command = rest.empty() ? std::string("serve") : to_lower(rest[0]);
    if (command == "call") {
        return run_call(rest, settings);
    }
    if (command != "serve") {
        std::cerr << "未知命令: " << command << "（可用 serve / call / demo）\n";
        return 2;
    }
    return run_service(settings, &store);
}

}  // namespace

int main(int argc, char** argv)
{
    Args args("svcframe", "1.0", "libmini 应用：RPC 服务骨架（分层配置 / 热加载 / 指标 / 追踪 / 优雅停机）");
    args.add_flag("demo", "", "自检模式：走完配置分层/热加载/指标/追踪/健康/停机");
    args.add_option("config", "c", "INI 配置文件路径", std::string(""));
    args.add_int("port", "", "RPC 端口（覆盖配置，默认 0 = 自动分配）", 0);
    args.add_int("http-port", "", "控制面端口（覆盖配置）", 0);
    args.add_flag("ui", "", "打开原生窗口（控制台面板，Windows）");
    args.add_flag("ui-selftest", "", "窗口自检：限帧渲染 + 截图 + 结论（CI 用）");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }
    if (args.has_flag("demo")) {
        return run_demo();
    }
    if (args.has_flag("ui") || args.has_flag("ui-selftest")) {
        // 窗口模式固定端口，便于子进程与面板对上：RPC 默认 8831、控制面 8832
        const int rpc_port = args.get_int("port") > 0 ? args.get_int("port") : 8831;
        const int http_port = args.get_int("http-port") > 0 ? args.get_int("http-port") : 8832;
        const std::string argv0 = (argc > 0 && argv[0] != 0) ? std::string(argv[0])
                                                             : std::string("svcframe");
        return run_ui(argv0, rpc_port, http_port, args.has_flag("ui-selftest"));
    }
    return run_cli(args);
}
