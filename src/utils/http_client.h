#ifndef LIBMINI_HTTP_CLIENT_H
#define LIBMINI_HTTP_CLIENT_H

#include <map>
#include <string>

#include "export.h"

namespace libmini {

// HTTP 响应（传输层失败时 status == 0，error 非空）
struct HttpResponse
{
    int status = 0;         // HTTP 状态码；0 = 未到达 HTTP 层（连接/DNS/超时）
    std::string body;
    std::map<std::string, std::string> headers;  // 响应头（键统一小写）
    std::string error;      // 传输层错误描述（status == 0 时非空）

    // 2xx 视为成功
    bool ok() const { return status >= 200 && status < 300; }
};

// HTTP 客户端（基于 cpp-httplib 的一等公民封装）：
//
//   HttpClient c("127.0.0.1", 8080);
//   c.set_timeout_ms(3000);
//   HttpResponse r = c.get("/api/items", {{"page", "1"}, {"size", "20"}});
//   if (r.ok()) { use(r.body); }
//
//   HttpResponse p = c.post_json("/api/items", R"({"name":"x"})");
//
// 说明：
//   - 实例非线程安全：并发请求请为每个线程建独立实例（或外部加锁）；
//   - 本构建未启用 SSL 时，https:// 请求返回 status=0 且 error 说明原因。
class LIBMINI_API HttpClient {
public:
    // host + 端口（明文 HTTP）
    HttpClient(const std::string& host, int port);

    // base_url："http://host:port[/path]"；https:// 见上方 SSL 说明
    explicit HttpClient(const std::string& base_url);

    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // 连接/读/写统一超时（毫秒），默认 5000。须在请求前设置
    void set_timeout_ms(int timeout_ms);

    // 每次请求自动携带的头（可多次调用叠加；同名覆盖）
    void set_default_header(const std::string& name, const std::string& value);
    void clear_default_headers();

    // 是否跟随 3xx 重定向，默认 false
    void set_follow_location(bool follow);

    // 把 query 参数编码为 "k1=v1&k2=v2"（键值均 URL 编码）；空表返回空串
    static std::string build_query(
        const std::map<std::string, std::string>& params);

    // ---------------- 请求 ----------------
    // path 可自带 query（"/path?x=1"）；带 query 参数版本自动追加

    HttpResponse get(const std::string& path);
    HttpResponse get(const std::string& path,
                     const std::map<std::string, std::string>& query);

    HttpResponse post(const std::string& path, const std::string& body,
                      const std::string& content_type);
    HttpResponse post_json(const std::string& path, const std::string& body);

    HttpResponse put(const std::string& path, const std::string& body,
                     const std::string& content_type);

    HttpResponse del(const std::string& path);

    // 通用方法（method: "GET"/"POST"/"PATCH"...）
    HttpResponse request(const std::string& method, const std::string& path,
                         const std::string& body,
                         const std::string& content_type);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_HTTP_CLIENT_H
