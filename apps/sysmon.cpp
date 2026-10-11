// sysmon —— libmini 应用示例 2：本机观测台
//
// 一个常驻采集器：按间隔采集 CPU / 内存 / 磁盘余量，历史落 SQLite，实时值同步
// 到 MetricRegistry（`/metrics` 直接输出 Prometheus 文本），网页看板画趋势，
// 越过阈值就记一条告警（`/api/alerts`）。只用本机数据，不需要任何外部组件。
//
// 用法：
//   sysmon --demo                                  自检：快采样几轮并汇总
//   sysmon --interval 1s --port 8790               正常模式（Ctrl+C 退出）
//   sysmon --alert-cpu 90 --alert-mem 90 --alert-disk 10
//
// 采集项：
//   cpu_usage_percent  区间占用率（Windows + Linux + macOS；FreeBSD 为 -1）
//   memory_used/total  物理内存（字节）；disk_free/total 采集路径所在卷
//
// 用到的 libmini 模块：system_info（含本次新增的 cpu_usage_percent）、
// hardware_info、machine_fingerprint、sqlite、metrics、http_server、log_facade、
// json_utils、args、console、stopwatch。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "app_common.h"
#include "libmini.h"
#include "utils/ui_panels.h"

using namespace libmini;
using namespace app;

namespace {

const int kDefaultIntervalMs = 1000;

// ---------------- 采样与存储 ----------------

struct Sample
{
    std::int64_t ts_ms = 0;
    double cpu_percent = -1.0;
    std::uint64_t mem_used = 0;
    std::uint64_t mem_total = 0;
    std::uint64_t disk_free = 0;
    std::uint64_t disk_total = 0;
};

struct Alert
{
    std::int64_t id = 0;
    std::int64_t ts_ms = 0;
    std::string kind;
    std::string detail;
    double value = 0.0;
};

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS samples("
    " ts_ms INTEGER PRIMARY KEY, cpu REAL, mem_used INTEGER, mem_total INTEGER,"
    " disk_free INTEGER, disk_total INTEGER);"
    "CREATE TABLE IF NOT EXISTS alerts("
    " id INTEGER PRIMARY KEY AUTOINCREMENT, ts_ms INTEGER, kind TEXT,"
    " detail TEXT, value REAL);"
    "CREATE INDEX IF NOT EXISTS idx_alerts_ts ON alerts(id DESC);";

class SampleStore
{
public:
    bool open(const std::string& path, std::string* error)
    {
        Status status = db_.try_open(path, SqliteDatabase::OpenReadWrite |
                                               SqliteDatabase::OpenCreate);
        if (!status.ok()) {
            *error = status.to_string();
            return false;
        }
        db_.set_busy_timeout_ms(5000);
        if (!db_.exec(kSchema)) {
            *error = db_.last_error("SampleStore::schema").to_string();
            return false;
        }
        return true;
    }

    bool insert(const Sample& sample)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "INSERT OR REPLACE INTO samples(ts_ms, cpu, mem_used, mem_total,"
                           " disk_free, disk_total) VALUES(?, ?, ?, ?, ?, ?)");
        if (!st.is_prepared() || !st.bind_int64(1, sample.ts_ms) ||
            !st.bind_double(2, sample.cpu_percent) ||
            !st.bind_int64(3, static_cast<std::int64_t>(sample.mem_used)) ||
            !st.bind_int64(4, static_cast<std::int64_t>(sample.mem_total)) ||
            !st.bind_int64(5, static_cast<std::int64_t>(sample.disk_free)) ||
            !st.bind_int64(6, static_cast<std::int64_t>(sample.disk_total))) {
            return false;
        }
        return st.step() == SqliteStatement::StepDone;
    }

