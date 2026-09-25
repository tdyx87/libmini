#include "tcp.h"
#include "net_addr.h"
#include "timer_wheel.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

#include "timer_wheel.h"

namespace libmini {

std::uint64_t steady_now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {

// ---------------- 帧协议 ----------------
// [4B 小端长度(载荷+1, 含 type)] [1B type] [payload]
constexpr std::uint8_t kFrameData = 0x01;
constexpr std::uint8_t kFramePing = 0x02;
constexpr std::uint8_t kFramePong = 0x03;

void put_u32_le(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 24) & 0xff));
}

std::uint32_t get_u32_le(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(u[0]) | (static_cast<std::uint32_t>(u[1]) << 8) |
           (static_cast<std::uint32_t>(u[2]) << 16) | (static_cast<std::uint32_t>(u[3]) << 24);
}

std::string make_frame(std::uint8_t type, const std::string& payload)
{
    std::string frame;
    frame.reserve(payload.size() + 5);
    put_u32_le(frame, static_cast<std::uint32_t>(payload.size() + 1));
    frame.push_back(static_cast<char>(type));
    frame.append(payload);
    return frame;
}

// ---------------- 平台层 ----------------

void net_startup()
{
#ifdef _WIN32
    WSADATA data;
    ::WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

void net_cleanup()
{
#ifdef _WIN32
    ::WSACleanup();
#endif
}

void close_socket(SocketHandle fd)
{
    if (fd != kInvalidSocket) {
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }
}

void shutdown_socket(SocketHandle fd)
{
    if (fd == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

// ---------------- 共享 TimerWheel（进程级单例）----------------
// 客户端与 wheel_liveness 模式的服务端共用一条轮线程（50ms 节拍）。
// 会话级超时从「每会话 100ms 轮询」变为「到期才触发」，空闲连接零唤醒；
// 万级连接下 CPU 占用不随连接数增长。
TimerWheel& shared_timer_wheel()
{
    static TimerWheel wheel(std::chrono::milliseconds(50));
    return wheel;
}

// 会话活性记录：入站数据时间戳（wheel 模式下由会话线程原子更新，
// 轮任务到期时复查决定保活还是判死）
struct LivenessStamp
{
    std::atomic<std::uint64_t> last_inbound_ms;
    void touch() { last_inbound_ms.store(steady_now_ms(), std::memory_order_relaxed); }
    std::uint64_t get() const { return last_inbound_ms.load(std::memory_order_relaxed); }
};

// 域名解析（IPv4 字面量与主机名），失败返回 0。
// 统一走 net_addr 模块（支持 IPv6 与主机名，IPv4 语义一致）
std::uint32_t resolve_ipv4(const std::string& host)
{
    return resolve_ipv4_net(host);
}

// 带超时的非阻塞 connect。返回：1=成功 0=超时 -1=错误
int connect_with_timeout(SocketHandle fd, std::uint32_t net_addr, std::uint16_t port, int timeout_ms)
{
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = net_addr;
    sa.sin_port = htons(port);

    // 非阻塞 connect + 超时等待
#ifdef _WIN32
    u_long mode = 1;
    ::ioctlsocket(fd, FIONBIO, &mode);
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    const int rc = ::connect(fd, reinterpret_cast<::sockaddr*>(&sa), sizeof(sa));
    if (rc == 0) {
        return 1;
    }
#ifdef _WIN32
    if (::WSAGetLastError() != WSAEWOULDBLOCK) {
        return -1;
    }
    ::fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    ::timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int sr = ::select(0, nullptr, &wset, nullptr, &tv);
#else
    if (errno != EINPROGRESS) {
        return -1;
    }
    ::pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int sr = ::poll(&pfd, 1, timeout_ms);
#endif
    if (sr == 0) {
        return 0;
    }
    if (sr < 0) {
        return -1;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len) != 0 ||
        so_error != 0) {
        return -1;
    }
    // 连接完成：恢复阻塞模式（send/recv 语义简单）
#ifdef _WIN32
    mode = 0;
    ::ioctlsocket(fd, FIONBIO, &mode);
#else
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
    return 1;
}

// 等待 fd 可读/挂断/出错，timeout_ms 内返回 true。out_readable=true 表示需要 recv
bool wait_readable(SocketHandle fd, int timeout_ms, bool& out_readable)
{
    out_readable = false;
#ifdef _WIN32
    ::fd_set rset;
    FD_ZERO(&rset);
    FD_SET(fd, &rset);
    ::fd_set eset;
    FD_ZERO(&eset);
    FD_SET(fd, &eset);
    ::timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int sr = ::select(0, &rset, nullptr, &eset, &tv);
    if (sr <= 0) {
        return false;
    }
    out_readable = FD_ISSET(fd, &rset) != 0 || FD_ISSET(fd, &eset) != 0;
    return true;
#else
    ::pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return false;
    }
    out_readable = (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    return true;
#endif
}

// 阻塞写完整缓冲；失败返回 false
bool send_all(SocketHandle fd, const char* data, std::size_t size)
{
    std::size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        const int n = ::send(fd, data + sent, static_cast<int>(size - sent), 0);
#else
        const ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void set_nodelay(SocketHandle fd)
{
    int one = 1;
#ifdef _WIN32
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

// 进程级 WSA 初始化（RAII，多次使用安全）
struct NetInitializer
{
    NetInitializer() { net_startup(); }
    ~NetInitializer() { net_cleanup(); }
};

NetInitializer& net_initializer()
{
    static NetInitializer instance;
    return instance;
}

// 从缓冲拆出全部完整帧；协议违规返回 false（*frames 已拆出的仍有效）
bool extract_frames(const TcpConfig& config, std::string& buffer,
                    std::vector<std::pair<std::uint8_t, std::string>>& frames)
{
    while (buffer.size() >= 5) {
        const std::uint32_t frame_len = get_u32_le(buffer.data());  // 载荷+1
        if (frame_len < 1 || frame_len > static_cast<std::uint32_t>(config.max_frame_bytes)) {
            return false;
        }
        if (buffer.size() < frame_len + 4u) {
            break;  // 半包
        }
        const std::uint8_t type = static_cast<std::uint8_t>(buffer[4]);
        frames.emplace_back(type, buffer.substr(5, frame_len - 1));
        buffer.erase(0, frame_len + 4);
    }
    return true;
}

}  // namespace

// ============================ 客户端 ============================

struct TcpClient::Impl
{
    TcpConfig config;

    mutable std::mutex mutex;
    SocketHandle fd = kInvalidSocket;
    std::string host;
    std::uint16_t port = 0;

    std::function<void(const std::string&)> on_message;
    std::function<void()> on_connect;
    std::function<void(const std::string&)> on_disconnect;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> connected{false};
    std::thread worker;  // 收包线程（connect 启动、close/析构 join）

    // wheel_liveness 模式：入站戳 + 轮上的超时任务句柄
    std::shared_ptr<LivenessStamp> stamp;
    TimerWheel::Handle hb_handle;

    // auto_reconnect 状态（TimerWheel 调度指数退避重连）
    std::atomic<bool> user_closed{false};   // close() 置位：不重连
    std::atomic<int>  reconnect_attempt{0};
    std::string reconnect_host;
    std::uint16_t reconnect_port = 0;
    TimerWheel::Handle rc_handle;           // 待触发的重连任务

    int heartbeat_timeout() const
    {
        if (config.heartbeat_interval_ms <= 0) {
            return 0;
        }
        return config.heartbeat_timeout_ms > 0 ? config.heartbeat_timeout_ms
                                               : config.heartbeat_interval_ms * 3;
    }
};

TcpClient::TcpClient() : impl_(new Impl)
{
    net_initializer();
}

TcpClient::TcpClient(const TcpConfig& config) : TcpClient()
{
    impl_->config = config;
}

TcpClient::~TcpClient()
{
    close();
    // 收包线程的收尾（含断连回调）在 session_loop 里完成；join 即等它退出
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    delete impl_;
}

// 客户端收包线程主体：管理连接直至断开（不自动重连——外部判断 is_connected 决定）
void TcpClient::session_loop(std::uint64_t /*conn_id*/, std::intptr_t fd_handle)
{
    const SocketHandle fd = static_cast<SocketHandle>(fd_handle);
    const bool use_wheel = impl_->config.wheel_liveness;
    std::string read_buf;
    read_buf.reserve(8 * 1024);
    char chunk[16 * 1024];
    std::uint64_t last_ping_ms = steady_now_ms();
    std::uint64_t last_inbound = steady_now_ms();
    const int hb_timeout = impl_->heartbeat_timeout();

    // wheel 模式：注册周期复查任务（每 hb_timeout/2 tick 复查一次入站戳；
    // 超时则 shutdown 唤醒会话线程走正常收尾，任务由句柄取消而自停）。
    // 复查频率减半 + 仅在超时才动作：空闲连接从 10Hz 轮询降到准零唤醒。
    TimerWheel::Handle local_hb;
    if (use_wheel && hb_timeout > 0) {
        impl_->stamp = std::make_shared<LivenessStamp>();
        impl_->stamp->touch();
        auto* impl = impl_;
        const std::int64_t check_every =
            std::max<std::int64_t>(hb_timeout / 2, 50);
        local_hb = shared_timer_wheel().add_periodic_ms(
            check_every, [impl, fd, hb_timeout]() -> bool {
                if (impl->stop_requested.load()) {
                    return false;   // 会话已停：任务自停
                }
                const std::uint64_t stamp_ms =
                    impl->stamp ? impl->stamp->get() : 0;
                if (steady_now_ms() - stamp_ms >
                    static_cast<std::uint64_t>(hb_timeout)) {
                    // 判死：shutdown 让阻塞的 recv/wait_readable 立即返回，
                    // 会话线程走与轮询模式完全相同的收尾路径
                    shutdown_socket(fd);
                    return false;
                }
                return true;    // 仍存活：下一 tick 再查
            });
        impl_->hb_handle = local_hb;
    }

    for (;;) {
        if (impl_->stop_requested.load()) {
            break;
        }
        bool readable = false;
        if (wait_readable(fd, use_wheel ? 200 : 100, readable) && readable) {
            const int n = static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
            if (n <= 0) {
                break;  // 对端关闭或连接错误
            }
            last_inbound = steady_now_ms();
            if (use_wheel && impl_->stamp) {
                impl_->stamp->touch();
            }
            read_buf.append(chunk, static_cast<std::size_t>(n));
            std::vector<std::pair<std::uint8_t, std::string>> frames;
            bool protocol_ok;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                protocol_ok = extract_frames(impl_->config, read_buf, frames);
            }
            if (!protocol_ok) {
                break;  // 帧长度炸弹：断开
            }
            for (const auto& f : frames) {
                if (f.first == kFrameData) {
                    std::function<void(const std::string&)> msg;
                    {
                        std::lock_guard<std::mutex> lock(impl_->mutex);
                        msg = impl_->on_message;
                    }
                    if (msg) {
                        msg(f.second);  // 锁外：回调可安全调 send()
                    }
                }
                // PING/PONG 客户端只用于保活，无业务动作
            }
        }        // 心跳：wheel 模式下超时判死由轮任务负责（shutdown 唤醒），
        // 这里只负责周期发 PING；轮询模式维持原有完整逻辑
        if (hb_timeout > 0) {
            const std::uint64_t now = steady_now_ms();
            if (!use_wheel &&
                now - last_inbound > static_cast<std::uint64_t>(hb_timeout)) {
                break;
            }
            if (now - last_ping_ms >=
                static_cast<std::uint64_t>(impl_->config.heartbeat_interval_ms)) {
                const std::string ping = make_frame(kFramePing, "");
                if (send_all(fd, ping.data(), ping.size())) {
                    last_ping_ms = now;
                }
            }
        }
    }

    // 取消轮上的会话任务（wheel 模式）
    if (use_wheel) {
        impl_->hb_handle.cancel();
        impl_->hb_handle = TimerWheel::Handle();
        impl_->stamp.reset();
    }

    // 收尾：关 socket、置状态、触发断连回调（锁外）
    shutdown_socket(fd);
    close_socket(fd);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->fd == fd) {
            impl_->fd = kInvalidSocket;
        }
        impl_->connected.store(false);
    }
    const bool abnormal = !impl_->stop_requested.load();
    if (abnormal) {
        std::function<void(const std::string&)> disc;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            disc = impl_->on_disconnect;
        }
        if (disc) {
            disc("connection lost");
        }
    }

    // auto_reconnect：异常断开时按指数退避调度重连（TimerWheel 触发，
    // 不占用任何线程等待）。close()（user_closed）或 stop 时不重连
    if (abnormal && impl_->config.auto_reconnect &&
        !impl_->user_closed.load()) {
        const int attempt = impl_->reconnect_attempt.fetch_add(1) + 1;
        std::int64_t delay = static_cast<std::int64_t>(
            impl_->config.reconnect_base_delay_ms *
            (1 << std::min(attempt - 1, 16)));
        delay = std::min<std::int64_t>(delay,
                                       impl_->config.reconnect_max_delay_ms);
        auto* impl = impl_;
        const std::string host = impl_->reconnect_host;
        const std::uint16_t port = impl_->reconnect_port;
        impl_->rc_handle = shared_timer_wheel().add_ms(delay, [this, impl, host, port] {
            if (impl->stop_requested.load() || impl->user_closed.load()) {
                return;
            }
            impl->reconnect_attempt.store(0);
            connect_impl(host, port);   // 成功即恢复；再断会再次调度
        });
    }
}

