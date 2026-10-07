#include "websocket.h"

#include "digest.h"
#include "encoding.h"
#include "log_facade.h"
#include "net_addr.h"
#include "secure_random.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <spdlog/logger.h>

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

namespace libmini {

namespace {

// ---------------- 平台层（与 tcp.cpp 同构） ----------------

std::uint64_t ws_now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

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
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

// 带超时的非阻塞 connect（1=成功 0=超时 -1=错误）
int connect_with_timeout(SocketHandle fd, std::uint32_t net_addr,
                         std::uint16_t port, int timeout_ms)
{
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = net_addr;
    sa.sin_port = htons(port);

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
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&so_error), &len) != 0 ||
        so_error != 0) {
        return -1;
    }
#ifdef _WIN32
    mode = 0;
    ::ioctlsocket(fd, FIONBIO, &mode);
#else
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
    return 1;
}

// ---------------- RFC 6455 帧协议 ----------------

constexpr std::uint8_t kOpCont = 0x0;
constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpBinary = 0x2;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

// Sec-WebSocket-Accept = Base64(SHA-1(key + GUID))
constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string compute_accept(const std::string& key)
{
    Sha1 sha;
    sha.update(key);
    sha.update(std::string(kWsGuid));
    return Base64::encode(sha.finish());
}

std::string apply_mask(const std::string& payload, const std::string& key)
{
    std::string out = payload;
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(out[i] ^ key[i % 4]);
    }
    return out;
}

// 构造一帧：FIN=1。mask=true 时附加 4 字节随机掩码键（客户端→服务端必须）
std::string make_frame(std::uint8_t opcode, const std::string& payload,
                       bool mask)
{
    std::string out;
    out.reserve(payload.size() + 14);
    out.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode

    const std::size_t len = payload.size();
    const std::uint8_t mask_bit = mask ? 0x80 : 0;
    if (len <= 125) {
        out.push_back(static_cast<char>(mask_bit | len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>((len >> 8) & 0xff));
        out.push_back(static_cast<char>(len & 0xff));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>(
                (static_cast<std::uint64_t>(len) >> shift) & 0xff));
        }
    }
    if (mask) {
        const std::string key = secure_random_string(4);
        out.append(key);
        out.append(apply_mask(payload, key));
    } else {
        out.append(payload);
    }
    return out;
}

struct WsFrame
{
    std::uint8_t opcode = 0;
    bool fin = true;
    std::string payload;
};

// 从缓冲拆出全部完整帧；协议违规返回 false。expect_masked 表示「对端
// 发来的帧必须带掩码」（服务端收客户端帧为 true，客户端收服务端帧为
// false）——RFC 6455 §5.1 双向强制
bool extract_frames(std::string& buffer, bool expect_masked,
                    std::size_t max_frame, std::vector<WsFrame>& frames)
{
    for (;;) {
        if (buffer.size() < 2) {
            return true;
        }
        const auto* ub = reinterpret_cast<const unsigned char*>(buffer.data());
        const std::uint8_t b0 = ub[0];
        const std::uint8_t b1 = ub[1];

        if ((b0 & 0x70) != 0) {
            return false;  // RSV1-3：本实现不协商扩展
        }
        const std::uint8_t opcode = b0 & 0x0f;
        const bool fin = (b0 & 0x80) != 0;
        const bool masked = (b1 & 0x80) != 0;
        if (masked != expect_masked) {
            return false;  // 掩码方向违规
        }
        if (opcode != kOpCont && opcode != kOpText && opcode != kOpBinary &&
            opcode != kOpClose && opcode != kOpPing && opcode != kOpPong) {
            return false;  // 保留 opcode
        }
        const bool control = (opcode & 0x08) != 0;
        std::uint64_t len = b1 & 0x7f;
        std::size_t pos = 2;
        if (len == 126) {
            if (buffer.size() < pos + 2) {
                return true;  // 半包
            }
            len = (static_cast<std::uint64_t>(ub[2]) << 8) | ub[3];
            pos += 2;
        } else if (len == 127) {
            if (buffer.size() < pos + 8) {
                return true;
            }
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | ub[pos + i];
            }
            pos += 8;
            if (len > 0x7FFFFFFFFFFFFFFFULL) {
                return false;  // 64 位长度最高位必须为 0
            }
        }
        if (control) {
            if (!fin || len > 125) {
                return false;  // 控制帧：不可分片、载荷 <= 125 字节
            }
        }
        if (len > static_cast<std::uint64_t>(max_frame)) {
            return false;  // 长度炸弹
        }
        const std::size_t payload_len = static_cast<std::size_t>(len);
        if (masked) {
            if (buffer.size() < pos + 4 + payload_len) {
                return true;
            }
            WsFrame f;
            f.opcode = opcode;
            f.fin = fin;
            f.payload = apply_mask(
                std::string(buffer.data() + pos + 4, payload_len),
                std::string(buffer.data() + pos, 4));
            frames.push_back(std::move(f));
            buffer.erase(0, pos + 4 + payload_len);
        } else {
            if (buffer.size() < pos + payload_len) {
                return true;
            }
            WsFrame f;
            f.opcode = opcode;
            f.fin = fin;
            f.payload = buffer.substr(pos, payload_len);
            frames.push_back(std::move(f));
            buffer.erase(0, pos + payload_len);
        }
    }
}

