#ifndef LIBMINI_WEBSOCKET_H
#define LIBMINI_WEBSOCKET_H

#include <cstdint>
#include <functional>
#include <string>

#include "export.h"

namespace libmini {

// WebSocket（RFC 6455）客户端与服务端（零第三方依赖）。
// 在 tcp.h 的裸连接之上补齐两层协议：
//   1. HTTP Upgrade 握手（Sec-WebSocket-Key/Accept，SHA-1 + Base64）；
//   2. 标准帧协议——变长长度域、客户端掩码、分片重组、控制帧，
//      与浏览器（new WebSocket）可直接互通。
//
//   服务端：
//     WsServer server;
//     server.set_on_message([&](uint64_t conn, const std::string& msg) {
//         server.send(conn, "echo:" + msg);
//     });
//     server.start("127.0.0.1", 9001);
//
//   客户端：
//     WsClient ws;
//     ws.set_on_message([](const std::string& msg) { got(msg); });
//     ws.connect("127.0.0.1", 9001, "/chat");   // 同步：握手成功才返回 true
//     ws.send("hello");
//
// 语义（对齐 tcp.h 的回调约定）：
//   - 文本帧与二进制帧都经 on_message 投递（载荷按字节给出，不做
//     UTF-8 校验/转码）；需要区分类型时按业务约定自行判别；
//   - PING 自动回 PONG、CLOSE 握手自动应答，均不触发 on_message；
//   - 分片消息（continuation）在库内重组后一次性投递；
//   - 回调在库内部线程执行，可安全调用 send()/close()；
//   - 同步 connect() 受 connect_timeout_ms + handshake_timeout_ms 约束，
//     返回 true 表示握手已完成、可立即收发；
//   - 不提供断线自动重连（浏览器语义里重连是应用层策略），
//     断开后可直接再次 connect()。

struct LIBMINI_API WsConfig {
    int connect_timeout_ms = 5000;       // TCP connect 超时
    int handshake_timeout_ms = 5000;     // HTTP Upgrade 握手读写超时
    int max_message_bytes = 16 * 1024 * 1024;  // 单条消息上限（含分片重组，
                                              // 超限视为协议违规断开）

    // 非法项逐条经 LogFacade 记 warn，任一命中返回 false：
    //   - 任一毫秒值 < 0；- max_message_bytes <= 0
    // start/connect 前自动调用
    bool validate() const;
};

// ---------------- 客户端 ----------------

class LIBMINI_API WsClient
{
public:
    WsClient();
    explicit WsClient(const WsConfig& config);
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // 同步连接 + Upgrade 握手。重复 connect 会先关闭旧连接。
    // path 形如 "/chat"（空串按 "/" 处理）
    bool connect(const std::string& host, std::uint16_t port,
                 const std::string& path = "/");

    // 主动关闭：尽力发送 CLOSE 帧后断开（幂等），触发 on_disconnect
    void close();

    // 发送文本帧；false = 连接不可用
    bool send(const std::string& payload);
    // 发送二进制帧
    bool send_binary(const std::string& payload);

    bool is_connected() const;

    void set_on_message(std::function<void(const std::string& payload)> handler);
    void set_on_connect(std::function<void()> handler);
    void set_on_disconnect(std::function<void(const std::string& reason)> handler);

    // 对端 "host:port"（未连接返回空）
    std::string peer() const;

private:
    struct Impl;
    Impl* impl_;
};

// ---------------- 服务端 ----------------

class LIBMINI_API WsServer
{
public:
    WsServer();
    explicit WsServer(const WsConfig& config);
    ~WsServer();

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // 后台开始监听。port 传 0 由系统分配（port() 取实际值）。
    // on_connect 在 Upgrade 握手成功后触发（不是 TCP 一 accept 就触发）
    bool start(const std::string& host, std::uint16_t port);

    // 停止监听并断开全部连接（幂等，阻塞至所有线程退出）
    void stop();

    // 向指定连接发送文本帧；连接不存在/已断开则静默丢弃
    void send(std::uint64_t conn_id, const std::string& payload);
    void send_binary(std::uint64_t conn_id, const std::string& payload);
    // 向所有已握手连接广播文本帧
    void broadcast(const std::string& payload);

    // 主动断开某连接（尽力发 CLOSE 帧）
    void disconnect(std::uint64_t conn_id);

    void set_on_message(
        std::function<void(std::uint64_t conn_id, const std::string& payload)> handler);
    void set_on_connect(std::function<void(std::uint64_t conn_id)> handler);
    void set_on_disconnect(
        std::function<void(std::uint64_t conn_id, const std::string& reason)> handler);

    // 实际监听端口（未 start 返回 0）
    std::uint16_t port() const;
    bool is_running() const;
    // 当前已建立（含握手中）的连接数
    std::size_t connection_count() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_WEBSOCKET_H