bool TcpClient::connect(const std::string& host, std::uint16_t port)
{
    return connect_impl(host, port);
}

std::future<bool> TcpClient::async_connect(const std::string& host,
                                           std::uint16_t port)
{
    // shared_ptr 持 promise：连接尝试在内部线程进行，调用方可能
    // 不保存 future（promise 必须与尝试同生命周期）；future 侧
    // 析构不影响结果置值，不会 broken_promise
    std::shared_ptr<std::promise<bool>> p(new std::promise<bool>());
    std::future<bool> f = p->get_future();
    std::thread([this, p, host, port]() {
        p->set_value(connect_impl(host, port));
    }).detach();
    return f;
}

// connect() 的共用实现：同步路径直接调用；async_connect 在独立
// 线程上调用（解析 + 握手最多阻塞 connect_timeout_ms）
bool TcpClient::connect_impl(const std::string& host, std::uint16_t port)
{
    close();  // 重复 connect：停旧线程（若在跑，close 内部 join）

    const std::uint32_t net_addr = resolve_ipv4(host);
    if (net_addr == 0) {
        return false;
    }
    SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        return false;
    }
    if (connect_with_timeout(fd, net_addr, port, impl_->config.connect_timeout_ms) != 1) {
        close_socket(fd);
        return false;
    }
    set_nodelay(fd);

    std::function<void()> conn;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->fd = fd;
        impl_->host = host;
        impl_->port = port;
        impl_->stop_requested.store(false);
        impl_->connected.store(true);
        conn = impl_->on_connect;
    }
    impl_->worker = std::thread([this, fd]() { session_loop(0, static_cast<std::intptr_t>(fd)); });

    if (conn) {
        conn();
    }
    return true;
}

