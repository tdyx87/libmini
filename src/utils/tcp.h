#ifndef LIBMINI_TCP_H
#define LIBMINI_TCP_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <string>

#include "libmini.h"

namespace libmini {

// 裸 TCP 长连接封装（对标 asio 的常用子集，零第三方依赖）。
// 在字节流之上提供「4 字节小端长度 + 类型 + 载荷」的帧协议，业务无需处理粘包：
//
//   服务端：
//     TcpServer server;
//     server.set_on_message([&](uint64_t conn, const std::string& msg) {
//         server.send(conn, "pong:" + msg);
//     });
//     server.start("127.0.0.1", 9000);
//
//   客户端：
//     TcpClient client;
//     client.set_on_message([&](const std::string& msg) { got(msg); });
//     client.connect("127.0.0.1", 9000);
//     client.send("ping");
//
// 帧类型（自动处理，业务回调只见到数据帧）：
//   0x01 DATA  —— 业务数据（双方）
//   0x02 PING  —— 心跳（客户端→服务端），服务端自动回 PONG，不触发 on_message
//   0x03 PONG  —— 心跳应答（服务端→客户端），仅刷新存活时间
//
// 其他语义：
//   - 所有 IO 回调在库内部线程上执行，回调里不要做长时间阻塞；
//   - send() 线程安全（帧级串行化）；
//   - 回调里调用 server.stop() / client.close() 允许（延迟到回调返回后执行）。

struct TcpConfig {
    int connect_timeout_ms = 5000;      // 客户端 connect 超时
    int max_frame_bytes = 16 * 1024 * 1024;  // 单帧载荷上限（防异常长度炸弹）
    int heartbeat_interval_ms = 0;      // >0 启用心跳：空闲超间隔后自动发 PING
    int heartbeat_timeout_ms = 0;       // >0 且启用心跳：超过此值无任何入站帧判死
                                        // （0 = 与 interval*3 取大）
    bool auto_reconnect = false;        // 客户端断线自动重连（指数退避）
    int reconnect_base_delay_ms = 200;  // 重连退避基数
    int reconnect_max_delay_ms = 5000;  // 重连退避上限
};

// ---------------- 客户端 ----------------

class LIBMINI_API TcpClient
{
public:
    TcpClient();
    explicit TcpClient(const TcpConfig& config);
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // 同步连接（受 connect_timeout_ms 约束）。重复 connect 会先关闭旧连接
    bool connect(const std::string& host, std::uint16_t port);

    // 异步连接：在内部线程上执行与 connect() 相同的流程（解析/DNS/
    // 握手受 connect_timeout_ms 约束），不阻塞调用线程。返回的 future
    // 就绪即连接完成（true）或失败（false）；连接成功后 on_connect
    // 回调照常触发。与 connect() 一样会先关闭旧连接。
    // 线程安全：析构前未完成的连接尝试会正常结束（析构等待内部线程），
    // future 不会悬空
    std::future<bool> async_connect(const std::string& host, std::uint16_t port);

    // 主动关闭（幂等）。断连回调会触发
    void close();

    // 发送数据帧；false = 连接不可用（auto_reconnect 时已安排重连）
    bool send(const std::string& payload);

    // 连接是否可用（自最后一次 IO 错误/关闭以来）
    bool is_connected() const;

    // 配置回调（可在连接前或运行中设置）
    void set_on_message(std::function<void(const std::string& payload)> handler);
    void set_on_connect(std::function<void()> handler);
    void set_on_disconnect(std::function<void(const std::string& reason)> handler);

    const TcpConfig& config() const;
    // 对端地址 "host:port"（未连接返回空）
    std::string peer() const;

private:
    void session_loop(std::uint64_t conn_id, std::intptr_t fd_handle);
    bool connect_impl(const std::string& host, std::uint16_t port);

    struct Impl;
    Impl* impl_;
};

// ---------------- 服务端 ----------------

class LIBMINI_API TcpServer
{
public:
    TcpServer();
    explicit TcpServer(const TcpConfig& config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 后台开始监听。port 传 0 让系统分配（用 port() 取实际值）
    bool start(const std::string& host, std::uint16_t port);

    // 停止监听并断开全部连接（幂等，阻塞至所有线程退出）
    void stop();

    // 向指定连接发送数据帧；conn 已断开则静默丢弃
    void send(std::uint64_t conn_id, const std::string& payload);

    // 向所有活跃连接广播数据帧
    void broadcast(const std::string& payload);

    // 主动断开某连接
    void disconnect(std::uint64_t conn_id);

    void set_on_message(std::function<void(std::uint64_t conn_id, const std::string& payload)> handler);
    void set_on_connect(std::function<void(std::uint64_t conn_id)> handler);
    void set_on_disconnect(std::function<void(std::uint64_t conn_id, const std::string& reason)> handler);

    // 实际监听端口（start 后有效；未 start 返回 0）
    std::uint16_t port() const;
    bool is_running() const;
    // 当前活跃连接数
    std::size_t connection_count() const;

private:
    void session_loop(std::uint64_t conn_id, std::intptr_t fd_handle);

    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_TCP_H