    bool add_alert(const Alert& alert, std::int64_t* id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "INSERT INTO alerts(ts_ms, kind, detail, value)"
                           " VALUES(?, ?, ?, ?)");
        if (!st.is_prepared() || !st.bind_int64(1, alert.ts_ms) ||
            !st.bind_text(2, alert.kind) || !st.bind_text(3, alert.detail) ||
            !st.bind_double(4, alert.value)) {
            return false;
        }
        if (st.step() != SqliteStatement::StepDone) {
            return false;
        }
        if (id != nullptr) {
            *id = db_.last_insert_rowid();
        }
        return true;
    }

    // 最新的 limit 条采样，返回顺序为时间升序（前端画图直接用）
    std::vector<Sample> recent(std::size_t limit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Sample> out;
        SqliteStatement st(db_,
                           "SELECT ts_ms, cpu, mem_used, mem_total, disk_free, disk_total"
                           " FROM samples ORDER BY ts_ms DESC LIMIT ?");
        if (!st.is_prepared() || !st.bind_int(1, static_cast<int>(limit))) {
            return out;
        }
        while (st.step() == SqliteStatement::StepRow) {
            Sample sample;
            sample.ts_ms = st.column_int64(0);
            sample.cpu_percent = st.column_double(1);
            sample.mem_used = static_cast<std::uint64_t>(st.column_int64(2));
            sample.mem_total = static_cast<std::uint64_t>(st.column_int64(3));
            sample.disk_free = static_cast<std::uint64_t>(st.column_int64(4));
            sample.disk_total = static_cast<std::uint64_t>(st.column_int64(5));
            out.push_back(sample);
        }
        // 反转为升序
        std::vector<Sample> ascending;
        for (std::size_t i = out.size(); i > 0; --i) {
            ascending.push_back(out[i - 1]);
        }
        return ascending;
    }

    std::vector<Alert> alerts(std::size_t limit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Alert> out;
        SqliteStatement st(db_,
                           "SELECT id, ts_ms, kind, detail, value FROM alerts"
                           " ORDER BY id DESC LIMIT ?");
        if (!st.is_prepared() || !st.bind_int(1, static_cast<int>(limit))) {
            return out;
        }
        while (st.step() == SqliteStatement::StepRow) {
            Alert alert;
            alert.id = st.column_int64(0);
            alert.ts_ms = st.column_int64(1);
            alert.kind = st.column_text(2);
            alert.detail = st.column_text(3);
            alert.value = st.column_double(4);
            out.push_back(alert);
        }
        return out;
    }

    std::size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_, "SELECT COUNT(*) FROM samples");
        if (!st.is_prepared()) {
            return 0;
        }
        return st.step() == SqliteStatement::StepRow
                   ? static_cast<std::size_t>(st.column_int64(0))
                   : 0;
    }

    // 只保留最近 keep 条（避免长期运行把库撑大）
    void prune(std::size_t keep)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "DELETE FROM samples WHERE ts_ms NOT IN (SELECT ts_ms FROM samples"
                           " ORDER BY ts_ms DESC LIMIT ?)");
        if (st.is_prepared() && st.bind_int(1, static_cast<int>(keep))) {
            st.step();
        }
    }

private:
    SqliteDatabase db_;
    std::mutex mutex_;
};

// ---------------- 阈值 ----------------

struct Thresholds
{
    bool cpu_enabled = false;
    double cpu_percent = 90.0;
    bool mem_enabled = false;
    double mem_percent = 90.0;
    bool disk_enabled = false;
    double disk_free_percent_below = 10.0;
};

// ---------------- 采集器 ----------------

class Collector
{
public:
    Collector(SampleStore* store, MetricRegistry* metrics, const Thresholds& thresholds,
              int interval_ms, std::size_t keep, std::string disk_path)
        : store_(store), metrics_(metrics), thresholds_(thresholds),
          interval_ms_(interval_ms < 50 ? 50 : interval_ms), keep_(keep),
          disk_path_(disk_path), stopping_(false), samples_(0), last_alert_total_(0)
    {
        cpu_gauge_ = metrics_->gauge("sysmon_cpu_usage_percent", "CPU 区间占用率（%）");
        mem_used_gauge_ = metrics_->gauge("sysmon_memory_used_bytes", "已用物理内存");
        mem_total_gauge_ = metrics_->gauge("sysmon_memory_total_bytes", "物理内存总量");
        mem_percent_gauge_ = metrics_->gauge("sysmon_memory_used_percent", "内存占用率（%）");
        disk_free_gauge_ = metrics_->gauge("sysmon_disk_free_bytes", "采集路径所在卷剩余");
        disk_total_gauge_ = metrics_->gauge("sysmon_disk_total_bytes", "采集路径所在卷容量");
        samples_counter_ = metrics_->counter("sysmon_samples_total", "累计采样次数");
        alerts_counter_ = metrics_->counter("sysmon_alerts_total", "累计告警次数");
        collect_hist_ = metrics_->histogram("sysmon_collect_duration_ms", "单次采集耗时");
    }

    ~Collector() { stop(); }

    void start()
    {
        stopping_.store(false);
        thread_ = std::thread([this]() { loop(); });
    }

