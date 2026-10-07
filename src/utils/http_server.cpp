#include "http_server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>
#include <atomic>

#include <httplib.h>

#include "config_facade.h"
#include "json_utils.h"  // /readyz 响应组装

namespace libmini {

namespace {

// 请求体大小默认上限（64 MiB）
constexpr std::size_t kDefaultMaxBodyBytes = 64u * 1024u * 1024u;

// ---------------- URL 解码（query 参数用；'+' 视为空格） ----------------

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]);
            const int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// ---------------- query 串解析 ----------------

std::map<std::string, std::string> parse_query(const std::string& query)
{
    std::map<std::string, std::string> out;
    std::string::size_type pos = 0;
    while (pos < query.size()) {
        const std::string::size_type amp = query.find('&', pos);
        const std::string::size_type end =
            (amp == std::string::npos) ? query.size() : amp;
        const std::string::size_type eq = query.find('=', pos);
        if (eq != std::string::npos && eq < end) {
            out[url_decode(query.substr(pos, eq - pos))] =
                url_decode(query.substr(eq + 1, end - eq - 1));
        } else if (end > pos) {
            out[url_decode(query.substr(pos, end - pos))] = std::string();
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::string ascii_lower(const std::string& s)
{
    std::string out = s;
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

// ---------------- httplib → libmini 的请求转换 ----------------
//
// 说明：httplib 的 req.params 同时包含路径参数与 query 参数（已 URL 解码）；
// 这里先从 target 解析 query，再把 req.params 中与 query 同名的键剔除，
// 保证 HttpRequest::params 只含路径参数。

HttpRequest convert_request(const httplib::Request& req)
{
    HttpRequest out;
    out.method = req.method;
    out.path = req.path;
    out.body = req.body;
    if (!req.remote_addr.empty()) {
        out.remote_addr = req.remote_addr;  // inet_ntop 产物，纯 ASCII
        if (req.remote_port >= 0) {
            out.remote_addr += ":" + std::to_string(req.remote_port);
        }  // remote_port 为 -1 时（无对端）只写地址
    }
    {
        const std::string::size_type q = req.target.find('?');
        if (q != std::string::npos) {
            out.query = parse_query(req.target.substr(q + 1));
        }
    }
    {
        // 此版 httplib：路径参数在 req.path_params（unordered_map），
        // req.params 只含 query 参数（已 URL 解码）
        for (std::unordered_map<std::string, std::string>::const_iterator it =
                 req.path_params.begin();
             it != req.path_params.end(); ++it) {
            out.params[it->first] = it->second;
        }
    }
    for (httplib::Params::const_iterator it = req.params.begin();
         it != req.params.end(); ++it) {
        out.query[it->first] = it->second;  // httplib 已解码，与 target 解析合并
    }
    for (httplib::Headers::const_iterator it = req.headers.begin();
         it != req.headers.end(); ++it) {
        // httplib 的 Headers 保留原始大小写（case_ignore 只参与哈希比较），
        // 统一转小写，与 HttpClient 的响应头口径一致
        out.headers[ascii_lower(it->first)] = it->second;
    }
    return out;
}

// ---------------- libmini → httplib 的响应转换 ----------------

void apply_reply(const HttpReply& reply, httplib::Response& res)
{
    res.status = reply.status;
    res.body = reply.body;
    for (std::map<std::string, std::string>::const_iterator it =
             reply.headers.begin();
         it != reply.headers.end(); ++it) {
        res.set_header(it->first, it->second);
    }
}

}  // namespace

// ---------------- HttpRequest ----------------

std::string HttpRequest::param(const std::string& name) const
{
    std::map<std::string, std::string>::const_iterator it = params.find(name);
    return (it == params.end()) ? std::string() : it->second;
}

// ---------------- HttpReply 静态构造 ----------------

HttpReply HttpReply::text(int status, const std::string& body)
{
    HttpReply r;
    r.status = status;
    r.body = body;
    r.headers["Content-Type"] = "text/plain; charset=utf-8";
    return r;
}

HttpReply HttpReply::json(int status, const std::string& json_body)
{
    HttpReply r;
    r.status = status;
    r.body = json_body;
    r.headers["Content-Type"] = "application/json; charset=utf-8";
    return r;
}

HttpReply HttpReply::error(int status, const std::string& message)
{
    std::ostringstream os;
    os << "{\"error\":\"";
    for (std::size_t i = 0; i < message.size(); ++i) {
        const char c = message[i];
        if (c == '"' || c == '\\') {
            os << '\\' << c;
        } else if (c == '\n') {
            os << "\\n";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            os << ' ';
        } else {
            os << c;
        }
    }
    os << "\",\"status\":" << status << "}";
    return json(status, os.str());
}

// ================================ Impl ====================================

class HttpServer::Impl
{
public:
    httplib::Server svr;
    int configured_port = 0;   // apply_config 的 web.port
    int actual_port = 0;       // 启动后真实监听端口
    std::string error_text;
    std::atomic<bool> ready;
    std::mutex error_mu;

    HttpHandler fallback;                       // 未命中路由的兜底
    std::vector<HttpFilter> filters;            // 前置过滤器（按注册顺序）
    HttpAccessLogger access_logger;             // 访问日志钩子

    // ------- 健康检查状态（运行期可读写，health_mu 保护） -------
    HttpHandler liveness;                       // 空 = 默认 200 "ok"
    struct ReadinessEntry {
        std::string name;
        HealthCheck check;
    };
    std::vector<ReadinessEntry> readiness;      // 注册顺序保序
    std::mutex health_mu;
    std::vector<std::string> get_routes;        // 已注册 GET/HEAD 路由（内建端点去重）
    bool healthz_registered = false;            // enable_health_endpoints 幂等
    bool readyz_registered = false;

    Impl()
    {
        bind_error_handlers();
        // 默认请求体上限 64 MiB（httplib 默认无限制，防止内存被撑爆）
        svr.set_payload_max_length(kDefaultMaxBodyBytes);
    }

    // ------- 异常与兜底响应的统一形态 -------

    void bind_error_handlers()
    {
        svr.set_exception_handler(
            [](const httplib::Request&, httplib::Response& res,
               std::exception_ptr ep) {
                std::string what = "internal error";
                try {
                    if (ep) std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    what = e.what();
                } catch (...) {
                }
                apply_reply(HttpReply::error(500, what), res);
            });
    }

    // ------- 包装 libmini 处理器为 httplib 处理器 -------

    httplib::Server::Handler wrap(HttpHandler handler)
    {
        return [this, handler](const httplib::Request& req,
                               httplib::Response& res) {
            HttpRequest hreq = convert_request(req);
            HttpReply hreply;
            bool handled = false;

            for (std::size_t i = 0; i < filters.size(); ++i) {
                HttpReply freply;
                if (!filters[i](hreq, freply)) {
                    hreply = freply;
                    handled = true;
                    break;
                }
            }
            if (!handled) {
                hreply = handler(hreq);
            }

            apply_reply(hreply, res);
        };
    }

    // ------- 访问日志（挂 httplib 的 logger：覆盖所有路径，含 fallback/错误） -------

    void install_access_logger()
    {
        svr.set_logger(
            [this](const httplib::Request& req, const httplib::Response& res) {
                if (!access_logger) {
                    return;
                }
                HttpRequest hreq = convert_request(req);
                HttpReply hreply;
                hreply.status = res.status;
                hreply.body = res.body;
                const std::chrono::steady_clock::time_point end =
                    std::chrono::steady_clock::now();
                const std::int64_t ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        end - req.start_time_)
                        .count();
                access_logger(hreq, hreply, ms);
            });
    }

    // ------- 未命中路由的兜底（含 404 默认 JSON 形态） -------
    //
    // 注意：此版 httplib 对一切 ≥400 的响应都会调用 error_handler
    //（write_response_core），包括过滤器/处理器主动返回的 401/403 等。
    // 用「响应体未被填充」作为未命中路由的判据，避免覆盖业务错误响应。

    void install_routing_error_handler()
    {
        svr.set_error_handler(
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!res.body.empty()) {
                    return;  // 处理器/过滤器已写响应体，不覆盖
                }
                if (fallback) {
                    apply_reply(fallback(convert_request(req)), res);
                } else {
                    apply_reply(HttpReply::error(res.status, "not found"), res);
                }
            });
    }

    // ------- start_background 的统一入口 -------

    bool listen(int port)
    {
        error_text.clear();
        ready = false;

        install_routing_error_handler();
        install_access_logger();

        std::thread t([this, port] {
            if (port == 0) {
                // bind_to_any_port 返回实际端口（<0 = 失败）
                const int bound = svr.bind_to_any_port("0.0.0.0");
                if (bound < 0) {
                    std::lock_guard<std::mutex> lock(error_mu);
                    error_text = "bind to any port failed";
                    return;
                }
                actual_port = bound;
                svr.listen_after_bind();  // 阻塞至 stop()
            } else {
                actual_port = port;
                if (!svr.bind_to_port("0.0.0.0", port)) {
                    std::ostringstream os;
                    os << "bind failed on port " << port
                       << "（端口被占用或无权限）";
                    std::lock_guard<std::mutex> lock(error_mu);
                    error_text = os.str();
                    return;
                }
                svr.listen_after_bind();
            }
        });
        t.detach();

        // 等待监听就绪或显式失败（bind 通常瞬时完成）
        for (int i = 0; i < 300; ++i) {
            if (svr.is_running()) {
                ready = true;
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(error_mu);
                if (!error_text.empty()) {
                    return false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        error_text = "listen timeout";
        return false;
    }
};

// ================================ HttpServer ==============================

HttpServer::HttpServer() : impl_(new Impl) {}

HttpServer::~HttpServer()
{
    stop();
}

// ---------------- 路由注册 ----------------

void HttpServer::route(const std::string& method, const std::string& pattern,
                       HttpHandler handler)
{
    httplib::Server::Handler h = impl_->wrap(std::move(handler));
    if (method == "GET" || method == "HEAD") {
        impl_->get_routes.push_back(pattern);  // 内建健康端点「显式优先」的判据
    }
    if (method == "GET") {
        impl_->svr.Get(pattern, h);
    } else if (method == "POST") {
        impl_->svr.Post(pattern, h);
    } else if (method == "PUT") {
        impl_->svr.Put(pattern, h);
    } else if (method == "DELETE") {
        impl_->svr.Delete(pattern, h);
    } else if (method == "HEAD") {
        // 此版 httplib 无 Head() 注册；HEAD 请求内部按 GET 分发，路由到 GET 即可
        impl_->svr.Get(pattern, h);
    } else if (method == "OPTIONS") {
        impl_->svr.Options(pattern, h);
    } else if (method == "PATCH") {
        impl_->svr.Patch(pattern, h);
    }
}

void HttpServer::get(const std::string& pattern, HttpHandler handler)
{
    route("GET", pattern, std::move(handler));
}

void HttpServer::post(const std::string& pattern, HttpHandler handler)
{
    route("POST", pattern, std::move(handler));
}

void HttpServer::put(const std::string& pattern, HttpHandler handler)
{
    route("PUT", pattern, std::move(handler));
}

void HttpServer::del(const std::string& pattern, HttpHandler handler)
{
    route("DELETE", pattern, std::move(handler));
}

void HttpServer::set_fallback(HttpHandler handler)
{
    impl_->fallback = std::move(handler);
}

void HttpServer::use(HttpFilter filter)
{
    impl_->filters.push_back(std::move(filter));
}

void HttpServer::set_access_logger(HttpAccessLogger logger)
{
    impl_->access_logger = std::move(logger);
}

// ---------------- 健康检查 ----------------

void HttpServer::set_liveness_handler(HttpHandler handler)
{
    std::lock_guard<std::mutex> lock(impl_->health_mu);
    impl_->liveness = std::move(handler);
}

void HttpServer::add_readiness_check(const std::string& name, HealthCheck check)
{
    std::lock_guard<std::mutex> lock(impl_->health_mu);
    for (std::size_t i = 0; i < impl_->readiness.size(); ++i) {
        if (impl_->readiness[i].name == name) {
            impl_->readiness[i].check = std::move(check);  // 重名覆盖，位置保序
            return;
        }
    }
    Impl::ReadinessEntry entry;
    entry.name = name;
    entry.check = std::move(check);
    impl_->readiness.push_back(std::move(entry));
}

void HttpServer::clear_readiness_checks()
{
    std::lock_guard<std::mutex> lock(impl_->health_mu);
    impl_->readiness.clear();
}

void HttpServer::enable_health_endpoints()
{
    // 显式路由优先：同路径 GET 已注册时不接管内建版
    const bool have_healthz =
        std::find(impl_->get_routes.begin(), impl_->get_routes.end(),
                  "/healthz") != impl_->get_routes.end();
    if (!impl_->healthz_registered && !have_healthz) {
        impl_->svr.Get("/healthz",
                       [this](const httplib::Request&, httplib::Response& res) {
            HttpHandler handler;
            {
                std::lock_guard<std::mutex> lock(impl_->health_mu);
                handler = impl_->liveness;
            }
            if (handler) {
                HttpRequest hreq;  // 探针无业务上下文：只给 method/path
                hreq.method = "GET";
                hreq.path = "/healthz";
                apply_reply(handler(hreq), res);
            } else {
                apply_reply(HttpReply::text(200, "ok"), res);
            }
        });
        impl_->healthz_registered = true;
    }

    const bool have_readyz =
        std::find(impl_->get_routes.begin(), impl_->get_routes.end(),
                  "/readyz") != impl_->get_routes.end();
    if (!impl_->readyz_registered && !have_readyz) {
        impl_->svr.Get("/readyz",
                       [this](const httplib::Request&, httplib::Response& res) {
            std::vector<Impl::ReadinessEntry> checks;
            {
                std::lock_guard<std::mutex> lock(impl_->health_mu);
                // 锁外执行回调：检查里可再注册新检查，不自锁；也不阻塞其它探针
                checks = impl_->readiness;
            }
            JsonValue arr = JsonValue::array();
            bool all_ok = true;
            for (std::size_t i = 0; i < checks.size(); ++i) {
                std::string detail;
                const std::chrono::steady_clock::time_point t0 =
                    std::chrono::steady_clock::now();
                if (checks[i].check) {
                    try {
                        detail = checks[i].check();
                    } catch (const std::exception& e) {
                        detail = e.what();
                    } catch (...) {
                        detail = "health check threw";
                    }
                }
                const std::int64_t elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
                const bool ok = detail.empty();
                if (!ok) {
                    all_ok = false;
                }
                JsonValue item;
                item["name"] = checks[i].name;
                item["status"] = ok ? "ok" : "fail";
                item["detail"] = detail;
                item["elapsed_ms"] = elapsed_ms;
                arr.push_back(item);
            }
            JsonValue doc;
            doc["status"] = all_ok ? "ok" : "unavailable";
            doc["checks"] = arr;
            apply_reply(HttpReply::json(all_ok ? 200 : 503,
                                        to_json_string(doc)),
                        res);
        });
        impl_->readyz_registered = true;
    }
}

// ---------------- 生命周期 ----------------

bool HttpServer::start_background(int port)
{
    // port==0 时用配置端口（默认 0 = 系统分配）
    int p = port;
    if (p == 0) {
        p = impl_->configured_port;
    }
    return impl_->listen(p);
}

bool HttpServer::wait_until_ready(int timeout_ms)
{
    const int step_ms = 10;
    for (int waited = 0; waited < timeout_ms; waited += step_ms) {
        if (impl_->ready) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return impl_->ready;
}

void HttpServer::stop()
{
    if (!impl_) return;
    if (impl_->svr.is_running()) {
        impl_->svr.stop();  // 关监听 fd，accept 循环退出后 is_running_ 复位
        // 等内部线程真正收尾（stop 只发信号），避免析构竞态
        for (int i = 0; i < 300 && impl_->svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

bool HttpServer::is_running() const
{
    return impl_->svr.is_running();
}

int HttpServer::port() const
{
    if (impl_->svr.is_running() && impl_->actual_port > 0) {
        return impl_->actual_port;
    }
    return impl_->configured_port;
}

const std::string& HttpServer::last_error() const
{
    return impl_->error_text;
}

// ---------------- 配置化 ----------------

void HttpServer::apply_config(const ConfigFacade& config,
                              const std::string& key_prefix)
{
    const std::string port_key = key_prefix + "port";
    const std::string body_key = key_prefix + "max_body_bytes";

    if (!config.get(port_key).empty()) {
        const int port = config.get_int(port_key, -1);
        if (port > 0 && port < 65536) {
            impl_->configured_port = port;
        }
    }
    if (!config.get(body_key).empty()) {
        const std::int64_t bytes = config.get_int64(body_key, 0);
        if (bytes > 0) {
            impl_->svr.set_payload_max_length(static_cast<std::size_t>(bytes));
        }
    }
}

}  // namespace libmini