// ---------------- HTTP 头部解析 ----------------

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(::tolower(c));
    });
    return s;
}

std::string trim(const std::string& s)
{
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

struct HttpHead
{
    std::string line;  // 请求行 / 状态行（原样）
    std::map<std::string, std::string> headers;  // 键小写
};

// 解析到 \r\n\r\n 为止的头部；不足一对空行返回 false
bool parse_http_head(const std::string& raw, HttpHead& out)
{
    const std::size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos) {
        return false;
    }
    std::size_t pos = raw.find("\r\n");
    if (pos == std::string::npos || pos > end) {
        return false;
    }
    out.line = raw.substr(0, pos);
    pos += 2;
    while (pos < end) {
        std::size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > end) {
            eol = end;
        }
        const std::string line = raw.substr(pos, eol - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            out.headers[to_lower(trim(line.substr(0, colon)))] =
                trim(line.substr(colon + 1));
        }
        pos = eol + 2;
    }
    return true;
}

// 读到 \r\n\r\n（超时或头部超 16KB 返回 false）
bool recv_http_head(SocketHandle fd, int timeout_ms, std::string& raw)
{
    char chunk[2048];
    const std::uint64_t deadline =
        ws_now_ms() + static_cast<std::uint64_t>(timeout_ms > 0 ? timeout_ms
                                                                : 5000);
    while (raw.find("\r\n\r\n") == std::string::npos) {
        if (raw.size() > 16 * 1024) {
            return false;
        }
        const std::uint64_t now = ws_now_ms();
        if (now >= deadline) {
            return false;
        }
        bool readable = false;
        if (!wait_readable(fd, static_cast<int>(deadline - now), readable)) {
            return false;
        }
        if (!readable) {
            continue;
        }
        const int n = static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
        if (n <= 0) {
            return false;
        }
        raw.append(chunk, static_cast<std::size_t>(n));
    }
    return true;
}

// ---------------- 握手 ----------------

