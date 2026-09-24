#include "net_addr.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace libmini {

namespace {

// 进程级 Winsock 引导（Windows 需要；POSIX 空实现）。
// 引用计数式：与 tcp 模块各自的 WSAStartup 互不干扰。
void platform_net_startup()
{
#ifdef _WIN32
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        static struct Cleaner {
            ~Cleaner() { ::WSACleanup(); }
        } cleaner;
    });
#endif
}

// "a.b.c.d" → 网络序 uint32 值（与 inet_pton/htonl 的 uint32 约定一致）。
// 严格校验：纯数字、无前导零、每段 0-255、恰好 4 段。
bool parse_ipv4_literal(const std::string& ip, std::uint32_t& net_out)
{
    int parts[4];
    int idx = 0;
    std::string cur;
    for (std::size_t i = 0; i <= ip.size(); ++i) {
        if (i == ip.size() || ip[i] == '.') {
            if (cur.empty() || idx >= 4) {
                return false;
            }
            for (std::size_t j = 0; j < cur.size(); ++j) {
                if (cur[j] < '0' || cur[j] > '9') {
                    return false;
                }
            }
            if (cur.size() > 1 && cur[0] == '0') {
                return false;
            }
            const long v = std::strtol(cur.c_str(), nullptr, 10);
            if (v < 0 || v > 255) {
                return false;
            }
            parts[idx++] = static_cast<int>(v);
            cur.clear();
            continue;
        }
        cur.push_back(ip[i]);
    }
    if (idx != 4) {
        return false;
    }
    // 主机序拼值再转网络序：内存字节序即 [p0,p1,p2,p3]（与 s_addr 约定一致）
    const std::uint32_t host_val = (static_cast<std::uint32_t>(parts[0]) << 24) |
                                   (static_cast<std::uint32_t>(parts[1]) << 16) |
                                   (static_cast<std::uint32_t>(parts[2]) << 8) |
                                   static_cast<std::uint32_t>(parts[3]);
    net_out = ::htonl(host_val);
    return true;
}

}  // namespace

bool parse_endpoint(const std::string& endpoint, std::string& host, int& port)
{
    host.clear();
    port = 0;
    if (endpoint.empty()) {
        return false;
    }

    // IPv6 括号形式：[xxx] 或 [xxx]:port
    if (endpoint[0] == '[') {
        const std::size_t close = endpoint.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = endpoint.substr(1, close - 1);
        if (host.empty()) {
            return false;
        }
        if (close + 1 == endpoint.size()) {
            return true;  // 无端口，port 保持 0
        }
        if (endpoint[close + 1] != ':') {
            return false;
        }
        const std::string port_str = endpoint.substr(close + 2);
        if (port_str.empty()) {
            return true;
        }
        char* endp = nullptr;
        const long v = std::strtol(port_str.c_str(), &endp, 10);
        if (endp == nullptr || *endp != '\0' || v < 0 || v > 65535) {
            return false;
        }
        port = static_cast<int>(v);
        return true;
    }

    // 一般形式：按最后一个冒号切分
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
        // 无冒号：纯端口号 or 主机名
        char* endp = nullptr;
        const long v = std::strtol(endpoint.c_str(), &endp, 10);
        if (endp != nullptr && *endp == '\0' && v >= 0 && v <= 65535) {
            port = static_cast<int>(v);
            return true;
        }
        host = endpoint;
        return true;
    }

    host = endpoint.substr(0, colon);
    const std::string port_str = endpoint.substr(colon + 1);
    if (port_str.empty()) {
        // "host:" 形式：主机有效、端口缺省 0
        return !host.empty();
    }
    char* endp = nullptr;
    const long v = std::strtol(port_str.c_str(), &endp, 10);
    if (endp == nullptr || *endp != '\0' || v < 0 || v > 65535) {
        return false;
    }
    port = static_cast<int>(v);
    if (host.empty()) {
        host = "0.0.0.0";
    }
    return true;
}

void parse_endpoint_or_default(const std::string& endpoint,
                               std::string& host, int& port)
{
    if (!parse_endpoint(endpoint, host, port)) {
        host = "0.0.0.0";
        port = 0;
    }
    // 与 RPC 既有语义一致：显式空 host 视为监听所有网卡
    if (host.empty()) {
        host = "0.0.0.0";
    }
}

std::vector<NetAddrEntry> resolve_host(const std::string& host,
                                       const std::string& service_port)
{
    std::vector<NetAddrEntry> out;
    if (host.empty()) {
        return out;
    }

    platform_net_startup();

    ::addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;  // IPv4 + IPv6 都要
    hints.ai_socktype = SOCK_STREAM;

    ::addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(),
                      service_port.empty() ? nullptr : service_port.c_str(),
                      &hints, &result) != 0 ||
        result == nullptr) {
        return out;
    }

    // 两趟追加：IPv4 在前，IPv6 在后
    std::vector<NetAddrEntry> v6;
    for (::addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        NetAddrEntry entry;
        char text[INET6_ADDRSTRLEN] = {0};
        if (ai->ai_family == AF_INET) {
            const auto* sa = reinterpret_cast<const ::sockaddr_in*>(ai->ai_addr);
            ::inet_ntop(AF_INET, &sa->sin_addr, text, sizeof(text));
            entry.ip = text;
            entry.is_ipv6 = false;
            entry.port = ::ntohs(sa->sin_port);
        } else if (ai->ai_family == AF_INET6) {
            const auto* sa = reinterpret_cast<const ::sockaddr_in6*>(ai->ai_addr);
            ::inet_ntop(AF_INET6, &sa->sin6_addr, text, sizeof(text));
            entry.ip = text;
            entry.is_ipv6 = true;
            entry.port = ::ntohs(sa->sin6_port);
        } else {
            continue;
        }
        if (entry.is_ipv6) {
            v6.push_back(entry);
        } else {
            out.push_back(entry);
        }
    }
    ::freeaddrinfo(result);

    for (std::size_t i = 0; i < v6.size(); ++i) {
        out.push_back(v6[i]);
    }
    return out;
}

std::uint32_t resolve_ipv4_net(const std::string& host)
{
    // 字面量快速路径（无需 Winsock）
    std::uint32_t net = 0;
    if (parse_ipv4_literal(host, net)) {
        return net;
    }
    const std::vector<NetAddrEntry> entries = resolve_host(host, std::string());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].is_ipv6) {
            return ipv4_from_string(entries[i].ip);
        }
    }
    return 0;
}

std::string ipv4_to_string(std::uint32_t net_addr)
{
    char buf[16];
    const std::uint32_t h = ::ntohl(net_addr);
    const unsigned b0 = (h >> 24) & 0xff;
    const unsigned b1 = (h >> 16) & 0xff;
    const unsigned b2 = (h >> 8) & 0xff;
    const unsigned b3 = h & 0xff;
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b0, b1, b2, b3);
    return std::string(buf);
}

std::uint32_t ipv4_from_string(const std::string& ip)
{
    std::uint32_t net = 0;
    if (!parse_ipv4_literal(ip, net)) {
        return 0;
    }
    return net;
}

}  // namespace libmini
