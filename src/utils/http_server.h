#ifndef LIBMINI_HTTP_SERVER_H
#define LIBMINI_HTTP_SERVER_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {

class ConfigFacade;

// ---------------- 请求 / 响应类型（与 HttpClient 的 HttpResponse 对称） ----------------

// 服务端收到的请求。headers 键统一小写；query 已按 & 和 = 拆分并 URL 解码；
// params 是路由模式里的路径参数（如 "/items/:id" 匹配 "/items/42" → params["id"]="42"）。
struct HttpRequest
{
    std::string method;                            // "GET" / "POST" / ...
    std::string path;                              // 去掉 query 后的路径
    std::map<std::string, std::string> params;     // 路径参数
    std::map<std::string, std::string> query;      // 查询串参数（URL 解码后）
    std::map<std::string, std::string> headers;    // 请求头（键小写）
    std::string body;                              // 原始请求体
    std::string remote_addr;                       // 对端地址（"ip:port"，可能为空）

    // 路径参数取值（缺失返回空串）；req.params["id"] 的便捷形式
    LIBMINI_API std::string param(const std::string& name) const;
};

// 服务端返回的响应。状态码默认 200；未显式设置的 Content-Type 由
// HttpReply::text / ::json 补上，手动塞 headers 同样有效。
struct HttpReply
{
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;

    LIBMINI_API static HttpReply text(int status, const std::string& body);
    LIBMINI_API static HttpReply json(int status, const std::string& json_body);
    LIBMINI_API static HttpReply error(int status, const std::string& message);
};

// 处理器：返回要写给客户端的响应。在 httplib 的工作线程上并发执行，
// 必须线程安全（无共享可变状态或自行加锁）。
typedef std::function<HttpReply(const HttpRequest&)> HttpHandler;

// 前置过滤器：请求进入路由匹配前调用。返回 false 表示已生成响应、
// 短路结束（用于鉴权、限流、CORS 预检等）。可叠加多个，按注册顺序执行。
typedef std::function<bool(HttpRequest&, HttpReply&)> HttpFilter;

// 访问日志钩子：响应写回后调用（对齐 RpcServer 的请求日志形态）。
// elapsed_ms 为处理耗时；wire 失败（客户端断连）时 status 为实际写入值。
typedef std::function<void(const HttpRequest&, const HttpReply&,
                           std::int64_t elapsed_ms)>
    HttpAccessLogger;

// HTTP 服务器（基于 cpp-httplib 的一等公民封装，与 HttpClient 对称）：
//
//   HttpServer server;
//   server.get("/hello/:name", [](const HttpRequest& req) {
//       return HttpReply::json(200,
//           "{\"hello\":\"" + req.param("name") + "\"}");
//   });
//   server.start_background(8080);   // 或 start_background(0) 由系统分配端口
//   if (server.wait_until_ready()) { use(server.port()); }
//   ...
//   server.stop();
//
// 配置化（可选）：
//   server.apply_config(cfg, "web.");   // web.port / web.max_body_bytes
//
// 说明：
//   - 处理器并发执行于 httplib 内部线程池，注册与 start 须在任何请求前完成；
//   - 路由按「先精确后模式、模式内先注册先匹配」的顺序命中；
//   - 请求体超过 max_body_bytes（默认 64 MiB）直接返回 413。
class LIBMINI_API HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // ---------------- 路由注册（start 前任意顺序） ----------------
    void get(const std::string& pattern, HttpHandler handler);
    void post(const std::string& pattern, HttpHandler handler);
    void put(const std::string& pattern, HttpHandler handler);
    void del(const std::string& pattern, HttpHandler handler);
    // 任意方法的通用注册
    void route(const std::string& method, const std::string& pattern,
               HttpHandler handler);

    // 未命中任何路由时的兜底处理器（默认返回 404 JSON）
    void set_fallback(HttpHandler handler);

    // 前置过滤器（按注册顺序执行；返回 false 短路）
    void use(HttpFilter filter);

    // 访问日志钩子（每次请求收尾时回调一次）
    void set_access_logger(HttpAccessLogger logger);

    // ---------------- 生命周期 ----------------

    // 在另一线程启动监听并立即返回；port=0 时由系统分配，实际端口经 port() 取。
    // 返回 false 表示端口占用等启动失败（错误见 last_error()）。
    bool start_background(int port);

    // 等待监听就绪（默认 5s）。port=0 时须等它返回后 port() 才有值
    bool wait_until_ready(int timeout_ms = 5000);

    // 停止监听并等待内部线程收尾；未启动时为无害调用
    void stop();

    bool is_running() const;
    int port() const;                      // start 前返回入参 port（0 = 未定）
    const std::string& last_error() const; // 最近一次启动失败的错误描述

    // ---------------- 配置化（对齐 RpcServer::apply_config） ----------------

    // 从分层配置门面按前缀取值应用。可配键（前缀后）：
    //   port            — 监听端口（start 前生效）
    //   max_body_bytes  — 请求体大小上限（默认 64 MiB）
    // 未出现的键保持当前值，未知键静默跳过。
    void apply_config(const ConfigFacade& config,
                      const std::string& key_prefix);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace libmini

#endif  // LIBMINI_HTTP_SERVER_H