// 客户端：发 Upgrade 请求并校验 101；success 时 leftover 收下后续残余字节
bool client_handshake(SocketHandle fd, const std::string& host,
                      std::uint16_t port, const std::string& path,
                      int timeout_ms, std::string& leftover, std::string& err)
{
    const std::string key = Base64::encode(secure_random_string(16));
    if (key.empty()) {
        err = "secure random unavailable";
        return false;
    }
    const std::string req =
        "GET " + (path.empty() ? std::string("/") : path) +
        " HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    if (!send_all(fd, req.data(), req.size())) {
        err = "handshake request send failed";
        return false;
    }

    std::string raw;
    if (!recv_http_head(fd, timeout_ms, raw)) {
        err = "handshake response timeout or too large";
        return false;
    }
    HttpHead head;
    if (!parse_http_head(raw, head)) {
        err = "malformed handshake response";
        return false;
    }
    // 状态行："HTTP/1.1 101 Switching Protocols"
    if (head.line.size() < 12 || head.line.compare(0, 5, "HTTP/") != 0 ||
        head.line.compare(9, 3, "101") != 0) {
        err = "unexpected status: " + head.line;
        return false;
    }
    auto it = head.headers.find("sec-websocket-accept");
    if (it == head.headers.end() || it->second != compute_accept(key)) {
        err = "Sec-WebSocket-Accept mismatch";
        return false;
    }
    leftover = raw.substr(raw.find("\r\n\r\n") + 4);
    return true;
}

// 服务端：读请求、校验后回写 101；success 时 leftover 收下残余字节
bool server_handshake(SocketHandle fd, int timeout_ms, std::string& leftover,
                      std::string& err)
{
    std::string raw;
    if (!recv_http_head(fd, timeout_ms, raw)) {
        err = "handshake request timeout or too large";
        return false;
    }
    HttpHead head;
    if (!parse_http_head(raw, head)) {
        err = "malformed handshake request";
        return false;
    }
    if (head.line.size() <= 4 || head.line.compare(0, 3, "GET") != 0 ||
        head.line[3] != ' ') {
        err = "method must be GET";
        return false;
    }
    auto up = head.headers.find("upgrade");
    if (up == head.headers.end() ||
        to_lower(up->second).find("websocket") == std::string::npos) {
        err = "missing Upgrade: websocket";
        return false;
    }
    auto key_it = head.headers.find("sec-websocket-key");
    if (key_it == head.headers.end() || key_it->second.empty()) {
        err = "missing Sec-WebSocket-Key";
        return false;
    }
    auto ver = head.headers.find("sec-websocket-version");
    if (ver != head.headers.end() && ver->second != "13") {
        err = "unsupported Sec-WebSocket-Version";
        return false;
    }

    const std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + compute_accept(key_it->second) + "\r\n"
        "\r\n";
    if (!send_all(fd, resp.data(), resp.size())) {
        err = "handshake response send failed";
        return false;
    }
    leftover = raw.substr(raw.find("\r\n\r\n") + 4);
    return true;
}

std::string close_payload(const std::string& reason)
{
    std::string p;
    p.push_back(static_cast<char>(0x03));  // 1000 normal closure
    p.push_back(static_cast<char>(0xE8));
    p.append(reason, 0, std::min<std::size_t>(reason.size(), 123));
    return p;
}

}  // namespace

// ---------------- 配置校验 ----------------

bool WsConfig::validate() const
{
    bool ok = true;
    spdlog::logger* log = LogFacade::logger();

    const auto warn = [&ok, log](bool bad, const char* what) {
        if (bad) {
            if (log) {
                log->warn("websocket config invalid: {}", what);
            }
            ok = false;
        }
    };

    warn(connect_timeout_ms < 0, "connect_timeout_ms < 0");
    warn(handshake_timeout_ms < 0, "handshake_timeout_ms < 0");
    warn(max_message_bytes <= 0, "max_message_bytes must be positive");
    return ok;
}

// ============================ 客户端 ============================

struct WsClient::Impl
{
    WsConfig config;

    mutable std::mutex mutex;
    SocketHandle fd = kInvalidSocket;
    std::string host;
    std::uint16_t port = 0;

    std::function<void(const std::string&)> on_message;
    std::function<void()> on_connect;
    std::function<void(const std::string&)> on_disconnect;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> connected{false};
    std::thread worker;
    bool self_detached = false;  // 极端路径：自身线程上析构（泄漏 impl_）

