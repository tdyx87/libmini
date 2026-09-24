#ifndef LIBMINI_NET_ADDR_H
#define LIBMINI_NET_ADDR_H

#include <cstdint>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// socket 地址工具：端点解析、域名解析、地址格式化。
// 统一 RPC 的 "host:port" 解析与 TCP 模块的 DNS 查询，支持 IPv6。

// ---------------- 端点解析 ----------------

// "host:port" → host + port。
//   "192.168.1.5:8080"  → host="192.168.1.5", port=8080
//   "[::1]:8080"        → host="::1",         port=8080   （IPv6 括号形式）
//   "myhost:8080"       → host="myhost",      port=8080   （域名）
//   "8080"              → host="",            port=8080   （纯端口）
//   "[::1]"             → host="::1",         port=0      （括号内可无端口）
//   "myhost"            → host="myhost",      port=0      （无冒号视为主机名）
// 解析失败（端口非数字/越界）返回 false，输出参数置空。
// 注意： "::1" 不带括号的裸 IPv6 无法与 "host:port" 区分，需用括号形式。
bool parse_endpoint(const std::string& endpoint, std::string& host, int& port);

// 便捷封装：失败时 host="0.0.0.0"、port=0（服务端监听默认口径）
void parse_endpoint_or_default(const std::string& endpoint,
                               std::string& host, int& port);

// ---------------- 域名解析 ----------------

// 解析出的地址项（Pv4 优先，与多数应用直觉一致）
struct NetAddrEntry {
    std::string ip;         // 点分 IPv4 或压缩 IPv6 文本
    bool is_ipv6 = false;
    std::uint16_t port = 0;  // resolve_host 传 nullptr 时为 0
};

// 主机名 → 地址列表（IPv4 字面量直接返回；域名走 getaddrinfo）。
// service_port 非空时填入每项 port。失败/无结果返回空表。
// 首次调用会初始化 Winsock（Windows）。
std::vector<NetAddrEntry> resolve_host(const std::string& host,
                                       const std::string& service_port);

// 仅取第一个 IPv4（与 tcp.cpp 的 resolve_ipv4 语义一致）；
// 失败返回 0（INADDR_ANY 语义，与既有调用方约定一致）。
std::uint32_t resolve_ipv4_net(const std::string& host);

// ---------------- IPv4 格式化 ----------------

// 网络序 uint32 → "a.b.c.d"
std::string ipv4_to_string(std::uint32_t net_addr);

// "a.b.c.d" → 网络序 uint32；非法返回 0
std::uint32_t ipv4_from_string(const std::string& ip);

}  // namespace libmini

#endif  // LIBMINI_NET_ADDR_H