    void stop()
    {
        if (stopping_.exchange(true)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // 立即采集一次（demo/手动刷新用）
    Sample collect_once()
    {
        Stopwatch stopwatch;
        Sample sample;
        sample.ts_ms = current_timestamp_ms();
        sample.cpu_percent = cpu_usage_percent();
        sample.mem_total = total_physical_memory();
        const std::uint64_t available = available_physical_memory();
        sample.mem_used = sample.mem_total > available ? sample.mem_total - available : 0;
        sample.disk_total = disk_total_bytes(disk_path_);
        sample.disk_free = disk_free_bytes(disk_path_);

        if (sample.cpu_percent >= 0.0) {
            cpu_gauge_->set(sample.cpu_percent);
        }
        mem_used_gauge_->set(static_cast<double>(sample.mem_used));
        mem_total_gauge_->set(static_cast<double>(sample.mem_total));
        mem_percent_gauge_->set(sample.mem_total == 0
                                    ? 0.0
                                    : 100.0 * static_cast<double>(sample.mem_used) /
                                          static_cast<double>(sample.mem_total));
        disk_free_gauge_->set(static_cast<double>(sample.disk_free));
        disk_total_gauge_->set(static_cast<double>(sample.disk_total));
        samples_counter_->inc();
        samples_.store(samples_.load() + 1);
        collect_hist_->observe(static_cast<double>(stopwatch.elapsed_ms()));
        check_alerts(sample);
        return sample;
    }

    std::size_t sample_count() const { return samples_.load(); }

private:
    void loop()
    {
        // 第一轮先建立 CPU 基线，避免首条采样是 -1
        cpu_usage_percent();
        while (!stopping_.load()) {
            const Sample sample = collect_once();
            store_->insert(sample);
            if (keep_ > 0 && (samples_.load() % 60) == 0) {
                store_->prune(keep_);
            }
            const int slices = interval_ms_ / 50;
            for (int i = 0; i < slices && !stopping_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void check_alerts(const Sample& sample)
    {
        if (thresholds_.cpu_enabled && sample.cpu_percent >= 0.0 &&
            sample.cpu_percent >= thresholds_.cpu_percent) {
            raise("cpu", sample.cpu_percent, "CPU 占用 " + fmt_one(sample.cpu_percent) +
                                                 "% 超过阈值 " +
                                                 fmt_one(thresholds_.cpu_percent) + "%");
        } else {
            cpu_alert_active_ = false;
        }
        if (sample.mem_total > 0) {
            const double used_percent = 100.0 * static_cast<double>(sample.mem_used) /
                                        static_cast<double>(sample.mem_total);
            if (thresholds_.mem_enabled && used_percent >= thresholds_.mem_percent) {
                raise("memory", used_percent,
                      "内存占用 " + fmt_one(used_percent) + "% 超过阈值 " +
                          fmt_one(thresholds_.mem_percent) + "%");
            } else {
                mem_alert_active_ = false;
            }
        }
        if (sample.disk_total > 0) {
            const double free_percent = 100.0 * static_cast<double>(sample.disk_free) /
                                        static_cast<double>(sample.disk_total);
            if (thresholds_.disk_enabled &&
                free_percent <= thresholds_.disk_free_percent_below) {
                raise("disk", free_percent,
                      "磁盘剩余 " + fmt_one(free_percent) + "% 低于阈值 " +
                          fmt_one(thresholds_.disk_free_percent_below) + "%");
            } else {
                disk_alert_active_ = false;
            }
        }
    }

    // 只在「进入告警态」时记一次，避免每秒写一条同样的告警
    void raise(const std::string& kind, double value, const std::string& detail)
    {
        bool* active = &cpu_alert_active_;
        if (kind == "memory") {
            active = &mem_alert_active_;
        } else if (kind == "disk") {
            active = &disk_alert_active_;
        }
        if (*active) {
            return;
        }
        *active = true;
        Alert alert;
        alert.ts_ms = current_timestamp_ms();
        alert.kind = kind;
        alert.detail = detail;
        alert.value = value;
        store_->add_alert(alert, nullptr);
        alerts_counter_->inc();
        last_alert_total_.store(last_alert_total_.load() + 1);
        if (LogFacade::logger() != nullptr) {
            LogFacade::logger()->warn("[告警] {}", detail);
        }
    }

    static std::string fmt_one(double value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        return std::string(buffer);
    }

    SampleStore* store_;
    MetricRegistry* metrics_;
    Thresholds thresholds_;
    int interval_ms_;
    std::size_t keep_;
    std::string disk_path_;
    std::atomic<bool> stopping_;
    std::atomic<std::size_t> samples_;
    std::atomic<std::size_t> last_alert_total_;
    std::thread thread_;
    bool cpu_alert_active_ = false;
    bool mem_alert_active_ = false;
    bool disk_alert_active_ = false;
    std::shared_ptr<MetricGauge> cpu_gauge_;
    std::shared_ptr<MetricGauge> mem_used_gauge_;
    std::shared_ptr<MetricGauge> mem_total_gauge_;
    std::shared_ptr<MetricGauge> mem_percent_gauge_;
    std::shared_ptr<MetricGauge> disk_free_gauge_;
    std::shared_ptr<MetricGauge> disk_total_gauge_;
    std::shared_ptr<MetricCounter> samples_counter_;
    std::shared_ptr<MetricCounter> alerts_counter_;
    std::shared_ptr<MetricHistogram> collect_hist_;
};

// ---------------- HTTP 接口 ----------------

JsonValue sample_to_json(const Sample& sample)
{
    const double mem_percent =
        sample.mem_total == 0
            ? 0.0
            : 100.0 * static_cast<double>(sample.mem_used) / static_cast<double>(sample.mem_total);
    const double disk_free_percent =
        sample.disk_total == 0 ? 0.0
                               : 100.0 * static_cast<double>(sample.disk_free) /
                                     static_cast<double>(sample.disk_total);
    JsonValue node = JsonValue::object();
    node["ts_ms"] = sample.ts_ms;
    node["cpu_percent"] = sample.cpu_percent;
    node["memory_used"] = static_cast<std::int64_t>(sample.mem_used);
    node["memory_total"] = static_cast<std::int64_t>(sample.mem_total);
    node["memory_percent"] = mem_percent;
    node["disk_free"] = static_cast<std::int64_t>(sample.disk_free);
    node["disk_total"] = static_cast<std::int64_t>(sample.disk_total);
    node["disk_free_percent"] = disk_free_percent;
    return node;
}

const char kDashboardPage[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<title>sysmon</title><style>"
    "body{font:14px/1.5 ui-sans-serif,system-ui,'Segoe UI',sans-serif;background:#0e1015;"
    "color:#e7e9ef;margin:24px}h1{font-size:19px;margin:0 0 4px}.dim{color:#8b93a7}"
    ".grid{display:flex;gap:18px;flex-wrap:wrap}.card{background:#161923;"
    "border:1px solid #242938;border-radius:10px;padding:14px 16px;min-width:220px;flex:1}"
    ".card b{font-size:22px;font-weight:650;display:block}.card span{font-size:12px}"
    "canvas{width:100%;height:60px;display:block;margin-top:8px}"
    "table{border-collapse:collapse;width:100%;margin-top:8px;font-size:13px}"
    "th,td{border-bottom:1px solid #242938;padding:6px 8px;text-align:left}"
    "th{color:#8b93a7;font-weight:500}</style></head><body>"
    "<h1>sysmon 本机观测台</h1><div class=\"dim\" id=\"meta\">加载中…</div>"
    "<div class=\"grid\" style=\"margin-top:14px\">"
    "<div class=\"card\"><span class=\"dim\">CPU 占用</span><b id=\"cpu\">-</b>"
    "<canvas id=\"cpuChart\"></canvas></div>"
    "<div class=\"card\"><span class=\"dim\">内存占用</span><b id=\"mem\">-</b>"
    "<canvas id=\"memChart\"></canvas></div>"
    "<div class=\"card\"><span class=\"dim\">磁盘剩余</span><b id=\"disk\">-</b>"
    "<span class=\"dim\" id=\"diskDetail\"></span></div></div>"
    "<h3>告警</h3><div id=\"alerts\" class=\"dim\">（无）</div>"
    "<script>"
    "function $(id){return document.getElementById(id);}"
    "function fmtBytes(v){var u=['B','KiB','MiB','GiB','TiB'],i=0;while(v>=1024&&i<u.length-1){"
    "v/=1024;++i;}return v.toFixed(1)+' '+u[i];}"
    "function draw(id,values,color){var c=$(id);var r=c.getBoundingClientRect();"
    "c.width=r.width*2;c.height=r.height*2;var x=c.getContext('2d');"
    "x.scale(2,2);x.clearRect(0,0,r.width,r.height);if(values.length<2)return;"
    "var max=1,i;for(i=0;i<values.length;++i){if(values[i]>max)max=values[i];}"
    "x.beginPath();for(i=0;i<values.length;++i){var px=i*(r.width/(values.length-1));"
    "var py=r.height-(values[i]/max)*(r.height-6)-3;"
    "if(i===0){x.moveTo(px,py);}else{x.lineTo(px,py);}}"
    "x.strokeStyle=color;x.lineWidth=1.6;x.stroke();}"
    "function poll(){fetch('/api/state',{cache:'no-store'}).then(function(r){return r.json();})"
    ".then(function(d){var s=d.latest;if(!s){return;}"
    "$('cpu').textContent=(s.cpu_percent<0?'不支持':s.cpu_percent.toFixed(1)+' %');"
    "$('mem').textContent=s.memory_percent.toFixed(1)+' %';"
    "$('disk').textContent=s.disk_free_percent.toFixed(1)+' %';"
    "$('diskDetail').textContent='剩余 '+fmtBytes(s.disk_free)+' / 共 '+fmtBytes(s.disk_total)+"
    "'  · 采集 '+d.samples_count+' 次';"
    "$('meta').textContent='主机 '+d.host+' · 指纹 '+d.fingerprint+' · 采集间隔 '+"
    "d.interval_ms+'ms · 最新 '+new Date(s.ts_ms).toLocaleTimeString();"
    "var cpu=[],mem=[];d.series.forEach(function(p){cpu.push(Math.max(p.cpu_percent,0));"
    "mem.push(p.memory_percent);});draw('cpuChart',cpu,'#4aa8ff');draw('memChart',mem,'#3ddc84');"
    "var a=d.alerts||[];$('alerts').innerHTML=a.length?a.map(function(x){return '<div>'+"
    "new Date(x.ts_ms).toLocaleString()+' · '+esc(x.kind)+' · '+esc(x.detail)+'</div>';})"
    ".join(''):'（无）';}).catch(function(e){$('meta').textContent='与进程失联：'+e;});}"
    "function esc(v){return String(v==null?'':v).replace(/[&<>\"]/g,function(c){"
    "return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c];});}"
    "poll();setInterval(poll,1000);draw('cpuChart',[],'#4aa8ff');draw('memChart',[],'#3ddc84');"
    "</script></body></html>";

class SysmonApi
{
public:
    SysmonApi(SampleStore* store, Collector* collector, MetricRegistry* metrics,
              int interval_ms, const Thresholds& thresholds)
        : store_(store), collector_(collector), metrics_(metrics), interval_ms_(interval_ms),
          thresholds_(thresholds)
    {
    }

    void register_routes(HttpServer* server)
    {
        server->get("/", [this](const HttpRequest&) { return index(); });
        server->get("/api/state", [this](const HttpRequest&) { return state(); });
        server->get("/api/series", [this](const HttpRequest& req) { return series(req); });
        server->get("/api/alerts", [this](const HttpRequest& req) { return alerts(req); });
        server->get("/metrics", [this](const HttpRequest&) { return metrics_text(); });
    }

private:
    HttpReply index()
    {
        HttpReply reply = HttpReply::text(200, kDashboardPage);
        reply.headers["Content-Type"] = "text/html; charset=utf-8";
        return reply;
    }

    HttpReply state()
    {
        const std::vector<Sample> history = store_->recent(120);
        JsonValue series = JsonValue::array();
        for (std::size_t i = 0; i < history.size(); ++i) {
            series.push_back(sample_to_json(history[i]));
        }
        JsonValue root = JsonValue::object();
        root["host"] = hostname();
        root["fingerprint"] = machine_fingerprint().id;
        root["cpu_count"] = cpu_count();
        root["interval_ms"] = interval_ms_;
        root["samples_count"] = static_cast<std::int64_t>(collector_->sample_count());
        root["series"] = series;
        if (!history.empty()) {
            root["latest"] = sample_to_json(history[history.size() - 1]);
        } else {
            root["latest"] = JsonValue();
        }
        JsonValue alert_array = JsonValue::array();
        const std::vector<Alert> recent_alerts = store_->alerts(20);
        for (std::size_t i = 0; i < recent_alerts.size(); ++i) {
            JsonValue node = JsonValue::object();
            node["ts_ms"] = recent_alerts[i].ts_ms;
            node["kind"] = recent_alerts[i].kind;
            node["detail"] = recent_alerts[i].detail;
            node["value"] = recent_alerts[i].value;
            alert_array.push_back(node);
        }
        root["alerts"] = alert_array;
        return json_ok(root);
    }

    HttpReply series(const HttpRequest& req)
    {
        int limit = 120;
        std::map<std::string, std::string>::const_iterator it = req.query.find("limit");
        if (it != req.query.end()) {
            to_int(it->second, &limit);
        }
        if (limit < 1) {
            limit = 1;
        }
        if (limit > 2000) {
            limit = 2000;
        }
        const std::vector<Sample> history = store_->recent(static_cast<std::size_t>(limit));
        JsonValue array = JsonValue::array();
        for (std::size_t i = 0; i < history.size(); ++i) {
            array.push_back(sample_to_json(history[i]));
        }
        JsonValue root = JsonValue::object();
        root["series"] = array;
        return json_ok(root);
    }

    HttpReply alerts(const HttpRequest& req)
    {
        int limit = 50;
        std::map<std::string, std::string>::const_iterator it = req.query.find("limit");
        if (it != req.query.end()) {
            to_int(it->second, &limit);
        }
        const std::vector<Alert> records = store_->alerts(static_cast<std::size_t>(limit < 1 ? 1 : limit));
        JsonValue array = JsonValue::array();
        for (std::size_t i = 0; i < records.size(); ++i) {
            JsonValue node = JsonValue::object();
            node["id"] = records[i].id;
            node["ts_ms"] = records[i].ts_ms;
            node["kind"] = records[i].kind;
            node["detail"] = records[i].detail;
            node["value"] = records[i].value;
            array.push_back(node);
        }
        JsonValue root = JsonValue::object();
        root["alerts"] = array;
        return json_ok(root);
    }

    HttpReply metrics_text()
    {
        HttpReply reply = HttpReply::text(200, metrics_->render_prometheus());
        reply.headers["Content-Type"] = "text/plain; version=0.0.4; charset=utf-8";
        return reply;
    }

    SampleStore* store_;
    Collector* collector_;
    MetricRegistry* metrics_;
    int interval_ms_;
    Thresholds thresholds_;
};

// ---------------- demo ----------------

int run_demo()
{
    DemoReport report("sysmon demo");

    // 1) 直接验证新加的库 API 语义：首次 -1（建立基线），之后落在 [0, 100]
    const double first = cpu_usage_percent();
    report.check(first < 0.0, "cpu_usage_percent 首次调用返回 -1（建立基线）");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double second = cpu_usage_percent();
    report.check(second >= 0.0 && second <= 100.0,
                 "cpu_usage_percent 第二次调用给出区间占用率（0..100）");
    report.check(total_physical_memory() > 0, "读取物理内存总量");
    report.check(disk_total_bytes(temp_directory_path()) > 0,
                 "读取采集路径所在卷容量");

    // 2) 起采集器 + 服务
    const std::string workspace = make_workspace("sysmon_demo");
    report.check(!workspace.empty(), "创建临时工作目录");
    if (workspace.empty()) {
        return report.finish();
    }
    const std::string db_path = workspace_file(workspace, "sysmon.db");

    SampleStore store;
    std::string error;
    if (!store.open(db_path, &error)) {
        report.check(false, "打开 SQLite: " + error);
        remove_tree(workspace);
        return report.finish();
    }
    report.check(true, "打开 SQLite 并建表");

    Thresholds thresholds;
    thresholds.cpu_enabled = true;
    thresholds.cpu_percent = 0.0;  // demo 里让 CPU 阈值必然触发，验证告警链路
    MetricRegistry metrics;
    Collector collector(&store, &metrics, thresholds, 100, 1000, workspace);

    SysmonApi api(&store, &collector, &metrics, 100, thresholds);
    HttpServer server;
    server.enable_health_endpoints();
    api.register_routes(&server);
    const bool listening = server.start_background(0) && server.wait_until_ready(5000);
    report.check(listening, "看板服务监听成功（自动分配端口）");
    if (!listening) {
        remove_tree(workspace);
        return report.finish();
    }
    const std::string base = "http://127.0.0.1:" + lexical_cast<std::string>(server.port());
    report.info("看板: " + base + "/");

    collector.start();
    // 等到「至少 3 条采样」且「至少一条带可用的 CPU 读数」再停采集器。
    // cpu_usage_percent 是区间占用率，读数取决于两次调用之间 CPU 时间计数器
    // 有没有推进：刻度粗的平台（macOS 的聚合计数器尤其）在短间隔下会给出 -1。
    // 告警检查依赖一条可用读数，所以这里必须等它出现，而不是等固定条数。
    const std::int64_t deadline = current_timestamp_ms() + 8000;
    bool cpu_seen = false;
    while (current_timestamp_ms() < deadline) {
        const std::vector<Sample> recent = store.recent(8);
        cpu_seen = false;
        for (std::size_t i = 0; i < recent.size(); ++i) {
            if (recent[i].cpu_percent >= 0.0) {
                cpu_seen = true;
            }
        }
        if (store.count() >= 3 && cpu_seen) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    collector.stop();
    report.check(store.count() >= 3, "采集器按间隔写入至少 3 条采样");
    report.check(cpu_seen, "至少一条采样带可用的 CPU 读数");

    // 3) HTTP 面
    HttpClient http(base);
    Result<HttpResponse> state = http.try_get("/api/state");
    bool state_ok = false;
    if (state.ok()) {
        try {
            const JsonValue doc = parse_json(state.value().body);
            state_ok = doc["latest"].is_object() && doc["series"].is_array() &&
                       doc["series"].size() >= 3 && doc["fingerprint"].is_string();
        } catch (const std::exception&) {
        }
    }
    report.check(state_ok, "GET /api/state 返回最新采样与序列");

    Result<HttpResponse> series = http.try_get("/api/series?limit=10");
    bool series_ok = false;
    if (series.ok()) {
        try {
            const JsonValue doc = parse_json(series.value().body);
            series_ok = doc["series"].is_array() && doc["series"].size() <= 10;
        } catch (const std::exception&) {
        }
    }
    report.check(series_ok, "GET /api/series?limit=10 限制条数");

    Result<HttpResponse> prometheus = http.try_get("/metrics");
    report.check(prometheus.ok() &&
                     prometheus.value().body.find("sysmon_cpu_usage_percent") !=
                         std::string::npos &&
                     prometheus.value().body.find("sysmon_memory_used_bytes") !=
                         std::string::npos,
                 "GET /metrics 输出 CPU/内存指标");

    Result<HttpResponse> alerts = http.try_get("/api/alerts");
    bool alert_ok = false;
    if (alerts.ok()) {
        try {
            const JsonValue doc = parse_json(alerts.value().body);
            alert_ok = doc["alerts"].is_array() && !doc["alerts"].empty() &&
                       doc["alerts"][0]["kind"].get<std::string>() == "cpu";
        } catch (const std::exception&) {
        }
    }
    report.check(alert_ok, "越阈触发 CPU 告警并可通过 API 查询");

    Result<HttpResponse> ready = http.try_get("/readyz");
    report.check(ready.ok() && ready.value().status == 200, "GET /readyz 就绪探针通过");
    report.check(http.try_get("/").ok() && http.try_get("/").value().status == 200,
                 "GET / 返回看板页面");

    server.stop();
    report.info("采样库: " + db_path);
    remove_tree(workspace);
    return report.finish();
}

// ---------------- 原生窗口模式 ----------------

// `--ui`：把本应用的采集服务拉起来（子进程，与启动器同一套托管机制），再开原生
// 窗口盯实时状态：/api/state + /api/alerts 两个 JSON 视图、CPU/内存趋势折线与过滤后的
// 指标行。selftest = true 时跑满固定帧数、截图并打印结论（CI 用）。
int run_ui(const std::string& argv0, const std::string& db_path, const int port,
           const std::string& interval, bool selftest)
{
    ui::AppWindowSpec spec;
    spec.title = "sysmon 观测台";
    spec.subtitle = "采集 / 告警 / Prometheus 指标";
    spec.port = port;
    spec.child_args.push_back(app::self_exe(argv0));
    spec.child_args.push_back("--db");
    spec.child_args.push_back(db_path);
    spec.child_args.push_back("--port");
    spec.child_args.push_back(lexical_cast<std::string>(port));
    spec.child_args.push_back("--interval");
    spec.child_args.push_back(interval);

    spec.panel.json_views.push_back("/api/state");
    spec.panel.json_views.push_back("/api/alerts");
    spec.panel.metric_filters.push_back("sysmon_cpu_usage_percent");
    spec.panel.metric_filters.push_back("sysmon_memory_used_bytes");
    spec.panel.metric_filters.push_back("sysmon_disk_free_bytes");
    spec.panel.series_paths.push_back("latest.cpu_percent");
    spec.panel.series_paths.push_back("latest.memory_percent");

    if (selftest) {
        spec.frames = 40;
        spec.report = true;
        spec.shot_path = app::workspace_file(app::make_workspace("sysmon_ui"), "window.bmp");
    }

    std::string error;
    const int code = ui::run_app_window(spec, &error);
    if (code == 2) {
        std::cerr << "无法打开原生窗口（" << error << "），请改用 `sysmon --port ...`。\n";
    }
    return code;
}

// ---------------- 正常模式 ----------------

int run_service(Args& args)
{
    const std::string db_path = args.get_string("db");
    std::string error;
    SampleStore store;
    if (!store.open(db_path, &error)) {
        std::cerr << "打开数据库失败: " << error << "\n";
        return 1;
    }

    int interval_ms = kDefaultIntervalMs;
    if (!parse_duration_ms(args.get_string("interval"), &interval_ms)) {
        std::cerr << "采集间隔非法（示例 1s / 500ms）\n";
        return 1;
    }

    Thresholds thresholds;
    const std::string cpu_text = trim(args.get_string("alert-cpu"));
    if (!cpu_text.empty()) {
        thresholds.cpu_enabled = true;
        thresholds.cpu_percent = lexical_cast_or<double>(cpu_text, 90.0);
    }
    const std::string mem_text = trim(args.get_string("alert-mem"));
    if (!mem_text.empty()) {
        thresholds.mem_enabled = true;
        thresholds.mem_percent = lexical_cast_or<double>(mem_text, 90.0);
    }
    const std::string disk_text = trim(args.get_string("alert-disk"));
    if (!disk_text.empty()) {
        thresholds.disk_enabled = true;
        thresholds.disk_free_percent_below = lexical_cast_or<double>(disk_text, 10.0);
    }

    LogFacade::init();
    MetricRegistry metrics;
    Collector collector(&store, &metrics, thresholds, interval_ms, 100000,
                        args.get_string("path"));
    SysmonApi api(&store, &collector, &metrics, interval_ms, thresholds);

    HttpServer server;
    server.enable_health_endpoints();
    api.register_routes(&server);
    if (!server.start_background(args.get_int("port")) || !server.wait_until_ready(5000)) {
        std::cerr << "监听失败: " << server.last_error() << "\n";
        return 1;
    }
    collector.start();

    print_banner("sysmon", "数据库: " + db_path + "  采集间隔: " + ms_text(interval_ms));
    std::cout << "看板: http://127.0.0.1:" << server.port() << "/\n"
              << "指标: http://127.0.0.1:" << server.port() << "/metrics\n"
              << "主机: " << hostname() << "  指纹: " << machine_fingerprint().id << "\n"
              << "按 Ctrl+C 优雅退出\n"
              << std::flush;

    ConsoleExit exit_signal;
    exit_signal.wait();
    std::cout << "\n正在停止…\n";
    collector.stop();
    server.stop();
    LogFacade::shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args("sysmon", "1.0",
              "libmini 应用：本机观测台（采集 CPU/内存/磁盘 + Prometheus 指标 + 看板 + 告警）");
    args.add_flag("demo", "", "自检模式：快采样几轮并汇总结果");
    args.add_option("db", "", "SQLite 数据库路径", std::string("sysmon.db"));
    args.add_int("port", "p", "看板端口（0 = 自动分配）", 8790);
    args.add_option("interval", "i", "采集间隔（如 1s / 500ms）", std::string("1s"));
    args.add_option("path", "", "用于统计磁盘余量的路径", std::string("."));
    args.add_option("alert-cpu", "", "CPU 占用超过该百分比时告警（空 = 关闭）",
                    std::string(""));
    args.add_option("alert-mem", "", "内存占用超过该百分比时告警（空 = 关闭）",
                    std::string(""));
    args.add_option("alert-disk", "", "磁盘剩余低于该百分比时告警（空 = 关闭）",
                    std::string(""));
    args.add_flag("ui", "", "打开原生窗口（实时观测面板，Windows）");
    args.add_flag("ui-selftest", "", "窗口自检：限帧渲染 + 截图 + 结论（CI 用）");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }
    if (args.has_flag("demo")) {
        return run_demo();
    }
    if (args.has_flag("ui") || args.has_flag("ui-selftest")) {
        const int port = args.get_int("port") > 0 ? args.get_int("port") : 8790;
        const std::string argv0 = (argc > 0 && argv[0] != 0) ? std::string(argv[0])
                                                             : std::string("sysmon");
        return run_ui(argv0, args.get_string("db"), port, args.get_string("interval"),
                      args.has_flag("ui-selftest"));
    }
    return run_service(args);
}