    // 发送一帧（持锁串行化；客户端帧必须掩码）
    bool send_frame(std::uint8_t opcode, const std::string& payload)
    {
        const std::string frame = make_frame(opcode, payload, /*mask=*/true);
        std::lock_guard<std::mutex> lock(mutex);
        if (fd == kInvalidSocket) {
            return false;
        }
        return send_all(fd, frame.data(), frame.size());
    }

    void session_loop(std::intptr_t fd_handle, std::string initial);
};

// 客户端收包线程：拆帧分发、PING→PONG、CLOSE→回关收尾
void WsClient::Impl::session_loop(std::intptr_t fd_handle, std::string initial)
{
    const SocketHandle fd = static_cast<SocketHandle>(fd_handle);
    std::string read_buf = std::move(initial);
    char chunk[16 * 1024];
    bool close_sent = false;
    bool got_close = false;
    bool assembling = false;
    std::string asm_buf;
    std::string reason = "connection lost";

    for (;;) {
        if (stop_requested.load()) {
            reason = "closed";
            break;
        }
        bool readable = false;
        if (!wait_readable(fd, 100, readable)) {
            continue;  // 周期醒来检查 stop_requested
        }
        if (!readable) {
            continue;
        }
        const int n = static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
        if (n <= 0) {
            reason = (close_sent || got_close || stop_requested.load())
                         ? "closed"
                         : "connection lost";
            break;
        }
        read_buf.append(chunk, static_cast<std::size_t>(n));
        std::vector<WsFrame> frames;
        if (!extract_frames(read_buf, /*expect_masked=*/false,
                            static_cast<std::size_t>(config.max_message_bytes),
                            frames)) {
            reason = "protocol error";
            break;
        }
        bool quit = false;
        for (const auto& f : frames) {
            if (f.opcode == kOpClose) {
                if (!close_sent) {
                    send_frame(kOpClose, f.payload);  // 应答后正常关闭
                    close_sent = true;
                }
                got_close = true;
                reason = "closed";
                quit = true;
                break;
            }
            if (f.opcode == kOpPing) {
                if (!send_frame(kOpPong, f.payload)) {
                    reason = "connection lost";
                    quit = true;
                    break;
                }
                continue;
            }
            if (f.opcode == kOpPong) {
                continue;  // 心跳应答，无业务动作
            }
            if (f.opcode == kOpText || f.opcode == kOpBinary) {
                if (assembling) {
                    reason = "protocol error";  // 重组中收到新数据帧
                    quit = true;
                    break;
                }
                if (f.fin) {
                    std::function<void(const std::string&)> msg;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        msg = on_message;
                    }
                    if (msg) {
                        msg(f.payload);  // 锁外：回调可安全调 send()/close()
                    }
                } else {
                    assembling = true;
                    asm_buf = f.payload;
                }
            } else if (f.opcode == kOpCont) {
                if (!assembling) {
                    reason = "protocol error";  // 无起始分片
                    quit = true;
                    break;
                }
                asm_buf.append(f.payload);
                if (asm_buf.size() >
                    static_cast<std::size_t>(config.max_message_bytes)) {
                    reason = "protocol error";  // 重组超限
                    quit = true;
                    break;
                }
                if (f.fin) {
                    assembling = false;
                    std::function<void(const std::string&)> msg;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        msg = on_message;
                    }
                    if (msg) {
                        msg(asm_buf);
                    }
                    asm_buf.clear();
                }
            }
        }
        if (quit) {
            break;
        }
    }

    // 收尾：本线程仅在仍持有 fd 时负责关闭（close() 可能已关闭）
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (fd == this->fd) {
            this->fd = kInvalidSocket;
            shutdown_socket(fd);
            close_socket(fd);
            connected.store(false);
        }
    }
    std::function<void(const std::string&)> disc;
    {
        std::lock_guard<std::mutex> lock(mutex);
        disc = on_disconnect;
    }
    if (disc) {
        disc(reason);  // 锁外回调（含主动 close：浏览器 close 语义）
    }
}