void TcpClient::close()
{
    impl_->stop_requested.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        shutdown_socket(impl_->fd);
        close_socket(impl_->fd);
        impl_->fd = kInvalidSocket;
        impl_->connected.store(false);
    }
    // join 收包线程：session_loop 退出前会执行断连回调（若非主动关闭）。
    // 同步 join 保证 close() 返回后无残留线程，也避免下次 connect 时
    // 旧线程仍占用 worker 对象导致 move-from-joinable 崩溃
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

bool TcpClient::send(const std::string& payload)
{
    const std::string frame = make_frame(kFrameData, payload);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fd == kInvalidSocket) {
        return false;
    }
    return send_all(impl_->fd, frame.data(), frame.size());
}

bool TcpClient::is_connected() const
{
    return impl_->connected.load();
}

const TcpConfig& TcpClient::config() const
{
    return impl_->config;
}

std::string TcpClient::peer() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fd == kInvalidSocket) {
        return "";
    }
    return impl_->host + ":" + std::to_string(impl_->port);
}

void TcpClient::set_on_message(std::function<void(const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_message = std::move(handler);
}

void TcpClient::set_on_connect(std::function<void()> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_connect = std::move(handler);
}

void TcpClient::set_on_disconnect(std::function<void(const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_disconnect = std::move(handler);
}

// ============================ 服务端 ============================

struct TcpServer::Impl
{
    struct Session
    {
        SocketHandle fd = kInvalidSocket;
        std::string peer;
    };

    TcpConfig config;
    std::uint16_t listen_port = 0;

    mutable std::mutex mutex;
    SocketHandle listener = kInvalidSocket;
    std::thread accept_thread;
    std::map<std::uint64_t, Session> sessions;           // conn_id → 会话
    std::map<std::uint64_t, std::thread> session_threads;  // conn_id → 线程
    std::uint64_t next_conn_id = 1;

    std::function<void(std::uint64_t, const std::string&)> on_message;
    std::function<void(std::uint64_t)> on_connect;
    std::function<void(std::uint64_t, const std::string&)> on_disconnect;

    std::atomic<bool> running{false};

    // wheel_liveness 模式：会话入站戳（会话线程 touch）与轮上的活性
    // 周期任务句柄。轮任务到期集中复查：有新数据则继续，超时则
    // shutdown(fd) 唤醒会话线程收尾——空闲会话零唤醒
    std::map<std::uint64_t, std::shared_ptr<LivenessStamp>> liveness;
    std::map<std::uint64_t, TimerWheel::Handle> liveness_timers;

    // 注册会话活性周期任务。须持 mutex 调用：轮回调在轮锁外执行，
    // 本工程只存在「服务器锁 → 轮锁」单向嵌套，无反向路径，无死锁
    void register_liveness_locked(std::uint64_t conn_id,
                                  const std::shared_ptr<LivenessStamp>& stamp)
    {
        const int hb_timeout = heartbeat_timeout();
        // 复查频率为超时的一半：迟到容忍 + 轮 tick 粒度余量；
        // 只在真正超时才 shutdown(fd) 动手——空闲会话零唤醒
        const std::int64_t check_every = std::max<std::int64_t>(hb_timeout / 2, 50);
        auto* impl = this;
        liveness_timers[conn_id] = shared_timer_wheel().add_periodic_ms(
            check_every, [impl, conn_id, stamp, hb_timeout]() -> bool {
                SocketHandle fd = kInvalidSocket;
                bool dead = false;
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    if (!impl->running.load() ||
                        impl->liveness.count(conn_id) == 0) {
                        return false;   // 会话已收尾/服务已停：任务自停
                    }
                    auto sit = impl->sessions.find(conn_id);
                    if (sit == impl->sessions.end()) {
                        return false;
                    }
                    fd = sit->second.fd;
                    dead = steady_now_ms() - stamp->get() >
                           static_cast<std::uint64_t>(hb_timeout);
                }
                if (dead) {
                    shutdown_socket(fd);   // 锁外：唤醒阻塞在 recv 的会话线程收尾
                    return false;          // 判死：任务自停
                }
                return true;    // 仍存活：下一 tick 再查
            });
    }

    // 会话收尾：抹掉活性记录并取消轮任务（须持 mutex 调用）
    void drop_liveness_locked(std::uint64_t conn_id)
    {
        liveness.erase(conn_id);
        auto it = liveness_timers.find(conn_id);
        if (it != liveness_timers.end()) {
            it->second.cancel();
            liveness_timers.erase(it);
        }
    }

    int heartbeat_timeout() const
    {
        if (config.heartbeat_interval_ms <= 0) {
            return 0;
        }
        return config.heartbeat_timeout_ms > 0 ? config.heartbeat_timeout_ms
                                               : config.heartbeat_interval_ms * 3;
    }

    // 从活跃表摘除一条会话（不 join 线程——线程收尾后自摘时线程对象仍归表管，
    // 统一由 stop()/析构收割；持有 mutex 调用）
    void drop_session_locked(std::uint64_t conn_id, bool close_fd)
    {
        auto it = sessions.find(conn_id);
        if (it == sessions.end()) {
            return;
        }
        if (close_fd && it->second.fd != kInvalidSocket) {
            shutdown_socket(it->second.fd);
            close_socket(it->second.fd);
            it->second.fd = kInvalidSocket;
        }
        sessions.erase(it);
        drop_liveness_locked(conn_id);
    }
};

TcpServer::TcpServer() : impl_(new Impl)
{
    net_initializer();
}

TcpServer::TcpServer(const TcpConfig& config) : TcpServer()
{
    impl_->config = config;
}

TcpServer::~TcpServer()
{
    stop();
    delete impl_;
}

bool TcpServer::start(const std::string& host, std::uint16_t port)
{
    stop();

    SocketHandle l = ::socket(AF_INET, SOCK_STREAM, 0);
    if (l == kInvalidSocket) {
        return false;
    }
    int one = 1;
    ::setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    std::uint32_t net_addr;
    if (host.empty() || host == "0.0.0.0") {
        net_addr = INADDR_ANY;
    } else {
        net_addr = resolve_ipv4(host);
        if (net_addr == 0) {
            close_socket(l);
            return false;
        }
    }
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = net_addr;
    sa.sin_port = htons(port);
    if (::bind(l, reinterpret_cast<::sockaddr*>(&sa), sizeof(sa)) != 0) {
        close_socket(l);
        return false;
    }
    if (::listen(l, 16) != 0) {
        close_socket(l);
        return false;
    }
    ::sockaddr_in bound{};
#ifdef _WIN32
    int blen = static_cast<int>(sizeof(bound));
#else
    socklen_t blen = sizeof(bound);
#endif
    std::uint16_t actual = port;
    if (port == 0 && ::getsockname(l, reinterpret_cast<::sockaddr*>(&bound), &blen) == 0) {
        actual = ntohs(bound.sin_port);
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->listener = l;
        impl_->listen_port = actual;
        impl_->next_conn_id = 1;
    }
    impl_->running.store(true);

    impl_->accept_thread = std::thread([this]() {
        while (impl_->running.load()) {
            SocketHandle l;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                l = impl_->listener;
            }
            if (l == kInvalidSocket) {
                return;
            }
            bool readable = false;
            if (!wait_readable(l, 100, readable) || !readable) {
                continue;
            }
            ::sockaddr_in peer_addr{};
#ifdef _WIN32
            int addr_len = static_cast<int>(sizeof(peer_addr));
#else
            socklen_t addr_len = sizeof(peer_addr);
#endif
            SocketHandle client =
                ::accept(l, reinterpret_cast<::sockaddr*>(&peer_addr), &addr_len);
            if (client == kInvalidSocket) {
                if (!impl_->running.load()) {
                    return;  // stop() 关闭监听 socket 的假错误
                }
                continue;
            }
            set_nodelay(client);

            char ip[64] = {0};
            ::inet_ntop(AF_INET, &peer_addr.sin_addr, ip, sizeof(ip));
            Impl::Session session;
            session.fd = client;
            session.peer = std::string(ip) + ":" + std::to_string(ntohs(peer_addr.sin_port));

            std::uint64_t conn_id;
            std::function<void(std::uint64_t)> conn;
            std::shared_ptr<LivenessStamp> stamp;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                conn_id = impl_->next_conn_id++;
                impl_->sessions[conn_id] = session;
                conn = impl_->on_connect;
                if (impl_->config.wheel_liveness &&
                    impl_->heartbeat_timeout() > 0) {
                    stamp = std::make_shared<LivenessStamp>();
                    stamp->touch();
                    impl_->liveness[conn_id] = stamp;
                    impl_->register_liveness_locked(conn_id, stamp);
                }
            }
            // 会话线程登记与启动（锁外：thread 构造不持外层锁，避免与
            // session_loop 收尾路径互相等锁）
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->session_threads[conn_id] = std::thread(
                    [this, conn_id, client]() {
                        session_loop(conn_id, static_cast<std::intptr_t>(client));
                    });
            }
            if (conn) {
                conn(conn_id);
            }
        }
    });
    return true;
}

