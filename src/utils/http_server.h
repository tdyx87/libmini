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

// 就绪检查回调：返回空串 = 通过；返回文本 = 失败原因（写进 /readyz 响应体）。
// 在 /readyz 的请求线程上执行——必须快速、线程安全，别在里面做阻塞 IO
typedef std::function<std::string()> HealthCheck;

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

    // ---------------- 健康检查 ----------------

    // 自定义存活处理器（默认：恒 200 text "ok"）。运行期可安全切换——
    // 返回 503 即让负载均衡/K8s 摘流量（优雅停机前翻转）；读写内部加锁
    void set_liveness_handler(HttpHandler handler);

    // 注册就绪检查（保序输出；同名覆盖旧的，可在运行期增改）。回调逐个
    // 执行（全部跑、不短路——一次看全所有失败原因），抛异常按失败处理
    void add_readiness_check(const std::string& name, HealthCheck check);

    // 清空全部就绪检查（未注册任何检查时 /readyz 恒 200 = 视为就绪）
    void clear_readiness_checks();

    // 注册内建探针端点（可重复调用，幂等；已存在同路径 GET 路由时不注册，
    // 显式路由优先）。两个端点都绕过 use() 过滤器（探针不带业务鉴权头）：
    //   GET /healthz —— 存活：默认 200 "ok"，set_liveness_handler 可自定义
    //   GET /readyz  —— 就绪：执行全部就绪检查，响应为 JSON：
    //       200 {"status":"ok","checks":[{"name":..,"status":"ok",
    //            "detail":"","elapsed_ms":..}, ...]}
    //       503 {"status":"unavailable", ...}  任一检查失败即 503
    void enable_health_endpoints();

    // ---------------- TLS ----------------

    // 启用 HTTPS 监听：PEM 证书 + 私钥。须在 start_background 前调用；
    // 证书/配置在首次 start 创建监听对象时定型，启动过的实例不可再改
    //（改了下次 start 会报错，而非静默降级明文）。
    // 失败语义：证书文件缺失/PEM 损坏、或本构建未启用 OpenSSL 时
    // start_background 返回 false，原因见 last_error()
    void set_ssl_certificates(const std::string& cert_path,
                              const std::string& key_path);

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
