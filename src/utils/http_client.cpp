#include "http_client.h"

#include <cstdlib>
#include <memory>

#include <httplib.h>

#include "encoding.h"

namespace libmini {

namespace {

// httplib 头条目（区分大小写的键）转小写键 map
std::map<std::string, std::string> lower_headers(const httplib::Headers& h)
{
    std::map<std::string, std::string> out;
    for (const auto& kv : h) {
        std::string key = kv.first;
        for (char& c : key) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        out[key] = kv.second;
    }
    return out;
}

// URL 解析："http[s]://host[:port][/path]"。https 标志通过 is_https 返回
bool parse_base_url(const std::string& url, std::string& host, int& port,
                    bool& is_https)
{
    std::string rest = url;
    is_https = false;
    if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    } else if (rest.rfind("https://", 0) == 0) {
        rest = rest.substr(8);
        is_https = true;
    } else {
        return false;  // 必须带 scheme
    }
    const std::size_t slash = rest.find('/');
    const std::string hostport =
        (slash == std::string::npos) ? rest : rest.substr(0, slash);
    if (hostport.empty()) {
        return false;
    }
    if (!hostport.empty() && hostport[0] == '[') {
        return false;  // IPv6 字面量未支持
    }
    const std::size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        char* endp = nullptr;
        const long p = std::strtol(hostport.c_str() + colon + 1, &endp, 10);
        if (endp == hostport.c_str() + colon + 1 || *endp != '\0' || p <= 0 ||
            p > 65535) {
            return false;
        }
        port = static_cast<int>(p);
    } else {
        host = hostport;
        port = is_https ? 443 : 80;
    }
    return !host.empty();
}

HttpResponse make_error(const std::string& msg)
{
    HttpResponse r;
    r.status = 0;
    r.error = msg;
    return r;
}

// httplib::Result → HttpResponse 统一转换
HttpResponse convert_result(const httplib::Result& result)
{
    HttpResponse r;
    if (!result) {
        r.status = 0;
        r.error = httplib::to_string(result.error());
        return r;
    }
    r.status = result->status;
    r.body = result->body;
    r.headers = lower_headers(result->headers);
    return r;
}

}  // namespace

struct HttpClient::Impl
{
    std::unique_ptr<httplib::Client> client;
    std::map<std::string, std::string> default_headers;
    int timeout_ms = 5000;
    bool follow_location = false;

    void apply_common()
    {
        const time_t sec = timeout_ms / 1000;
        const time_t usec = (timeout_ms % 1000) * 1000;
        client->set_connection_timeout(sec, usec);
        client->set_read_timeout(sec, usec);
        client->set_write_timeout(sec, usec);
        client->set_follow_location(follow_location);
    }

    // 拼默认头（在请求前调用 set_headers 语义）
    void add_default_headers(httplib::Headers& headers) const
    {
        for (const auto& kv : default_headers) {
            headers.emplace(kv.first, kv.second);
        }
    }
};

HttpClient::HttpClient(const std::string& host, int port)
    : impl_(new Impl)
{
    impl_->client.reset(new httplib::Client(host, port));
    impl_->apply_common();
}

HttpClient::HttpClient(const std::string& base_url) : impl_(new Impl)
{
    std::string host;
    int port = 0;
    bool is_https = false;
    if (!parse_base_url(base_url, host, port, is_https)) {
        impl_->client.reset();  // 延迟到请求时报告错误
        return;
    }
    if (is_https) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        impl_->client.reset(new httplib::Client(host, port, nullptr,
                                                "https"));
#else
        // 未启用 SSL：client 置空，请求时统一报错
        impl_->client.reset();
        return;
#endif
    }
    if (!impl_->client) {
        impl_->client.reset(new httplib::Client(host, port));
    }
    impl_->apply_common();
}

HttpClient::~HttpClient()
{
    delete impl_;
}

void HttpClient::set_timeout_ms(int timeout_ms)
{
    impl_->timeout_ms = timeout_ms;
    if (impl_->client) {
        impl_->apply_common();
    }
}

void HttpClient::set_default_header(const std::string& name,
                                    const std::string& value)
{
    impl_->default_headers[name] = value;
}

void HttpClient::clear_default_headers()
{
    impl_->default_headers.clear();
}

void HttpClient::set_follow_location(bool follow)
{
    impl_->follow_location = follow;
    if (impl_->client) {
        impl_->client->set_follow_location(follow);
    }
}

std::string HttpClient::build_query(
    const std::map<std::string, std::string>& params)
{
    std::string q;
    for (const auto& kv : params) {
        if (!q.empty()) q += '&';
        q += UrlEncode::encode(kv.first);
        q += '=';
        q += UrlEncode::encode(kv.second);
    }
    return q;
}

HttpResponse HttpClient::get(const std::string& path)
{
    return get(path, std::map<std::string, std::string>());
}

HttpResponse HttpClient::get(
    const std::string& path, const std::map<std::string, std::string>& query)
{
    std::string full = path;
    const std::string q = build_query(query);
    if (!q.empty()) {
        full += (full.find('?') == std::string::npos) ? '?' : '&';
        full += q;
    }
    if (!impl_->client) {
        return make_error("client not usable: bad base URL or SSL unavailable");
    }
    httplib::Headers headers;
    impl_->add_default_headers(headers);
    const httplib::Result result = impl_->client->Get(full.c_str(), headers);
    return convert_result(result);
}

HttpResponse HttpClient::post(const std::string& path, const std::string& body,
                              const std::string& content_type)
{
    return request("POST", path, body, content_type);
}

HttpResponse HttpClient::post_json(const std::string& path,
                                   const std::string& body)
{
    return post(path, body, "application/json");
}

HttpResponse HttpClient::put(const std::string& path, const std::string& body,
                             const std::string& content_type)
{
    return request("PUT", path, body, content_type);
}

HttpResponse HttpClient::del(const std::string& path)
{
    return request("DELETE", path, std::string(), std::string());
}

HttpResponse HttpClient::request(const std::string& method,
                                 const std::string& path,
                                 const std::string& body,
                                 const std::string& content_type)
{
    if (!impl_->client) {
        return make_error("client not usable: bad base URL or SSL unavailable");
    }
    httplib::Headers headers;
    impl_->add_default_headers(headers);
    httplib::Result result;
    if (method == "GET") {
        result = impl_->client->Get(path.c_str(), headers);
    } else if (method == "POST") {
        result = impl_->client->Post(path.c_str(), headers, body,
                                     content_type.c_str());
    } else if (method == "PUT") {
        result = impl_->client->Put(path.c_str(), headers, body,
                                    content_type.c_str());
    } else if (method == "DELETE") {
        if (body.empty()) {
            result = impl_->client->Delete(path.c_str(), headers);
        } else {
            result = impl_->client->Delete(path.c_str(), headers, body,
                                           content_type.c_str());
        }
    } else if (method == "HEAD") {
        result = impl_->client->Head(path.c_str(), headers);
    } else if (method == "OPTIONS") {
        result = impl_->client->Options(path.c_str(), headers);
    } else if (method == "PATCH") {
        result = impl_->client->Patch(path.c_str(), headers, body,
                                      content_type.c_str());
    } else {
        return make_error("unsupported method: " + method);
    }
    return convert_result(result);
}

}  // namespace libmini