// 服务端会话线程：拆帧 → 分发；PING → PONG；心跳超时断开
void TcpServer::session_loop(std::uint64_t conn_id, std::intptr_t fd_handle)
{
    const SocketHandle fd = static_cast<SocketHandle>(fd_handle);
    std::string read_buf;
    read_buf.reserve(8 * 1024);
    char chunk[16 * 1024];
    std::uint64_t last_inbound = steady_now_ms();
    const int hb_timeout = impl_->heartbeat_timeout();
    // wheel 模式：活性由轮任务复查（超时 shutdown(fd) 唤醒本线程），
    // 会话线程只在有数据时被唤醒；检查 running 的频率也随之降低
    const bool use_wheel_mode = impl_->config.wheel_liveness && hb_timeout > 0;
    bool use_wheel = use_wheel_mode;
    std::shared_ptr<LivenessStamp> stamp;
    if (use_wheel) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->liveness.find(conn_id);
        if (it != impl_->liveness.end()) {
            stamp = it->second;
        } else {
            use_wheel = false;  // 无活性记录（如构造后改过配置）：退回轮询
        }
    }

    for (;;) {
        if (!impl_->running.load()) {
            break;
        }
        bool readable = false;
        if (wait_readable(fd, use_wheel ? 2000 : 100, readable) && readable) {
            const int n = static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
            if (n <= 0) {
                break;
            }
            last_inbound = steady_now_ms();
            if (use_wheel && stamp) {
                stamp->touch();
            }
            read_buf.append(chunk, static_cast<std::size_t>(n));
            std::vector<std::pair<std::uint8_t, std::string>> frames;
            bool protocol_ok;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                protocol_ok = extract_frames(impl_->config, read_buf, frames);
            }
            if (!protocol_ok) {
                break;
            }
            for (const auto& f : frames) {
                if (f.first == kFrameData) {
                    std::function<void(std::uint64_t, const std::string&)> msg;
                    {
                        std::lock_guard<std::mutex> lock(impl_->mutex);
                        msg = impl_->on_message;
                    }
                    if (msg) {
                        msg(conn_id, f.second);  // 锁外：回调可安全调 send()/disconnect()
                    }
                } else if (f.first == kFramePing) {
                    const std::string pong = make_frame(kFramePong, "");
                    send_all(fd, pong.data(), pong.size());
                }
            }
        }

        // 轮询模式判死；wheel 模式判死由轮任务 shutdown(fd) 触发（recv 返回
        // ≤0 走上面的 break），这里不再需要时间检查
        if (!use_wheel && hb_timeout > 0 &&
            steady_now_ms() - last_inbound > static_cast<std::uint64_t>(hb_timeout)) {
            break;  // 心跳超时判死
        }
    }

    // 收尾：摘除会话 + 断连回调（锁外执行）
    const bool was_known = [&]() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const bool known = impl_->sessions.count(conn_id) != 0;
        impl_->drop_session_locked(conn_id, /*close_fd=*/false);  // fd 由本线程关闭
        return known;
    }();
    shutdown_socket(fd);
    close_socket(fd);
    std::function<void(std::uint64_t, const std::string&)> disc;
    if (was_known) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        disc = impl_->on_disconnect;
    }
    if (disc) {
        disc(conn_id, "closed");
    }
}