WsClient::WsClient() : impl_(new Impl)
{
    net_initializer();
}

WsClient::WsClient(const WsConfig& config) : WsClient()
{
    impl_->config = config;
}

WsClient::~WsClient()
{
    close();
    if (impl_->worker.joinable()) {
        if (impl_->worker.get_id() == std::this_thread::get_id()) {
            // 在自身回调线程里销毁客户端（极端用法）：不能 join 自己，
            // 也不能释放 impl_（会话线程仍在使用）——泄漏优于悬空
            impl_->worker.detach();
            impl_->self_detached = true;
            return;
        }
        impl_->worker.join();
    }
    delete impl_;
}

bool WsClient::connect(const std::string& host, std::uint16_t port,
                       const std::string& path)
{
    close();  // 重复 connect：先停旧连接

    if (!impl_->config.validate()) {
        return false;
    }
    const std::uint32_t net_addr = resolve_ipv4_net(host);
    if (net_addr == 0) {
        return false;
    }
    SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        return false;
    }
    if (connect_with_timeout(fd, net_addr, port,
                             impl_->config.connect_timeout_ms) != 1) {
        close_socket(fd);
        return false;
    }
    set_nodelay(fd);

    // HTTP Upgrade 握手（失败即整体失败，不进入会话线程）
    std::string leftover;
    std::string err;
    if (!client_handshake(fd, host, port, path,
                          impl_->config.handshake_timeout_ms, leftover, err)) {
        shutdown_socket(fd);
        close_socket(fd);
        spdlog::logger* log = LogFacade::logger();
        if (log) {
            log->warn("websocket handshake failed: {}", err);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->fd = fd;
        impl_->host = host;
        impl_->port = port;
        impl_->stop_requested.store(false);
        impl_->connected.store(true);
    }
    // 极端路径保护：自身回调线程里重连时旧 worker 不能被赋值覆盖
    if (impl_->worker.joinable()) {
        if (impl_->worker.get_id() == std::this_thread::get_id()) {
            impl_->worker.detach();
        } else {
            impl_->worker.join();
        }
    }
    impl_->worker = std::thread([this, fd, leftover]() {
        impl_->session_loop(static_cast<std::intptr_t>(fd), leftover);
    });

    std::function<void()> conn;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        conn = impl_->on_connect;
    }
    if (conn) {
        conn();
    }
    return true;
}

void WsClient::close()
{
    impl_->stop_requested.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->fd != kInvalidSocket) {
            // 尽力发 CLOSE 帧，让对端走正常关闭握手
            const std::string frame =
                make_frame(kOpClose, close_payload("normal"), /*mask=*/true);
            send_all(impl_->fd, frame.data(), frame.size());
            shutdown_socket(impl_->fd);
            close_socket(impl_->fd);
            impl_->fd = kInvalidSocket;
        }
        impl_->connected.store(false);
    }
    if (impl_->worker.joinable() &&
        impl_->worker.get_id() != std::this_thread::get_id()) {
        impl_->worker.join();  // 回调线程内调用 close() 时跳过自 join
    }
}

bool WsClient::send(const std::string& payload)
{
    if (payload.size() >
        static_cast<std::size_t>(impl_->config.max_message_bytes)) {
        return false;
    }
    return impl_->send_frame(kOpText, payload);
}

bool WsClient::send_binary(const std::string& payload)
{
    if (payload.size() >
        static_cast<std::size_t>(impl_->config.max_message_bytes)) {
        return false;
    }
    return impl_->send_frame(kOpBinary, payload);
}

bool WsClient::is_connected() const
{
    return impl_->connected.load();
}

void WsClient::set_on_message(std::function<void(const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_message = std::move(handler);
}

void WsClient::set_on_connect(std::function<void()> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_connect = std::move(handler);
}

void WsClient::set_on_disconnect(
    std::function<void(const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_disconnect = std::move(handler);
}

std::string WsClient::peer() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fd == kInvalidSocket) {
        return "";
    }
    return impl_->host + ":" + std::to_string(impl_->port);
}

// ============================ 服务端 ============================

struct WsServer::Impl
{
    struct Session
    {
        SocketHandle fd = kInvalidSocket;
        std::string peer;
        bool handshake_done = false;
    };

    WsConfig config;
    std::uint16_t listen_port = 0;

    mutable std::mutex mutex;
    SocketHandle listener = kInvalidSocket;
    std::thread accept_thread;
    std::map<std::uint64_t, Session> sessions;
    std::map<std::uint64_t, std::thread> session_threads;
    std::uint64_t next_conn_id = 1;

    std::function<void(std::uint64_t, const std::string&)> on_message;
    std::function<void(std::uint64_t)> on_connect;
    std::function<void(std::uint64_t, const std::string&)> on_disconnect;

    std::atomic<bool> running{false};
    bool self_detached = false;  // 极端路径：会话回调里 stop()（泄漏 impl_）

    // 发送一帧（持锁；握手未完成的半连接静默丢弃；服务端帧不掩码）
    bool send_frame(std::uint64_t conn_id, std::uint8_t opcode,
                    const std::string& payload)
    {
        const std::string frame = make_frame(opcode, payload, /*mask=*/false);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = sessions.find(conn_id);
        if (it == sessions.end() || it->second.fd == kInvalidSocket ||
            !it->second.handshake_done) {
            return false;
        }
        return send_all(it->second.fd, frame.data(), frame.size());
    }

    void session_loop(std::uint64_t conn_id, std::intptr_t fd_handle);
};