void TcpServer::stop()
{
    impl_->running.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        shutdown_socket(impl_->listener);
        close_socket(impl_->listener);
        impl_->listener = kInvalidSocket;
        for (auto& kv : impl_->sessions) {
            shutdown_socket(kv.second.fd);
            close_socket(kv.second.fd);
            kv.second.fd = kInvalidSocket;
        }
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    // 收割全部会话线程（socket 已关，各线程 100ms 内退出）
    std::map<std::uint64_t, std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        threads = std::move(impl_->session_threads);
        impl_->session_threads.clear();
        impl_->sessions.clear();
    }
    for (auto& kv : threads) {
        if (kv.second.joinable()) {
            kv.second.join();
        }
    }
}

void TcpServer::send(std::uint64_t conn_id, const std::string& payload)
{
    const std::string frame = make_frame(kFrameData, payload);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->sessions.find(conn_id);
    if (it != impl_->sessions.end() && it->second.fd != kInvalidSocket) {
        send_all(it->second.fd, frame.data(), frame.size());
    }
}

void TcpServer::broadcast(const std::string& payload)
{
    const std::string frame = make_frame(kFrameData, payload);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& kv : impl_->sessions) {
        if (kv.second.fd != kInvalidSocket) {
            send_all(kv.second.fd, frame.data(), frame.size());
        }
    }
}

void TcpServer::disconnect(std::uint64_t conn_id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->sessions.find(conn_id);
    if (it != impl_->sessions.end()) {
        // 只 shutdown 不 close：关闭后的 fd 在 select 中报错而非可读，
        // 会话线程感知不到；shutdown 使其变可读、recv 返回 0 → 自收尾
        shutdown_socket(it->second.fd);
        it->second.fd = kInvalidSocket;
    }
}

std::uint16_t TcpServer::port() const
{
    return impl_->listen_port;
}

bool TcpServer::is_running() const
{
    return impl_->running.load();
}

std::size_t TcpServer::connection_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sessions.size();
}

void TcpServer::set_on_message(
    std::function<void(std::uint64_t, const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_message = std::move(handler);
}

void TcpServer::set_on_connect(std::function<void(std::uint64_t)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_connect = std::move(handler);
}

void TcpServer::set_on_disconnect(
    std::function<void(std::uint64_t, const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_disconnect = std::move(handler);
}

}  // namespace libmini