// 服务端会话线程：握手 → 拆帧分发；PING→PONG、CLOSE→回关。
// fd 归属：本线程是唯一 close 者（stop()/disconnect() 只 shutdown）
void WsServer::Impl::session_loop(std::uint64_t conn_id,
                                  std::intptr_t fd_handle)
{
    const SocketHandle fd = static_cast<SocketHandle>(fd_handle);

    // stop() 可能发生在本线程启动瞬间（accept 与 stop 的竞态窗口）：
    // 此时直接退出，避免白等一个握手超时
    if (!running.load()) {
        shutdown_socket(fd);
        close_socket(fd);
        std::lock_guard<std::mutex> lock(mutex);
        sessions.erase(conn_id);
        return;
    }

    // Upgrade 握手（未完成前不触发 on_connect、不接受 send）
    std::string leftover;
    std::string err;
    if (!server_handshake(fd, config.handshake_timeout_ms, leftover, err)) {
        spdlog::logger* log = LogFacade::logger();
        if (log) {
            log->warn("websocket server handshake failed (conn {}): {}",
                      conn_id, err);
        }
        shutdown_socket(fd);
        close_socket(fd);
        std::lock_guard<std::mutex> lock(mutex);
        sessions.erase(conn_id);
        return;  // 握手失败：不触发任何业务回调
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = sessions.find(conn_id);
        if (it != sessions.end()) {
            it->second.handshake_done = true;
        } else {
            // 握手期间 stop() 已清场：立即退出
            shutdown_socket(fd);
            close_socket(fd);
            return;
        }
    }
    std::function<void(std::uint64_t)> conn;
    {
        std::lock_guard<std::mutex> lock(mutex);
        conn = on_connect;
    }
    if (conn) {
        conn(conn_id);  // 锁外：回调可安全调 send()/disconnect()
    }

    std::string read_buf = std::move(leftover);
    char chunk[16 * 1024];
    bool close_sent = false;
    bool assembling = false;
    std::string asm_buf;

    for (;;) {
        if (!running.load()) {
            break;
        }
        bool readable = false;
        if (!wait_readable(fd, 100, readable)) {
            continue;  // 周期醒来检查 running
        }
        if (!readable) {
            continue;
        }
        const int n = static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
        if (n <= 0) {
            break;  // 对端关闭或连接错误
        }
        read_buf.append(chunk, static_cast<std::size_t>(n));
        std::vector<WsFrame> frames;
        if (!extract_frames(read_buf, /*expect_masked=*/true,
                            static_cast<std::size_t>(config.max_message_bytes),
                            frames)) {
            break;  // 协议违规（含掩码方向错误、长度炸弹）：断开
        }
        bool quit = false;
        for (const auto& f : frames) {
            if (f.opcode == kOpClose) {
                if (!close_sent) {
                    const std::string reply =
                        make_frame(kOpClose, f.payload, /*mask=*/false);
                    send_all(fd, reply.data(), reply.size());
                    close_sent = true;
                }
                quit = true;
                break;
            }
            if (f.opcode == kOpPing) {
                const std::string pong =
                    make_frame(kOpPong, f.payload, /*mask=*/false);
                if (!send_all(fd, pong.data(), pong.size())) {
                    quit = true;
                    break;
                }
                continue;
            }
            if (f.opcode == kOpPong) {
                continue;  // 心跳应答，无业务动作
            }
            if (f.opcode == kOpText || f.opcode == kOpBinary) {
                if (assembling) {
                    quit = true;  // 重组中收到新数据帧：协议违规
                    break;
                }
                if (f.fin) {
                    std::function<void(std::uint64_t, const std::string&)> msg;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        msg = on_message;
                    }
                    if (msg) {
                        msg(conn_id, f.payload);  // 锁外回调
                    }
                } else {
                    assembling = true;
                    asm_buf = f.payload;
                }
            } else if (f.opcode == kOpCont) {
                if (!assembling) {
                    quit = true;  // 无起始分片：协议违规
                    break;
                }
                asm_buf.append(f.payload);
                if (asm_buf.size() >
                    static_cast<std::size_t>(config.max_message_bytes)) {
                    quit = true;  // 重组超限
                    break;
                }
                if (f.fin) {
                    assembling = false;
                    std::function<void(std::uint64_t, const std::string&)> msg;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        msg = on_message;
                    }
                    if (msg) {
                        msg(conn_id, asm_buf);
                    }
                    asm_buf.clear();
                }
            }
        }
        if (quit) {
            break;
        }
    }

    // 收尾：摘会话 + 关 fd（握手成功才触发断连回调）
    const bool was_known = [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = sessions.find(conn_id);
        const bool known = it != sessions.end() && it->second.handshake_done;
        sessions.erase(conn_id);
        return known;
    }();
    shutdown_socket(fd);
    close_socket(fd);
    std::function<void(std::uint64_t, const std::string&)> disc;
    if (was_known) {
        std::lock_guard<std::mutex> lock(mutex);
        disc = on_disconnect;
    }
    if (disc) {
        disc(conn_id, "closed");
    }
}

WsServer::WsServer() : impl_(new Impl)
{
    net_initializer();
}

WsServer::WsServer(const WsConfig& config) : WsServer()
{
    impl_->config = config;
}

WsServer::~WsServer()
{
    stop();
    if (!impl_->self_detached) {
        delete impl_;
    }
    // self_detached：会话回调里析构过本对象——impl_ 交由残存线程自然消亡
}

bool WsServer::start(const std::string& host, std::uint16_t port)
{
    stop();

    if (!impl_->config.validate()) {
        return false;
    }

    SocketHandle l = ::socket(AF_INET, SOCK_STREAM, 0);
    if (l == kInvalidSocket) {
        return false;
    }
    int one = 1;
    ::setsockopt(l, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));

    std::uint32_t net_addr;
    if (host.empty() || host == "0.0.0.0") {
        net_addr = INADDR_ANY;
    } else {
        net_addr = resolve_ipv4_net(host);
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
    if (port == 0 &&
        ::getsockname(l, reinterpret_cast<::sockaddr*>(&bound), &blen) == 0) {
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
                ::accept(l, reinterpret_cast<::sockaddr*>(&peer_addr),
                         &addr_len);
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
            session.peer =
                std::string(ip) + ":" + std::to_string(ntohs(peer_addr.sin_port));

            std::uint64_t conn_id;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                conn_id = impl_->next_conn_id++;
                impl_->sessions[conn_id] = session;
            }
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->session_threads[conn_id] = std::thread(
                    [this, conn_id, client]() {
                        impl_->session_loop(conn_id,
                                            static_cast<std::intptr_t>(client));
                    });
            }
        }
    });
    return true;
}

void WsServer::stop()
{
    impl_->running.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        shutdown_socket(impl_->listener);
        close_socket(impl_->listener);
        impl_->listener = kInvalidSocket;
        // 只 shutdown 会话 fd：close 由各会话线程收尾时唯一执行，
        // 避免双关竞态（fd 复用时误关他人）
        for (auto& kv : impl_->sessions) {
            shutdown_socket(kv.second.fd);
            kv.second.fd = kInvalidSocket;
        }
    }
    if (impl_->accept_thread.joinable() &&
        impl_->accept_thread.get_id() != std::this_thread::get_id()) {
        impl_->accept_thread.join();
    }
    std::map<std::uint64_t, std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        threads = std::move(impl_->session_threads);
        impl_->session_threads.clear();
        impl_->sessions.clear();
    }
    for (auto& kv : threads) {
        if (!kv.second.joinable()) {
            continue;
        }
        if (kv.second.get_id() == std::this_thread::get_id()) {
            // 在会话回调里调 stop()：不能 join 自己，detach 让其自然收尾
            kv.second.detach();
            impl_->self_detached = true;
        } else {
            kv.second.join();
        }
    }
}

void WsServer::send(std::uint64_t conn_id, const std::string& payload)
{
    if (payload.size() >
        static_cast<std::size_t>(impl_->config.max_message_bytes)) {
        return;
    }
    impl_->send_frame(conn_id, kOpText, payload);
}

void WsServer::send_binary(std::uint64_t conn_id, const std::string& payload)
{
    if (payload.size() >
        static_cast<std::size_t>(impl_->config.max_message_bytes)) {
        return;
    }
    impl_->send_frame(conn_id, kOpBinary, payload);
}

void WsServer::broadcast(const std::string& payload)
{
    if (payload.size() >
        static_cast<std::size_t>(impl_->config.max_message_bytes)) {
        return;
    }
    const std::string frame = make_frame(kOpText, payload, /*mask=*/false);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& kv : impl_->sessions) {
        if (kv.second.fd != kInvalidSocket && kv.second.handshake_done) {
            send_all(kv.second.fd, frame.data(), frame.size());
        }
    }
}

void WsServer::disconnect(std::uint64_t conn_id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->sessions.find(conn_id);
    if (it != impl_->sessions.end() && it->second.fd != kInvalidSocket) {
        if (it->second.handshake_done) {
            const std::string frame = make_frame(
                kOpClose, close_payload("normal"), /*mask=*/false);
            send_all(it->second.fd, frame.data(), frame.size());
        }
        shutdown_socket(it->second.fd);  // 关闭由会话线程执行
        it->second.fd = kInvalidSocket;
    }
}

std::uint16_t WsServer::port() const
{
    return impl_->listen_port;
}

bool WsServer::is_running() const
{
    return impl_->running.load();
}

std::size_t WsServer::connection_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sessions.size();
}

void WsServer::set_on_message(
    std::function<void(std::uint64_t, const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_message = std::move(handler);
}

void WsServer::set_on_connect(std::function<void(std::uint64_t)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_connect = std::move(handler);
}

void WsServer::set_on_disconnect(
    std::function<void(std::uint64_t, const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_disconnect = std::move(handler);
}

}  // namespace libmini
