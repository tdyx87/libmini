#define _CRT_SECURE_NO_WARNINGS

// POSIX UDS 帧辅助仅非 Windows 编译单元可见；用它做条件闸门，
// 避免 Windows 分支下出现 unused-function 告警（-Werror 档）
#include "rpc.h"
#include "json_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <spdlog/logger.h>
#include <httplib.h>

#include "tcp.h"

// 注意：必须在 httplib.h 之后引入——它包含 Windows.h，
// 若先于 httplib 会把 _WIN32_WINNT 锁在旧值导致其静态断言失败
#ifdef _WIN32
#include "win_utf.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
// macOS 没有 MSG_NOSIGNAL（用 SO_NOSIGPIPE 套接字选项替代）
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define LIBMINI_BUILD_POSIX_FRAME
#endif

namespace libmini {

namespace {

// ==================== 传输端点识别 ====================

// 端点是否为本地传输：Windows 管道前缀或 UDS 路径（'/' 开头）
// 规范化本地端点：补全 "\\\\.\\pipe\\" 前缀（用户可能只写 "myrpc"）
std::string canonical_local_endpoint(const std::string& endpoint)
{
#ifdef _WIN32
    const char* kPrefix = "\\\\.\\pipe\\";
    if (endpoint.rfind(kPrefix, 0) == 0) {
        return endpoint;
    }
    return std::string(kPrefix) + endpoint;
#else
    return endpoint;
#endif
}

// 统一将 httplib 错误码映射为 libmini 的 RpcError
RpcError map_error(httplib::Error err)
{
    switch (err) {
        case httplib::Error::Success:           return RpcError::OK;
        case httplib::Error::Connection:        return RpcError::CONNECTION_FAILED;
        case httplib::Error::ConnectionTimeout: return RpcError::TIMEOUT;
        case httplib::Error::Read:
        case httplib::Error::Write:             return RpcError::TIMEOUT;
        default:                                return RpcError::UNKNOWN;
    }
}

std::string error_text(RpcError err)
{
    switch (err) {
        case RpcError::OK:                return "ok";
        case RpcError::CONNECTION_FAILED: return "connection failed";
        case RpcError::TIMEOUT:           return "timeout";
        case RpcError::SERVER_ERROR:      return "server error";
        case RpcError::PROTOCOL_ERROR:    return "protocol error";
        case RpcError::OVERLOADED:        return "server overloaded";
        case RpcError::UNKNOWN:           return "unknown error";
    }
    return "unknown error";
}

// HTTP 状态码语义映射到本地传输的 status 字段
const char* http_semantic_to_status(int http_status)
{
    switch (http_status) {
        case 200: return "ok";
        case 400: return "bad_request";
        case 404: return "not_found";
        case 429: return "overloaded";
        case 500: return "handler_error";
        default:  return "error";
    }
}

// 判定连接失败：Windows 上非阻塞 connect 对不可达端口通常表现为
// ConnectionTimeout（select 超时）而非 Connection，本机端口（127.0.0.1）上
// 几乎总是这种情况，因此将本机地址的超时归为连接失败
bool is_local_address(const std::string& host)
{
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// 解析 Retry-After 头（RFC 7231 §5.5.2）：
//   1) delta-seconds：非负整数（如 "30"）；
//   2) HTTP-date：IMF-fixdate（如 "Wed, 21 Oct 2015 07:28:00 GMT"），
//      取与当前时间的差值。
// 解析成功返回等待毫秒数（可为 0 表示立即重试）；无法解析返回 -1（无建议）。
long long parse_retry_after_ms(const std::string& value)
{
    // ---- 形式 1：纯数字 ----
    if (!value.empty() &&
        value.find_first_not_of("0123456789 ") == std::string::npos) {
        return atoll(value.c_str()) * 1000;
    }

    // ---- 形式 2：IMF-fixdate "Wed, 21 Oct 2015 07:28:00 GMT" ----
    // 宽容解析：找 "DD Mon YYYY HH:MM:SS" 五段即可（星期/时区部分忽略）
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char month[4] = {};
    if (std::sscanf(value.c_str(), "%*[^0-9]%d %3s %d %d:%d:%d", &day, month,
                    &year, &hour, &minute, &second) != 6) {
        return -1;
    }
    int mon = -1;
    for (int i = 0; i < 12; ++i) {
        if (month[0] == kMonths[i][0] && month[1] == kMonths[i][1] &&
            month[2] == kMonths[i][2]) {
            mon = i;
            break;
        }
    }
    if (mon < 0 || year < 1970 || day < 1 || day > 31) {
        return -1;
    }

    // 把该日期当作 UTC 时间点（HTTP-date 语义），换算为 unix 时间戳：
    // 采用 civil-from-days 算法（Howard Hinnant）
    long long y = year;
    y -= mon <= 1 ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (mon + (mon > 1 ? -3 : 9)) + 2) / 5 +
                         static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 +
                           static_cast<long long>(doe) - 719468;
    const long long server_time_s =
        days * 86400 + hour * 3600 + minute * 60 + second;

    const long long now_s = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const long long delta = server_time_s - now_s;
    return delta > 0 ? delta * 1000 : 0;  // 过去的时刻 = 立即重试
}

// 将处理器返回值包装为统一响应体 {"result":...,"error":null}
std::string make_response(const JsonValue& result, const char* error)
{
    JsonValue out;
    out["result"] = result;
    out["error"] = error ? JsonValue(error) : JsonValue();
    return to_json_string(out);
}

// httplib 默认线程数的计算方式（见 CPPHTTPLIB_THREAD_POOL_COUNT 宏）
std::size_t default_worker_threads()
{
    const unsigned hc = std::thread::hardware_concurrency();
    return static_cast<std::size_t>(std::max(8u, hc > 0 ? hc - 1 : 0u));
}

// ------------------ 毫秒级对数分桶延迟直方图 ------------------
//
// 桶边界为等比序列（下界 1µs，相邻边界比值 1.25），固定 96 个桶：
// 覆盖 1µs ~ 41 分钟，桶宽 25%；无动态分配、每次记录仅 O(log buckets)，
// 避免为每个请求保留样本导致的内存增长；分位数由命中桶内的线性插值
// 估算，样本量大时误差相对桶宽可忽略。
class LatencyHistogram
{
public:
    LatencyHistogram()
    {
        double edge = kMinMs;
        for (std::size_t i = 0; i < kBucketCount + 1; ++i) {
            bounds[i] = edge;
            edge *= kBucketRatio;
        }
        bounds[kBucketCount + 1] = kInfMs;
    }

    void record(double ms)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const std::size_t idx = bucket_index(ms);
        if (idx < kBucketCount) {
            ++counts[idx];
        }
        ++total;
        sum_ms += ms;
        if (ms < min_ms) {
            min_ms = ms;
        }
        if (ms > max_ms) {
            max_ms = ms;
        }
    }

    // 置零（服务器重新启动时复用）
    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            counts[i] = 0;
        }
        total = 0;
        sum_ms = 0.0;
        min_ms = kInfMs;
        max_ms = 0.0;
    }

    std::size_t count() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return total;
    }

    // 分位数估算（percentile 取值 [0,100]；无样本返回 -1）。
    // 定位分数秩 p/100*N 落入的桶，按桶内位置线性插值；
    // 结果 clamp 到 [min_ms, max_ms]，保证分位数序列单调且不越界
    double percentile(double percentile) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (total == 0) {
            return -1.0;
        }
        if (total == 1) {
            return min_ms;  // 唯一样本即 min=max
        }
        double target = percentile / 100.0 * static_cast<double>(total);
        if (target < 1.0) {
            target = 1.0;
        }
        if (target > static_cast<double>(total)) {
            target = static_cast<double>(total);
        }
        std::size_t seen = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            if (counts[i] > 0 &&
                target <= static_cast<double>(seen + counts[i])) {
                const double f =
                    (target - static_cast<double>(seen)) /
                    static_cast<double>(counts[i]);
                double v = bounds[i] +
                           (bounds[i + 1] - bounds[i]) * f;
                if (v < min_ms) {
                    v = min_ms;
                }
                if (v > max_ms) {
                    v = max_ms;
                }
                return v;
            }
            seen += counts[i];
        }
        return max_ms;
    }

    void snapshot(RpcLatencyStats& out) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        out.sample_count = total;
        out.min_ms = total ? min_ms : 0.0;
        out.max_ms = max_ms;
        out.mean_ms = total ? sum_ms / static_cast<double>(total) : 0.0;
    }

    // 供 percentile() 复用锁内的 total 判断（见 latency_stats 组装）
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return total == 0;
    }

private:
    static const std::size_t kBucketCount = 96;
    static const double kMinMs;       // 桶下界 0.001ms（1µs）
    static const double kBucketRatio; // 相邻桶边界比值 1.25
    static const double kInfMs;

    std::size_t bucket_index(double ms) const
    {
        if (ms < bounds[1]) {
            return 0;
        }
        if (ms >= bounds[kBucketCount]) {
            return kBucketCount;  // 超界桶（记录到 total，但不计入分布）
        }
        // bounds 单调递增（ratio>1），二分找第一个 > ms 的边界
        std::size_t lo = 1, hi = kBucketCount;  // ms < bounds[hi] 恒成立
        while (hi - lo > 1) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (ms < bounds[mid]) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        return lo;  // bounds[lo] <= ms < bounds[lo+1]
    }

    mutable std::mutex mutex;
    double bounds[kBucketCount + 2];
    std::size_t counts[kBucketCount] = {};
    std::size_t total = 0;
    double sum_ms = 0.0;
    double min_ms = kInfMs;
    double max_ms = 0.0;
};

const double LatencyHistogram::kMinMs = 0.001;
const double LatencyHistogram::kBucketRatio = 1.25;
const double LatencyHistogram::kInfMs = 1e30;

// ==================== 帧协议工具（本地传输与 Tcp 传输共用）====================
// 帧格式：4 字节小端长度 + JSON 文本；一连接一请求（客户端可复用连接）。
// 响应 JSON 里的 status 字段携带与 HTTP 语义对齐的状态：
//   "ok" / "bad_request" / "not_found" / "overloaded" / "handler_error"
// 过载时另有 retry_after_s 字段（整数秒），与 HTTP Retry-After 头对应。

// u32 长度头（小端）追加到 string。调用点全部在 Windows 管道分支
//（POSIX UDS 走裸缓冲区重载），非 Windows 编译单元不编译本函数
#ifdef _WIN32
void put_u32_le(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}
#endif

// 裸缓冲区重载（POSIX UDS 组帧专用，仅非 Windows 编译单元使用）
#ifdef LIBMINI_BUILD_POSIX_FRAME
void put_u32_le(char* out, std::uint32_t v)
{
    out[0] = static_cast<char>(v & 0xFF);
    out[1] = static_cast<char>((v >> 8) & 0xFF);
    out[2] = static_cast<char>((v >> 16) & 0xFF);
    out[3] = static_cast<char>((v >> 24) & 0xFF);
}
#endif

std::uint32_t get_u32_le(const char* p)
{
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(b[0]) |
           (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) |
           (static_cast<std::uint32_t>(b[3]) << 24);
}

// 单帧上限：防御性限制（正常 RPC 报文远小于此）
const std::uint32_t kMaxFrameBytes = 64u * 1024 * 1024;

#ifdef _WIN32
// ------------------ 重叠 I/O 辅助（重叠管道句柄专用） ------------------
// 背景见 ensure_session：同一管道句柄需要读写并发，非重叠句柄的
// 同步 ReadFile/WriteFile 会在内核层互斥（三方死锁），必须用重叠句柄。
// 单事件顺序重叠调用：一次只有一个未完成 IO，事件复用安全。
//
// wait_ms = INFINITE 表示无超时阻塞；超时后 CancelIoEx 取消未完成 IO
// 并返回 false（串行交换路径的读超时语义由这里承接）。

// 同步语义写全部字节；返回是否成功
bool pipe_ov_write(HANDLE h, const char* data, std::uint32_t len,
                   DWORD wait_ms = INFINITE)
{
    OVERLAPPED ov;
    std::memset(&ov, 0, sizeof(ov));
    ov.hEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == nullptr) {
        return false;
    }
    const BOOL ok = ::WriteFile(h, data, len, NULL, &ov);
    bool done = false;
    if (ok) {
        DWORD written = 0;
        done = ::GetOverlappedResult(h, &ov, &written, FALSE) &&
               written == len;
    } else if (::GetLastError() == ERROR_IO_PENDING) {
        if (::WaitForSingleObject(ov.hEvent, wait_ms) == WAIT_OBJECT_0) {
            DWORD written = 0;
            done = ::GetOverlappedResult(h, &ov, &written, FALSE) &&
                   written == len;
        } else {
            (void)::CancelIoEx(h, &ov);
            DWORD written = 0;
            (void)::GetOverlappedResult(h, &ov, &written, TRUE);
        }
    }
    ::CloseHandle(ov.hEvent);
    return done;
}

// 同步语义读 len 字节（short read 视为错误）；返回是否成功
bool pipe_ov_read(HANDLE h, char* data, std::uint32_t len,
                  DWORD wait_ms = INFINITE)
{
    OVERLAPPED ov;
    std::memset(&ov, 0, sizeof(ov));
    ov.hEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == nullptr) {
        return false;
    }
    const BOOL ok = ::ReadFile(h, data, len, NULL, &ov);
    bool done = false;
    if (ok) {
        DWORD got = 0;
        done = ::GetOverlappedResult(h, &ov, &got, FALSE) && got == len;
    } else if (::GetLastError() == ERROR_IO_PENDING) {
        if (::WaitForSingleObject(ov.hEvent, wait_ms) == WAIT_OBJECT_0) {
            DWORD got = 0;
            done = ::GetOverlappedResult(h, &ov, &got, FALSE) && got == len;
        } else {
            (void)::CancelIoEx(h, &ov);
            DWORD got = 0;
            (void)::GetOverlappedResult(h, &ov, &got, TRUE);
        }
    }
    ::CloseHandle(ov.hEvent);
    return done;
}
#endif  // _WIN32

}  // namespace

// ==================== Tcp 传输端点解析 ====================
// "host:port" → host + port；无冒号视为纯端口号（host 默认 0.0.0.0）
void parse_host_port(const std::string& endpoint, std::string& host, int& port)
{
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
        host = "0.0.0.0";
        port = std::atoi(endpoint.c_str());
        return;
    }
    host = endpoint.substr(0, colon);
    port = std::atoi(endpoint.substr(colon + 1).c_str());
    if (host.empty()) {
        host = "0.0.0.0";
    }
}

// ==================== RpcClient ====================

#ifndef _WIN32
// POSIX UDS 辅助函数（定义在 RpcServer 段；客户端本地传输先于此使用，
// GCC 的单遍查找要求使用点之前有声明——MSVC permissive 模式会放行）
#ifdef LIBMINI_BUILD_POSIX_FRAME
bool uds_send_frame(int fd, const std::string& payload);
bool uds_recv_frame(int fd, std::string& payload);
#endif
bool uds_connect(int fd, const std::string& path, int timeout_ms);
#endif

// 一次 RPC 尝试的结果（传输无关）
struct AttemptResult
{
    bool transport_ok = false;   // 是否收到了完整的响应（false = 连接/超时类错误）
    RpcError error = RpcError::UNKNOWN;
    std::string error_detail;    // 人类可读详情（last_error_message 用）
    int http_status = 200;       // 语义状态（本地传输由 status 字段映射）
    int retry_after_ms = 0;      // >0 = 服务器建议的等待
    std::string body;            // 响应 JSON 文本
};

struct RpcClient::Impl
{
    RpcTransport transport = RpcTransport::Http;
    std::string host;      // HTTP
    int port = 0;          // HTTP
    std::string endpoint;  // 本地（已规范化）；Tcp 传输保持原样
    int timeout_ms = 5000;

    // Tcp 传输端点解析结果
    std::string tcp_host;
    int tcp_port = 0;

    // 重试与退避配置
    int max_retries = 3;
    int retry_base_delay_ms = 100;
    int retry_max_delay_ms = 4000;
    int retry_max_total_wait_ms = 10000;
    int retry_after_ms = 0;  // 最近一次 429 响应的 Retry-After（毫秒），0 = 未提供
    bool jitter_enabled = false;

    // 重试日志器（可为空 = 不记录）；指针由外部持有，热更新无需加锁
    spdlog::logger* logger = nullptr;

    // 随机数源：mutex 保护（thread_local 引擎开销更小，但 msvc 2017 下
    // thread_local + 静态初始化在静态库场景偶有初始化顺序问题，保持简单）
    std::mutex rng_mutex;
    std::mt19937 rng{std::random_device{}()};

    RpcError last_error = RpcError::UNKNOWN;
    std::string last_message = "no request made";
    double last_call_ms = -1.0;  // 最近一次 call() 总耗时（含重试等待）

    // HTTP 客户端懒构造（本地传输用不到，避免无谓开销）
    std::unique_ptr<httplib::Client> http;

    // ------------------ 连接池（LocalPipe/Tcp 传输）------------------

    // Tcp 连接的同步等待状态：on_message/on_disconnect 回调唤醒请求线程。
    // 每个池连接独占一份，请求-响应严格串行，无跨请求串扰
    struct TcpWaitState
    {
        std::mutex m;
        std::condition_variable cv;
        std::string payload;
        bool got = false;
        bool broken = false;
    };

    // 前向声明：PooledConn 持有流水线会话，完整定义在其后
    struct PipeSession;

    // 池中的一个连接：平台句柄（管道/UDS）或 TcpClient，析构即关闭
    struct PooledConn
    {
#ifdef _WIN32
        HANDLE pipe = INVALID_HANDLE_VALUE;
#else
        int fd = -1;
#endif
        std::unique_ptr<TcpClient> tcp;
        std::shared_ptr<TcpWaitState> tcp_state;
        // 流水线会话（启用时惰性创建；池连接生命周期内复用）。
        // session_mu 串行化惰性创建：并发首用若各自创建会话，会
        // 丢失回调/读线程注册，在途请求永远等不到响应
        std::mutex session_mu;
        std::shared_ptr<PipeSession> session;
        std::chrono::steady_clock::time_point last_used;

        ~PooledConn()
        {
#ifdef _WIN32
            if (pipe != INVALID_HANDLE_VALUE) {
                ::CloseHandle(pipe);
            }
#else
            if (fd >= 0) {
                ::close(fd);
            }
#endif
            if (session) {
                session->request_close();
                if (session->reader.joinable()) {
                    session->reader.join();
                }
            }
            if (tcp) {
                tcp->close();
            }
        }
    };

    // 连接池：空闲队列 + 并发名额 + 计数，所有方法线程安全。
    // 淘汰策略为惰性回收：取用/归还时顺手清掉过期空闲连接，不设专职线程。
    // 空闲队列 front 为最旧连接：淘汰从最旧开始，复用也优先旧连接（先进先出，
    // 让「久未用」的连接尽早被检验或淘汰，热连接常驻队尾）
    struct ConnPool
    {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::unique_ptr<PooledConn>> idle;  // front = 最旧
        std::size_t busy = 0;  // 已借出（含新建中）的连接数

        std::uint64_t created_total = 0;
        std::uint64_t reused_total = 0;
        std::uint64_t closed_total = 0;

        enum class Lease { Idle, Create, Timeout };

        Lease acquire(std::unique_ptr<PooledConn>& out, std::size_t max_conns,
                      int idle_timeout_ms, int wait_ms)
        {
            std::unique_lock<std::mutex> lock(m);
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(wait_ms);
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                evict_expired_locked(now, idle_timeout_ms);
                if (!idle.empty()) {
                    // 连接从「空闲」转入「借出」：两个计数一减一增，
                    // open = idle + busy 恒为实际打开数
                    out = std::move(idle.front());
                    idle.pop_front();
                    ++busy;
                    ++reused_total;
                    return Lease::Idle;
                }
                if (busy + idle.size() < max_conns) {
                    ++busy;
                    ++created_total;
                    return Lease::Create;
                }
                // 达到并发上限：等待其他调用归还连接
                if (cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                    // 截止前最后检查一次（归还可能恰在唤醒与超时之间发生）
                    evict_expired_locked(std::chrono::steady_clock::now(),
                                         idle_timeout_ms);
                    if (!idle.empty()) {
                        out = std::move(idle.front());
                        idle.pop_front();
                        ++busy;
                        ++reused_total;
                        return Lease::Idle;
                    }
                    if (busy + idle.size() < max_conns) {
                        ++busy;
                        ++created_total;
                        return Lease::Create;
                    }
                    return Lease::Timeout;
                }
            }
        }

        // 用毕归还：空闲容量未满则入池，否则关闭（超出容量）
        void release(std::unique_ptr<PooledConn> conn, std::size_t max_idle,
                     int idle_timeout_ms)
        {
            {
                std::lock_guard<std::mutex> lock(m);
                --busy;
                conn->last_used = std::chrono::steady_clock::now();
                if (idle.size() < max_idle) {
                    idle.push_back(std::move(conn));
                } else {
                    ++closed_total;
                }
                evict_expired_locked(std::chrono::steady_clock::now(),
                                     idle_timeout_ms);
            }
            cv.notify_one();
        }

        // 传输级失败：连接不可信，直接关闭并归还并发名额
        void discard(std::unique_ptr<PooledConn> /*conn*/)
        {
            {
                std::lock_guard<std::mutex> lock(m);
                --busy;
                ++closed_total;
            }
            cv.notify_one();
        }

        // 新建失败：撤销名额与计数
        void cancel_create()
        {
            {
                std::lock_guard<std::mutex> lock(m);
                --busy;
                --created_total;
            }
            cv.notify_one();
        }

        void evict_expired_locked(std::chrono::steady_clock::time_point now,
                                  int idle_timeout_ms)
        {
            if (idle_timeout_ms <= 0) {
                return;
            }
            while (!idle.empty()) {
                const auto age_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - idle.front()->last_used)
                        .count();
                if (age_ms < idle_timeout_ms) {
                    break;
                }
                idle.pop_front();
                ++closed_total;
            }
        }

        RpcClientPoolStats stats()
        {
            std::lock_guard<std::mutex> lock(m);
            RpcClientPoolStats s;
            s.idle_connections = idle.size();
            s.busy_connections = busy;
            s.open_connections = idle.size() + busy;
            s.created_total = created_total;
            s.reused_total = reused_total;
            s.closed_total = closed_total;
            return s;
        }

        // 关闭全部空闲连接（禁用池时立即释放）
        void close_all_idle()
        {
            std::lock_guard<std::mutex> lock(m);
            closed_total += idle.size();
            idle.clear();
        }
    };

    // ------------------ 请求流水线（帧传输）------------------
    //
    // 协议扩展：请求 JSON 顶层可带 "id"（自增序号），服务器原样回带；
    // 客户端凭 id 在在途表中匹配响应。无 id 的旧服务器响应由 reader
    // 线程转给等待者（每连接同一时刻只有一个无 id 等待者，见下）。

    // 流水线模式下每个池连接挂一个读取线程：持续收帧，按响应 id
    // 唤醒对应等待者（等待者各自在 AttemptResult 里收自己的响应）
    struct PendingCall
    {
        std::mutex m;
        std::condition_variable cv;
        std::string payload;
        bool done = false;
        bool broken = false;
    };

    // 一条连接上的流水线会话：在途表 + 读取线程 + 发送锁
    struct PipeSession
    {
        // Tcp 与本地统一封装：发帧与收帧由传输相关 lambda 提供
        std::function<bool(const std::string&)> send_frame;
        std::function<void()> request_close;

        std::mutex m;
        std::map<std::uint64_t, std::shared_ptr<PendingCall>> inflight;
        std::shared_ptr<PendingCall> anon_waiter;  // 无 id 响应的退路接收者
        std::uint64_t next_id = 1;
        bool broken = false;
        std::string broken_reason = "connection closed";

        std::thread reader;
        bool reader_started = false;

        std::mutex send_mutex;  // 帧写入互斥（管道/Tcp 单写者）

        void mark_broken(const std::string& reason)
        {
            std::vector<std::shared_ptr<PendingCall>> wake;
            {
                std::lock_guard<std::mutex> lock(m);
                if (broken) {
                    return;
                }
                broken = true;
                broken_reason = reason;
                for (auto& kv : inflight) {
                    wake.push_back(kv.second);
                }
                inflight.clear();
                if (anon_waiter) {
                    wake.push_back(anon_waiter);
                    anon_waiter.reset();
                }
            }
            for (auto& p : wake) {
                std::lock_guard<std::mutex> lk(p->m);
                p->broken = true;
                p->done = true;
                p->cv.notify_all();
            }
        }

        void deliver(const std::string& payload, bool has_id,
                     std::uint64_t id)
        {
            std::shared_ptr<PendingCall> target;
            {
                std::lock_guard<std::mutex> lock(m);
                if (has_id) {
                    auto it = inflight.find(id);
                    if (it != inflight.end()) {
                        target = it->second;
                        inflight.erase(it);
                    }
                } else if (anon_waiter) {
                    target = anon_waiter;
                    anon_waiter.reset();
                }
            }
            if (target) {
                std::lock_guard<std::mutex> lk(target->m);
                target->payload = payload;
                target->done = true;
                target->cv.notify_all();
            }
            // 无主响应（超时已被等待者放弃）：丢弃。连接仍健康——
            // 帧边界完整，仅该请求的调用方已离开
        }
    };

    std::unique_ptr<ConnPool> pool;
    std::size_t pool_max_conns = 8;   // 0 = 禁用池（一调用一连接）
    std::size_t pool_max_idle = 4;
    int pool_idle_timeout_ms = 30000;
    std::size_t pipeline_max_in_flight = 0;  // 0 = 关闭流水线
    std::atomic<std::uint64_t> next_pipeline_id{1};  // 请求序号源

    // 流水线专用通道：一条专用连接上的会话并发承载多个在途请求，
    // 在途槽位由 pipeline_max_in_flight 限流（acquire/release 语义与
    // 连接池一致）。独立于连接池：串行模式走池（每次调用独占连接），
    // 流水线模式全部请求共享这条连接，QPS 不再受「连接数 = 并发数」
    // 限制，而只受在途槽位与服务器处理能力限制
    struct PipelineChannel
    {
        mutable std::mutex m;  // pool_stats() 快照读取也要加锁
        std::condition_variable cv;
        std::size_t busy = 0;
        bool broken = false;  // 连接断开：下次 acquire 后重建

        // 可观测性计数：total 类用 atomic 无锁自增（热路径），
        // in_flight_peak 与 busy 同受 m 保护（快照读取口径一致）
        std::atomic<std::uint64_t> reused_total{0};    // 单连接承载的请求总数
        std::atomic<std::uint64_t> broken_total{0};    // 断线重建次数
        std::atomic<std::uint64_t> slot_wait_total{0}; // 槽位等待（竞争）次数
        std::atomic<std::uint64_t> slot_timeout_total{0};  // 槽位等待超时次数
        std::size_t in_flight_peak = 0;  // m 保护

        std::mutex setup_mutex;             // 保护 conn/session 惰性建立
        // shared_ptr 共享所有权：断线重建只释放“最后一个使用者之后”的
        // 连接（正在使用旧连接的调用持有副本，不会遭遇 UAF）
        std::shared_ptr<PooledConn> conn;

        // 拿一个在途槽位；超过上限阻塞等待，等待上限 = 请求超时。
        // waited 非空时输出本次是否经历了竞争等待（统计口径用）
        bool acquire_slot(std::size_t max_in_flight, int timeout_ms,
                          bool* waited = NULL)
        {
            std::unique_lock<std::mutex> lock(m);
            if (busy >= max_in_flight) {
                if (waited != NULL) {
                    *waited = true;
                }
                slot_wait_total.fetch_add(1, std::memory_order_relaxed);
                const bool ok = cv.wait_for(lock,
                    std::chrono::milliseconds(timeout_ms),
                    [&] { return busy < max_in_flight; });
                if (!ok) {
                    slot_timeout_total.fetch_add(1,
                        std::memory_order_relaxed);
                    return false;
                }
            }
            ++busy;
            if (busy > in_flight_peak) {
                in_flight_peak = busy;
            }
            return true;
        }

        void release_slot()
        {
            {
                std::lock_guard<std::mutex> lock(m);
                --busy;
            }
            cv.notify_one();
        }

        void mark_broken()
        {
            {
                std::lock_guard<std::mutex> lock(m);
                broken = true;
            }
            cv.notify_all();  // 唤醒等待槽位者去走重建路径
        }
    };
    std::unique_ptr<PipelineChannel> pipeline;
    mutable std::mutex pipeline_mutex;  // 保护 pipeline 指针的惰性创建（并发首用竞态）

    Impl(const std::string& host_, int port_)
        : transport(RpcTransport::Http), host(host_), port(port_)
    {
        apply_timeout();
    }

    Impl(RpcTransport t, const std::string& endpoint_)
        : transport(t),
          endpoint(t == RpcTransport::LocalPipe
                       ? canonical_local_endpoint(endpoint_)
                       : endpoint_)
    {
        if (t == RpcTransport::Tcp) {
            parse_host_port(endpoint, tcp_host, tcp_port);
        }
        if (t == RpcTransport::Http && !endpoint.empty()) {
            // "host:port" 字符串端点同样可用（与 Tcp 传输形式一致）；
            // 不初始化的话 attempt_http 会解引用空客户端
            parse_host_port(endpoint, host, port);
            apply_timeout();
        }
        if (t != RpcTransport::Http) {
            pool.reset(new ConnPool());
        }
    }

    void apply_timeout()
    {
        if (transport != RpcTransport::Http) {
            return;
        }
        if (!http) {
            http.reset(new httplib::Client(host, port));
        }
        const time_t sec = timeout_ms / 1000;
        const time_t usec = (timeout_ms % 1000) * 1000;
        http->set_connection_timeout(sec, usec);
        http->set_read_timeout(sec, usec);
        http->set_write_timeout(sec, usec);
    }

    std::string describe_endpoint() const
    {
        return transport == RpcTransport::Http
                   ? host + ":" + std::to_string(port)
                   : (transport == RpcTransport::Tcp
                          ? tcp_host + ":" + std::to_string(tcp_port)
                          : endpoint);
    }

    // ------------------ 本地传输：连接建立与帧交换 ------------------
#ifdef _WIN32
    // 新建命名管道连接（含所有实例忙时的 WaitNamedPipe 等待重试）
    bool local_open_conn(HANDLE& out_pipe, int timeout_ms_, std::string& err)
    {
        const std::wstring wname = internal::utf8_to_wide(endpoint);
        const DWORD timeout = static_cast<DWORD>(timeout_ms_);
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (int busy_retry = 0; busy_retry < 40; ++busy_retry) {
            // FILE_FLAG_OVERLAPPED：流水线会话要求同一句柄读写并发
            //（读线程阻塞收帧 + 调用线程写请求）。非重叠句柄的同步
            // ReadFile/WriteFile 在内核层互斥，会三方死锁（见
            // pipe_ov_* 辅助函数注释）
            pipe = ::CreateFileW(
                wname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (pipe != INVALID_HANDLE_VALUE) {
                break;
            }
            const DWORD last_err = ::GetLastError();
            if (last_err != ERROR_PIPE_BUSY) {
                break;
            }
            // 所有实例忙：等服务器释放一个实例（标准客户端模式），
            // 总等待不超过本次调用的超时预算
            if (!::WaitNamedPipeW(wname.c_str(),
                                  timeout < 500 ? timeout : 500)) {
                break;
            }
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            const DWORD last_err = ::GetLastError();
            if (last_err == ERROR_PIPE_BUSY || last_err == ERROR_SEM_TIMEOUT) {
                err = "connection failed: pipe busy";
            } else {
                err = "connection failed: pipe not found";
            }
            return false;
        }
        out_pipe = pipe;
        return true;
    }

    // 在既有管道连接上执行一次「发请求帧 → 收响应帧」交换
    AttemptResult local_exchange(HANDLE pipe, const std::string& request_body,
                                 int timeout_ms_)
    {
        AttemptResult r;
        // 读写超时（CollectReadTimeout 的时钟粒度由系统定时器决定，
        // 500ms 内的值可能向上取整到粒度边界）
        DWORD read_timeout = static_cast<DWORD>(timeout_ms_);
        (void)::SetNamedPipeHandleState(pipe, PIPE_READMODE_BYTE, NULL,
                                        &read_timeout);

        std::string frame;
        put_u32_le(frame, static_cast<std::uint32_t>(request_body.size()));
        frame += request_body;
        if (!pipe_ov_write(pipe, frame.data(),
                           static_cast<std::uint32_t>(frame.size()),
                           read_timeout)) {
            r.error = RpcError::CONNECTION_FAILED;
            r.error_detail = "connection failed: pipe write error";
            return r;
        }

        char head[4];
        if (!pipe_ov_read(pipe, head, 4, read_timeout)) {
            r.error = RpcError::TIMEOUT;
            r.error_detail = "timeout: pipe read header";
            return r;
        }
        const std::uint32_t len = get_u32_le(head);
        if (len > kMaxFrameBytes) {
            r.error = RpcError::PROTOCOL_ERROR;
            r.error_detail = "protocol error: frame too large";
            return r;
        }
        std::string payload(len, '\0');
        if (len > 0 &&
            !pipe_ov_read(pipe, &payload[0], len, read_timeout)) {
            r.error = RpcError::TIMEOUT;
            r.error_detail = "timeout: pipe read body";
            return r;
        }

        r.transport_ok = true;
        r.body = payload;
        parse_local_status(r);
        return r;
    }
#else
    // 新建 UDS 连接
    bool local_open_conn(int& out_fd, int timeout_ms_, std::string& err)
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            err = "connection failed: socket() error";
            return false;
        }
        if (!uds_connect(fd, endpoint, timeout_ms_)) {
            ::close(fd);
            err = "connection failed: endpoint not reachable";
            return false;
        }
        out_fd = fd;
        return true;
    }

    // 在既有 UDS 连接上执行一次帧交换
    AttemptResult local_exchange(int fd, const std::string& request_body,
                                 int timeout_ms_)
    {
        AttemptResult r;
        // 收发超时（每次交换前重设，池连接可能跨多次调用使用）
        timeval tv;
        tv.tv_sec = timeout_ms_ / 1000;
        tv.tv_usec = (timeout_ms_ % 1000) * 1000;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (!uds_send_frame(fd, request_body)) {
            r.error = RpcError::CONNECTION_FAILED;
            r.error_detail = "connection failed: send error";
            return r;
        }
        std::string payload;
        if (!uds_recv_frame(fd, payload)) {
            r.error = RpcError::TIMEOUT;
            r.error_detail = "timeout: recv";
            return r;
        }
        r.transport_ok = true;
        r.body = payload;
        parse_local_status(r);
        return r;
    }
#endif

    // ------------------ Tcp 传输：连接建立与帧交换 ------------------
    // 新建 Tcp 长连接（回调在连接前注册，避免首响应竞态）
    bool tcp_open_conn(PooledConn& conn, std::string& err)
    {
        TcpConfig cfg;
        cfg.connect_timeout_ms = timeout_ms;
        cfg.max_frame_bytes = static_cast<int>(kMaxFrameBytes);
        cfg.heartbeat_interval_ms = 0;  // 请求-响应模式无需心跳

        conn.tcp.reset(new TcpClient(cfg));
        conn.tcp_state = std::make_shared<TcpWaitState>();
        const std::shared_ptr<TcpWaitState> st = conn.tcp_state;
        conn.tcp->set_on_message([st](const std::string& msg) {
            std::lock_guard<std::mutex> lock(st->m);
            st->payload = msg;
            st->got = true;
            st->cv.notify_one();
        });
        conn.tcp->set_on_disconnect([st](const std::string&) {
            std::lock_guard<std::mutex> lock(st->m);
            st->broken = true;
            st->cv.notify_one();
        });
        if (!conn.tcp->connect(tcp_host,
                               static_cast<std::uint16_t>(tcp_port))) {
            err = "connection failed: tcp connect " + describe_endpoint();
            return false;
        }
        return true;
    }

    // 在既有 Tcp 连接上执行一次帧交换
    AttemptResult tcp_exchange(PooledConn& conn,
                               const std::string& request_body,
                               int timeout_ms_)
    {
        AttemptResult r;
        const std::shared_ptr<TcpWaitState> st = conn.tcp_state;
        {
            std::lock_guard<std::mutex> lock(st->m);
            st->payload.clear();
            st->got = false;
            st->broken = false;
        }
        if (!conn.tcp->send(request_body)) {
            r.error = RpcError::CONNECTION_FAILED;
            r.error_detail = "connection failed: tcp send error";
            return r;
        }
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(st->m);
            // 等待响应（server auto-replies PONG to probe PINGs,
            // never on_message）
            const bool have = st->cv.wait_for(
                lock, std::chrono::milliseconds(timeout_ms_),
                [&] { return st->got || st->broken; });
            if (!have) {
                r.error = RpcError::TIMEOUT;
                r.error_detail = "timeout: tcp read response";
                return r;
            }
            if (st->broken && !st->got) {
                r.error = RpcError::CONNECTION_FAILED;
                r.error_detail = "connection closed before response";
                return r;
            }
            payload = st->payload;
        }
        r.transport_ok = true;
        r.body = payload;
        parse_local_status(r);
        return r;
    }

    // 从帧载荷提取顶层 "id" 字段（流水线协议扩展）；无 id 返回 false
    static bool extract_frame_id(const std::string& payload,
                                 std::uint64_t& out_id)
    {
        try {
            const JsonValue resp = JsonValue::parse(payload);
            if (resp.is_object() && resp.contains("id") &&
                resp["id"].is_number()) {
                out_id = resp["id"].get<std::uint64_t>();
                return true;
            }
        } catch (const std::exception&) {
            // 非 JSON 响应按无 id 处理（交给无 id 等待者/丢弃）
        }
        return false;
    }

    // 响应 JSON 的 status/retry_after_s 字段 → AttemptResult 语义状态
    //（与 HTTP 状态码语义对齐，重试层对传输无感知）
    static void parse_local_status(AttemptResult& r)
    {
        try {
            const JsonValue resp = JsonValue::parse(r.body);
            const std::string status =
                resp.value("status", std::string("ok"));
            if (status == "ok") {
                r.http_status = 200;
            } else if (status == "bad_request") {
                r.http_status = 400;
            } else if (status == "not_found") {
                r.http_status = 404;
            } else if (status == "overloaded") {
                r.http_status = 429;
                r.retry_after_ms =
                    resp.value("retry_after_s", 1) * 1000;
            } else {
                r.http_status = 500;
            }
        } catch (const std::exception&) {
            r.error = RpcError::PROTOCOL_ERROR;
            r.error_detail = "protocol error: response not valid JSON";
            r.transport_ok = false;
        }
    }

    // 按传输在既有连接上做一次交换
    AttemptResult exchange_on(PooledConn& conn,
                              const std::string& request_body,
                              int timeout_ms_)
    {
        if (transport == RpcTransport::Tcp) {
            return tcp_exchange(conn, request_body, timeout_ms_);
        }
#ifdef _WIN32
        return local_exchange(conn.pipe, request_body, timeout_ms_);
#else
        return local_exchange(conn.fd, request_body, timeout_ms_);
#endif
    }

    // 按传输新建连接
    bool open_conn(PooledConn& conn, int timeout_ms_, std::string& err)
    {
        if (transport == RpcTransport::Tcp) {
            return tcp_open_conn(conn, err);
        }
#ifdef _WIN32
        return local_open_conn(conn.pipe, timeout_ms_, err);
#else
        return local_open_conn(conn.fd, timeout_ms_, err);
#endif
    }

    // ------------------ 请求流水线路径 ------------------
    //
    // 请求 JSON 顶层加 "id"；服务器（本实现的帧协议服务端）原样回带 id，
    // 读取线程按 id 分发到在途等待者。发送在 send_mutex 下串行，
    // 处理在服务器端并发，读取线程单点收帧——三段各自独立。

    // 本地传输收一帧（读取线程用；阻塞、带超时）
    bool pipe_recv_frame(PooledConn& conn, std::string& payload)
    {
#ifdef _WIN32
        char head[4];
        if (!pipe_ov_read(conn.pipe, head, 4)) {
            return false;
        }
        const std::uint32_t len = get_u32_le(head);
        if (len > kMaxFrameBytes) {
            return false;
        }
        payload.resize(len);
        if (len > 0 &&
            !pipe_ov_read(conn.pipe, &payload[0], len)) {
            return false;
        }
        return true;
#else
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        (void)::setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return uds_recv_frame(conn.fd, payload);
#endif
    }

    // 为池连接启动（或复用）流水线会话
    std::shared_ptr<PipeSession> ensure_session(PooledConn& conn)
    {
        std::lock_guard<std::mutex> create_lock(conn.session_mu);
        if (conn.session) {
            return conn.session;
        }
        std::shared_ptr<PipeSession> s(new PipeSession());
        if (transport == RpcTransport::Tcp) {
            // 裸指针生命周期说明：TcpClient 归 PooledConn 所有，会话的
            // 读回调仅在其上注册；连接析构时先 request_close/join reader
            // 再关 TcpClient（见 PooledConn::~PooledConn），回调竞态窗口安全
            TcpClient* tcp_raw = conn.tcp.get();
            s->send_frame = [s, tcp_raw](const std::string& payload) {
                return tcp_raw->send(payload);
            };
            s->request_close = [tcp_raw]() { tcp_raw->close(); };

            // 收帧交给 TcpClient 的回调线程：帧到达即投递（on_message
            // 收到的已是裸载荷——tcp 模块内部已按帧结构拆好，与本地
            // 读取线程看到的形态一致，无需再拆）
            const std::weak_ptr<PipeSession> ws = s;
            conn.tcp->set_on_message([ws](const std::string& payload) {
                const std::shared_ptr<PipeSession> sess = ws.lock();
                if (!sess) {
                    return;
                }
                std::uint64_t id = 0;
                const bool has_id = extract_frame_id(payload, id);
                sess->deliver(payload, has_id, id);
            });
            conn.tcp->set_on_disconnect([ws](const std::string& reason) {
                const std::shared_ptr<PipeSession> sess = ws.lock();
                if (sess) {
                    sess->mark_broken(
                        reason.empty() ? "connection closed" : reason);
                }
            });
        } else {
            // 本地传输：独立读取线程阻塞收帧；组帧 = u32 长度头 + 载荷
            PooledConn* conn_ptr = &conn;
            s->send_frame = [conn_ptr, s](const std::string& payload) {
                std::lock_guard<std::mutex> send_lock(s->send_mutex);
#ifdef _WIN32
                // 组帧 = u32 长度头 + 载荷（与服务端读循环一致）；
                // 重叠句柄写入（读线程并发收帧，见 pipe_ov_write）
                std::string frame;
                put_u32_le(frame, static_cast<std::uint32_t>(payload.size()));
                frame += payload;
                return pipe_ov_write(conn_ptr->pipe, frame.data(),
                                     static_cast<std::uint32_t>(frame.size()));
#else
                // uds_send_frame 自带 u32 长度头——直接传裸载荷，
                // 勿在此预组帧（双重封装会让服务端 JSON 解析失败）
                return uds_send_frame(conn_ptr->fd, payload);
#endif
            };
            s->request_close = [conn_ptr]() {
#ifdef _WIN32
                ::CancelIoEx(conn_ptr->pipe, NULL);
                ::DisconnectNamedPipe(conn_ptr->pipe);
#else
                ::shutdown(conn_ptr->fd, SHUT_RDWR);
#endif
            };
            s->reader = std::thread([this, s, conn_ptr]() {
                std::string payload;
                for (;;) {
                    if (!this->pipe_recv_frame(*conn_ptr, payload)) {
                        s->mark_broken("connection closed");
                        return;
                    }
                    std::uint64_t id = 0;
                    const bool has_id = extract_frame_id(payload, id);
                    s->deliver(payload, has_id, id);
                }
            });
            s->reader_started = true;
        }
        conn.session = s;
        return s;
    }

    // 流水线一次尝试：注册在途等待者 → 发帧 → 等待响应（无 id 旧服务器
    // 的响应当作自己那一份：此时每连接只有一个在途请求，不会串扰）
    AttemptResult attempt_pipelined(PooledConn& conn,
                                    const std::string& /*request_body*/,
                                    std::uint64_t id,
                                    const std::string& request_with_id,
                                    int timeout_ms_)
    {
        AttemptResult r;
        const std::shared_ptr<PipeSession> s = ensure_session(conn);
        std::shared_ptr<PendingCall> call(new PendingCall());

        {
            std::lock_guard<std::mutex> lock(s->m);
            if (s->broken) {
                r.error = RpcError::CONNECTION_FAILED;
                r.error_detail = "connection closed: " + s->broken_reason;
                return r;
            }
            s->inflight[id] = call;
            // 首个请求前放置无 id 退路接收者：旧服务器（不回带 id）的
            // 响应落到这里。流水线在旧服务器上不能并发（响应无法区分），
            // 读者见 has_id=false 且无 anon_waiter 时丢弃——由「每连接
            // 只有一个无 id 等待者」的不变式保证正确性见下
            s->anon_waiter = call;
        }

        // 发送（组帧由传输相关 send_frame 负责：本地 = u32 长度头，
        // Tcp = TcpClient 帧协议。请求体裸传，勿在此预加帧头）
        if (!s->send_frame(request_with_id)) {
            {
                std::lock_guard<std::mutex> lock(s->m);
                s->inflight.erase(id);
                if (s->anon_waiter == call) {
                    s->anon_waiter.reset();
                }
            }
            r.error = RpcError::CONNECTION_FAILED;
            r.error_detail = "connection failed: send error";
            return r;
        }

        // 等待响应
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(call->m);
            const bool have = call->cv.wait_for(
                lock, std::chrono::milliseconds(timeout_ms_),
                [&] { return call->done; });
            if (!have) {
                // 超时：从在途表摘除自己（读取线程可能稍后送达→无主丢弃）
                {
                    std::lock_guard<std::mutex> lock2(s->m);
                    auto it = s->inflight.find(id);
                    if (it != s->inflight.end() && it->second == call) {
                        s->inflight.erase(it);
                    }
                    if (s->anon_waiter == call) {
                        s->anon_waiter.reset();
                    }
                }
                r.error = RpcError::TIMEOUT;
                r.error_detail = "timeout: read response";
                return r;
            }
            if (call->broken && call->payload.empty()) {
                r.error = RpcError::CONNECTION_FAILED;
                r.error_detail = "connection closed: " + s->broken_reason;
                return r;
            }
            payload = call->payload;
        }
        r.transport_ok = true;
        r.body = payload;
        parse_local_status(r);
        return r;
    }

    // 流水线通道路径：确保通道连接存活 → 取在途槽位 → attempt_pipelined。
    // 连接断开时在持锁状态下重建（握手失败向上抛给重试层）
    AttemptResult attempt_pipelined_channel(const std::string& request_body,
                                            int timeout_ms_)
    {
        if (!pipeline) {
            // 指针惰性创建必须串行：并发首用时两个线程同时 reset
            // 会销毁对方正在使用的通道（UAF）并泄漏一个通道
            std::lock_guard<std::mutex> init_lock(pipeline_mutex);
            if (!pipeline) {
                pipeline.reset(new PipelineChannel());
            }
        }
        PipelineChannel& ch = *pipeline;

        // 惰性建立/断线重建专用连接（setup_mutex 串行化握手）
        {
            std::unique_lock<std::mutex> setup_lock(ch.setup_mutex);
            bool need_rebuild = false;
            {
                std::lock_guard<std::mutex> lock(ch.m);
                need_rebuild = ch.broken || !ch.conn;
            }
            if (need_rebuild) {
                if (ch.conn) {
                    // 断线重建：释放自己的引用（仍在使用的调用持有
                    // 副本，旧连接在最后一批使用者离开后自然析构）
                    ch.conn.reset();
                    ch.broken_total.fetch_add(1, std::memory_order_relaxed);
                }
                std::shared_ptr<PooledConn> fresh(new PooledConn());
                std::string err;
                if (!open_conn(*fresh, timeout_ms_, err)) {
                    {
                        std::lock_guard<std::mutex> lock(ch.m);
                        ch.broken = true;
                    }
                    ch.cv.notify_all();
                    AttemptResult r;
                    r.error = RpcError::CONNECTION_FAILED;
                    r.error_detail = err;
                    return r;
                }
                ch.conn = std::move(fresh);
                {
                    std::lock_guard<std::mutex> lock(ch.m);
                    ch.broken = false;
                }
            }
        }

        // 在途槽位限流：拿不到（等待超时）报 pool busy 语义的超时，
        // 与连接池排队超时对调用方的表现一致
        bool slot_waited = false;
        if (!ch.acquire_slot(pipeline_max_in_flight, timeout_ms_,
                             &slot_waited)) {
            AttemptResult r;
            r.error = RpcError::TIMEOUT;
            r.error_detail = "timeout: pipeline in-flight limit (" +
                             std::to_string(pipeline_max_in_flight) +
                             ") reached";
            return r;
        }

        // RAII 槽位归还（attempt_pipelined 的所有返回路径都会经过这里）
        // 注意：不能用 NSDMI + 聚合初始化（C++14 语义，GCC -std=c++11 拒绝），
        // 显式构造函数保 C++11 兼容
        struct SlotGuard
        {
            PipelineChannel& ch;
            bool released;
            explicit SlotGuard(PipelineChannel& channel)
                : ch(channel), released(false)
            {
            }
            ~SlotGuard()
            {
                if (!released) {
                    ch.release_slot();
                }
            }
        } guard{ch};

        // 请求已被通道接纳：计入单连接承载量（复用口径）
        ch.reused_total.fetch_add(1, std::memory_order_relaxed);

        PooledConn* conn = nullptr;
        std::shared_ptr<PooledConn> conn_keep;
        {
            std::lock_guard<std::mutex> setup_lock(ch.setup_mutex);
            conn_keep = ch.conn;
            conn = conn_keep.get();
        }
        if (!conn) {
            // 并发重建竞态：连接刚被重建过，走下次调用
            AttemptResult r;
            r.error = RpcError::CONNECTION_FAILED;
            r.error_detail = "connection rebuilt, retry";
            return r;
        }

        const std::uint64_t id = next_pipeline_id.fetch_add(1);
        JsonValue req_with_id;
        try {
            req_with_id = JsonValue::parse(request_body);
            req_with_id["id"] = static_cast<double>(id);
        } catch (const std::exception&) {
            AttemptResult r;
            r.error = RpcError::PROTOCOL_ERROR;
            r.error_detail = "invalid request json";
            return r;
        }
        const std::string body = to_json_string(req_with_id);

        AttemptResult r = attempt_pipelined(*conn, request_body,
                                            id, body, timeout_ms_);
        if (r.transport_ok || r.error == RpcError::PROTOCOL_ERROR ||
            r.error == RpcError::TIMEOUT) {
            // 传输健康或帧已完整发出（超时是等待层面）：连接保留复用。
            // TIMEOUT 不能标记 broken——超时后响应可能稍后到达（无主丢弃）
            return r;
        }
        // 连接级失败：标记断线，下次调用重建；本次错误交给重试层
        ch.mark_broken();
        return r;
    }

    // ------------------ 池化请求路径 ------------------
    // 取连接（复用或新建）→ 交换 → 归还。复用连接失败时丢弃并换新连接
    // 立即重试一次（免退避），对调用方屏蔽「池中连接已被服务器关闭」的竞态。
    // 流水线开启时请求体注入自增 id 并走 attempt_pipelined（快路径：
    // 复用连接直接挂到会话，响应按 id 匹配）
    AttemptResult attempt_pooled(const std::string& request_body)
    {
        const int tmo = timeout_ms;
        for (int stale_retry = 0; stale_retry < 2; ++stale_retry) {
            std::unique_ptr<PooledConn> conn;
            const ConnPool::Lease lease = pool->acquire(
                conn, pool_max_conns, pool_idle_timeout_ms, tmo);
            if (lease == ConnPool::Lease::Timeout) {
                AttemptResult r;
                r.error = RpcError::TIMEOUT;
                r.error_detail =
                    "timeout: connection pool busy (all " +
                    std::to_string(pool_max_conns) +
                    " connections in use)";
                return r;
            }
            const bool was_reused = (lease == ConnPool::Lease::Idle);
            if (lease == ConnPool::Lease::Create) {
                conn.reset(new PooledConn());
                std::string err;
                if (!open_conn(*conn, tmo, err)) {
                    pool->cancel_create();
                    AttemptResult r;
                    r.error = RpcError::CONNECTION_FAILED;
                    r.error_detail = err;
                    return r;
                }
            }

            // （流水线路径由 call() 分发，attempt_pooled 只承担串行请求）

            AttemptResult r = exchange_on(*conn, request_body, tmo);
            if (r.transport_ok || r.error == RpcError::PROTOCOL_ERROR) {
                // PROTOCOL_ERROR 意味着帧完整接收，连接本身健康，照常归还
                pool->release(std::move(conn), pool_max_idle,
                              pool_idle_timeout_ms);
                return r;
            }
            pool->discard(std::move(conn));
            if (!was_reused) {
                return r;  // 新建连接也失败：交由 call() 的重试/退避处理
            }
            // 复用连接失效：下一轮循环换新建连接重试
        }
        AttemptResult r;
        r.error = RpcError::UNKNOWN;
        r.error_detail = "unreachable";
        return r;
    }

    // 池禁用路径：一调用一连接，用完即断（旧行为）
    AttemptResult attempt_one_shot(const std::string& request_body)
    {
        PooledConn conn;
        std::string err;
        if (!open_conn(conn, timeout_ms, err)) {
            AttemptResult r;
            r.error = RpcError::CONNECTION_FAILED;
            r.error_detail = err;
            return r;
        }
        return exchange_on(conn, request_body, timeout_ms);
    }

    // ------------------ HTTP 单次尝试 ------------------
    AttemptResult attempt_http(const std::string& request_body)
    {
        AttemptResult r;
        if (!http) {
            // 端点缺失（如 Http + 空端点构造）时报错而非崩溃
            r.error = RpcError::CONNECTION_FAILED;
            r.error_detail = "connection failed: http endpoint not configured";
            return r;
        }
        const httplib::Result result =
            http->Post("/rpc", request_body, "application/json");
        if (!result) {
            const httplib::Error http_err = result.error();
            RpcError err = map_error(http_err);
            if (err == RpcError::TIMEOUT &&
                http_err == httplib::Error::ConnectionTimeout &&
                is_local_address(host)) {
                err = RpcError::CONNECTION_FAILED;
            }
            r.error = err;
            r.error_detail =
                error_text(err) + ": " + httplib::to_string(http_err);
            return r;
        }
        r.transport_ok = true;
        r.http_status = result->status;
        r.body = result->body;
        if (result->has_header("Retry-After")) {
            const long long ms = parse_retry_after_ms(
                result->get_header_value("Retry-After"));
            if (ms > 0) {
                r.retry_after_ms = static_cast<int>(
                    std::min<long long>(ms, 24LL * 3600 * 1000));
            }
        }
        return r;
    }

    // 第 retry_index 次重试（从 1 开始）前应等待的毫秒数：
    // base * 2^(n-1)，封顶 retry_max_delay_ms
    int backoff_ms(int retry_index) const
    {
        long long delay = retry_base_delay_ms;
        for (int i = 1; i < retry_index; ++i) {
            delay *= 2;
            if (delay >= retry_max_delay_ms) {
                return retry_max_delay_ms;
            }
        }
        return static_cast<int>(delay);
    }

    // 相等抖动：半固定 + 半随机，期望值仍是退避间隔
    // actual = delay/2 + uniform_random[0, delay/2)
    int apply_jitter(int delay_ms)
    {
        if (!jitter_enabled || delay_ms <= 1) {
            return delay_ms;
        }
        const int half = delay_ms / 2;
        std::lock_guard<std::mutex> lock(rng_mutex);
        std::uniform_int_distribution<int> dist(0, half);
        return half + dist(rng);
    }

    // 重试日志："rpc retry method=\"...\" attempt=2/3 wait=118ms reason=\"...\""
    void log_retry(const std::string& method, int attempt, int wait_ms,
                   const std::string& reason)
    {
        if (logger) {
            logger->info("rpc retry method=\"{}\" attempt={}/{} wait={}ms "
                         "reason=\"{}\"",
                         method, attempt, max_retries, wait_ms, reason);
        }
    }
};

RpcClient::RpcClient(const std::string& host, int port)
    : impl_(new Impl(host, port)) {}

// 单参构造：端点自动识别（含 "\\\\.\\pipe\\" 前缀或 '/' 开头 = 本地传输）。
// Windows 上不含前缀的裸名字也按本地管道名处理（自动补全前缀）——
// 这与双参 host/port 构造不产生二义性（后者需要第二个 int 参数）
RpcClient::RpcClient(const std::string& pipe_or_socket_path)
    : impl_(new Impl(RpcTransport::LocalPipe, pipe_or_socket_path)) {}

RpcClient::RpcClient(RpcTransport transport, const std::string& endpoint)
    : impl_(new Impl(transport, endpoint)) {}

RpcClient::~RpcClient()
{
    delete impl_;
}

RpcClient::RpcClient(RpcClient&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

RpcClient& RpcClient::operator=(RpcClient&& other) noexcept
{
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void RpcClient::set_timeout_ms(int timeout_ms)
{
    impl_->timeout_ms = timeout_ms;
    impl_->apply_timeout();
}

void RpcClient::set_max_retries(int retries)
{
    impl_->max_retries = retries;
}

void RpcClient::set_retry_base_delay_ms(int ms)
{
    impl_->retry_base_delay_ms = ms;
}

void RpcClient::set_retry_max_delay_ms(int ms)
{
    impl_->retry_max_delay_ms = ms;
}

void RpcClient::set_retry_max_total_wait_ms(int ms)
{
    impl_->retry_max_total_wait_ms = ms;
}

void RpcClient::set_retry_jitter(bool enable)
{
    impl_->jitter_enabled = enable;
}

void RpcClient::set_logger(spdlog::logger* logger)
{
    impl_->logger = logger;
}

void RpcClient::set_connection_pool_max(std::size_t max_connections)
{
    impl_->pool_max_conns = max_connections;
    if (max_connections == 0 && impl_->pool) {
        // 禁用时立即释放空闲连接；使用中的连接在归还时按超容量关闭
        impl_->pool->close_all_idle();
    }
}

void RpcClient::set_connection_pool_idle_ms(int ms)
{
    impl_->pool_idle_timeout_ms = ms;
}

void RpcClient::set_pipeline_max_in_flight(std::size_t max_in_flight)
{
    impl_->pipeline_max_in_flight = max_in_flight;
}

RpcClientPoolStats RpcClient::pool_stats() const
{
    if (!impl_ || !impl_->pool) {
        return RpcClientPoolStats();
    }
    RpcClientPoolStats s = impl_->pool->stats();

    // 流水线通道统计（未开启流水线时各字段保持 0）
    if (impl_) {
        std::lock_guard<std::mutex> init_lock(impl_->pipeline_mutex);
        if (impl_->pipeline) {
            const Impl::PipelineChannel& ch = *impl_->pipeline;
            s.pipeline_reused_total =
                ch.reused_total.load(std::memory_order_relaxed);
            s.pipeline_broken_total =
                ch.broken_total.load(std::memory_order_relaxed);
            s.pipeline_slot_wait_total =
                ch.slot_wait_total.load(std::memory_order_relaxed);
            s.pipeline_slot_timeout_total =
                ch.slot_timeout_total.load(std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(ch.m);
            s.pipeline_in_flight = ch.busy;
            s.pipeline_in_flight_peak = ch.in_flight_peak;
        }
    }
    return s;
}

RpcError RpcClient::last_error() const
{
    return impl_->last_error;
}

std::string RpcClient::last_error_message() const
{
    return impl_->last_message;
}

double RpcClient::last_call_elapsed_ms() const
{
    return impl_ ? impl_->last_call_ms : -1.0;
}

RpcTransport RpcClient::transport() const
{
    return impl_ ? impl_->transport : RpcTransport::Http;
}

std::string RpcClient::endpoint() const
{
    return impl_ ? impl_->describe_endpoint() : std::string();
}

std::string RpcClient::call(const std::string& method, const std::string& params)
{
    Impl* impl = impl_;
    if (!impl) {
        return std::string();
    }

    const auto call_start = std::chrono::steady_clock::now();
    struct CallTimer
    {
        Impl* impl;
        std::chrono::steady_clock::time_point start;
        ~CallTimer()
        {
            impl->last_call_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
        }
    } call_timer{impl, call_start};

    // 失败时记录原因并返回空字符串
    auto fail = [impl](RpcError err, const std::string& msg) {
        impl->last_error = err;
        impl->last_message = msg;
        return std::string();
    };

    // 请求体只构建一次，重试时复用
    JsonValue body;
    try {
        body["method"] = method;
        body["params"] = JsonValue::parse(params);
    } catch (const std::exception&) {
        return fail(RpcError::PROTOCOL_ERROR, "params is not valid JSON");
    }
    const std::string request_body = to_json_string(body);

    // 可重试的失败：过载（429）、连接失败、连接超时；重试间隔见 backoff_ms
    const auto start = std::chrono::steady_clock::now();
    auto total_waited_ms = [&start]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
            .count();
    };

    RpcError last_fail_error = RpcError::UNKNOWN;
    std::string last_fail_msg;

    for (int attempt = 0; attempt <= impl->max_retries; ++attempt) {
        if (attempt > 0) {
            // 第 attempt 次重试前等待：优先 Retry-After，否则指数退避
            //（可叠加相等抖动），且保证总等待不超过 retry_max_total_wait_ms
            long long wait = impl->backoff_ms(attempt);
            if (last_fail_error == RpcError::OVERLOADED &&
                impl->retry_after_ms > 0) {
                wait = impl->retry_after_ms;  // 服务器指定值不随机化
            } else {
                wait = impl->apply_jitter(static_cast<int>(wait));
            }
            if (total_waited_ms() + wait > impl->retry_max_total_wait_ms) {
                break;  // 预算用尽，放弃重试
            }
            impl->log_retry(method, attempt, static_cast<int>(wait),
                            last_fail_msg);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        }

        // 按传输与模式分发：HTTP 走 httplib（内置 keep-alive）；本地/Tcp
        // 开流水线时走专用通道（单连接多在途）；否则走连接池（池禁用时
        // 一调用一连接）。所有路径都归一为 AttemptResult（http_status
        // 语义对齐），重试/退避/错误处理对传输无感知
        const AttemptResult r =
            (impl->transport == RpcTransport::Http)
                ? impl->attempt_http(request_body)
                : (impl->pipeline_max_in_flight > 0)
                      ? impl->attempt_pipelined_channel(request_body,
                                                        impl->timeout_ms)
                      : (impl->pool_max_conns > 0)
                            ? impl->attempt_pooled(request_body)
                            : impl->attempt_one_shot(request_body);

        if (!r.transport_ok) {
            last_fail_error = r.error;
            last_fail_msg = r.error_detail;
            continue;  // 连接/超时类错误可重试
        }

        // 429 过载：读取 Retry-After（HTTP 头秒数/HTTP-date，或本地
        // 响应的 retry_after_s 字段）与响应体里的自定义错误文本后重试
        if (r.http_status == 429) {
            impl->retry_after_ms = r.retry_after_ms;
            last_fail_error = RpcError::OVERLOADED;
            last_fail_msg = "server overloaded";
            try {
                const JsonValue error_body = JsonValue::parse(r.body);
                if (error_body.contains("error") &&
                    !error_body["error"].is_null()) {
                    // 字符串直接取值（避免日志中双重引号），其他类型用 dump
                    last_fail_msg = error_body["error"].is_string()
                                        ? error_body["error"].get<std::string>()
                                        : error_body["error"].dump();
                }
            } catch (const std::exception&) {
                // 响应体不是 JSON 时使用默认描述
            }
            continue;
        }

        // 服务器错误（除 429 外的 4xx/5xx）不重试，
        // 但优先使用响应体里的 error 字段（比状态码更具体）
        if (r.http_status / 100 != 2) {
            try {
                const JsonValue error_body = JsonValue::parse(r.body);
                if (error_body.contains("error") &&
                    !error_body["error"].is_null()) {
                    return fail(RpcError::SERVER_ERROR,
                                error_body["error"].is_string()
                                    ? error_body["error"].get<std::string>()
                                    : error_body["error"].dump());
                }
            } catch (const std::exception&) {
                // 响应体不是 JSON 时退回状态码描述
            }
            return fail(RpcError::SERVER_ERROR,
                        "http status " + std::to_string(r.http_status));
        }

        JsonValue response;
        try {
            response = JsonValue::parse(r.body);
        } catch (const std::exception&) {
            return fail(RpcError::PROTOCOL_ERROR, "response is not valid JSON");
        }

        if (response.contains("error") && !response["error"].is_null()) {
            return fail(RpcError::SERVER_ERROR, response["error"].dump());
        }

        impl->last_error = RpcError::OK;
        impl->last_message.clear();

        if (response.contains("result") && !response["result"].is_null()) {
            return response["result"].dump();
        }
        return std::string();
    }

    // 重试次数或等待预算用尽，返回最后一次的错误
    return fail(last_fail_error, last_fail_msg);
}

// ==================== RpcServer ====================

// 并发处理闸门：限制同时在 handle() 中执行的请求数。
// RejectImmediate 模式：acquire() 立即返回 false，由调用方返回 429；
// WaitInQueue 模式：acquire_for() 在条件变量上排队等待槽位，
// 超时未获得才返回 false（调用方同样返回 429）。
class AdmissionGate
{
public:
    ~AdmissionGate()
    {
        // 兑现“析构时唤醒”的承诺：正常流程先 shutdown() 再析构，
        // 这里拦截极少数仍在等待的调用方，避免析构后悬挂
        shutdown();
    }

    // RejectImmediate 模式：无阻塞获取槽位
    bool acquire()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (limit > 0 && in_flight >= limit) {
            return false;
        }
        ++in_flight;
        return true;
    }

    // WaitInQueue 模式：公平排队等待槽位（条件变量唤醒顺序即 FIFO）。
    // 返回 false 表示等待 queue_wait_ms 后仍未获得槽位。
    bool acquire_for(int queue_wait_ms, std::size_t& waited_ms)
    {
        waited_ms = 0;
        std::unique_lock<std::mutex> lock(mutex);
        if (limit > 0 && in_flight >= limit) {
            ++waiting;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(queue_wait_ms);
            const auto t0 = std::chrono::steady_clock::now();
            // 丢失唤醒防护：谓词不满足时的 spurious wakeup 会重新进入等待
            const bool got = slot_freed_cv.wait_until(lock, deadline, [this] {
                return limit == 0 || in_flight < limit || stop_waiting;
            });
            --waiting;
            waited_ms = elapsed_since(t0);
            if (!got || stop_waiting) {
                return false;
            }
        }
        ++in_flight;
        return true;
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        --in_flight;
        slot_freed_cv.notify_all();
    }

    void set_limit(std::size_t max_requests)
    {
        std::lock_guard<std::mutex> lock(mutex);
        limit = max_requests;
        slot_freed_cv.notify_all();
    }

    std::size_t current() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return in_flight;
    }

    // 服务器停止时唤醒所有仍在排队的请求（由 handle 统一返回 429）
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop_waiting = true;
        slot_freed_cv.notify_all();
    }

    // 重新启动时恢复可等待状态
    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop_waiting = false;
    }

    // 优雅停机排空：等待所有处理中的请求完成、排队中的请求获得槽位。
    // 返回 false 表示窗口用尽时仍有未消化者
    bool wait_until_idle(int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        return slot_freed_cv.wait_until(lock, deadline, [this] {
            return in_flight == 0 && waiting == 0;
        });
    }

private:
    static std::size_t elapsed_since(
        const std::chrono::steady_clock::time_point& t0)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        return ms > 0 ? static_cast<std::size_t>(ms) : 0;
    }

    mutable std::mutex mutex;
    std::size_t in_flight = 0;
    std::size_t waiting = 0;  // 正在 acquire_for() 中排队等槽位的请求数
    std::size_t limit = 0;    // 0 = 不限制
    bool stop_waiting = false;
    std::condition_variable slot_freed_cv;
};

#ifdef _WIN32

// 把核心处理结果转成本地传输的响应 JSON（status/retry_after_s 字段）。
// 接受基础字段而非 CoreReply，避免与 RpcServer::Impl 的定义顺序耦合
std::string core_reply_to_local(int http_status, int retry_after_seconds,
                                const std::string& body_json,
                                bool has_id = false, std::uint64_t id = 0)
{
    JsonValue body;
    try {
        body = JsonValue::parse(body_json);
    } catch (const std::exception&) {
        body = JsonValue{{"result", nullptr}, {"error", "internal"}};
    }
    body["status"] = http_semantic_to_status(http_status);
    if (retry_after_seconds > 0) {
        body["retry_after_s"] = retry_after_seconds;
    }
    if (has_id) {
        body["id"] = static_cast<double>(id);  // 流水线序号原样回带
    }
    return to_json_string(body);
}

#else

// ============ POSIX Unix 域套接字工具 ============

// Windows 版同名函数的 POSIX 侧（见上：含流水线 id 回带）
std::string core_reply_to_local(int http_status, int retry_after_seconds,
                                const std::string& body_json,
                                bool has_id = false, std::uint64_t id = 0)
{
    JsonValue body;
    try {
        body = JsonValue::parse(body_json);
    } catch (const std::exception&) {
        body = JsonValue{{"result", nullptr}, {"error", "internal"}};
    }
    body["status"] = http_semantic_to_status(http_status);
    if (retry_after_seconds > 0) {
        body["retry_after_s"] = retry_after_seconds;
    }
    if (has_id) {
        body["id"] = static_cast<double>(id);
    }
    return to_json_string(body);
}

// 发送完整缓冲区；返回是否成功
bool uds_send_all(int fd, const char* data, std::size_t len)
{
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// 接收完整缓冲区
bool uds_recv_all(int fd, char* data, std::size_t len)
{
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::recv(fd, data + got, len - got, 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

// 发送一帧（长度前缀 + JSON）
bool uds_send_frame(int fd, const std::string& payload)
{
    char head[4];
    put_u32_le(head, static_cast<std::uint32_t>(payload.size()));
    return uds_send_all(fd, head, 4) &&
           (payload.empty() ||
            uds_send_all(fd, payload.data(), payload.size()));
}

// 接收一帧；对端关闭返回 false

// 接收一帧；对端关闭返回 false
bool uds_recv_frame(int fd, std::string& payload)
{
    char head[4];
    if (!uds_recv_all(fd, head, 4)) {
        return false;
    }
    const std::uint32_t len = get_u32_le(head);
    if (len > kMaxFrameBytes) {
        return false;
    }
    payload.resize(len);
    if (len > 0 && !uds_recv_all(fd, &payload[0], len)) {
        return false;
    }
    return true;
}

// 带超时的连接（UDS connect 几乎立即完成，但防止异常路径阻塞）
bool uds_connect(int fd, const std::string& path, int timeout_ms)
{
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // 非阻塞 connect + poll
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) {
            return false;
        }
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, timeout_ms) != 1) {
            return false;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
            err != 0) {
            return false;
        }
    }
    ::fcntl(fd, F_SETFL, flags);  // 恢复阻塞模式
#ifdef SO_NOSIGPIPE
    // macOS：无 MSG_NOSIGNAL，用套接字选项抑制 SIGPIPE（写端对端已关时）
    const int nosig = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
    return true;
}

#endif  // _WIN32

struct RpcServer::Impl
{
    const int port;  // HTTP/Tcp 端口（0 = 自动分配）；本地传输为 -1
    int bound_port = -1;
    const RpcTransport transport;   // Http / LocalPipe / Tcp
    const std::string endpoint;     // 本地传输端点（已规范化）；Tcp 原样保存
    std::string tcp_host = "0.0.0.0";  // Tcp 监听地址（可运行前调整）
    // Tcp 传输壳。用 shared_ptr：流水线派发线程持有副本，停机时
    // TcpServer 在最后一批派发线程结束后才析构（否则派发线程回包时
    // 解引用已销毁的 send() 实现是 use-after-free）
    std::shared_ptr<TcpServer> tcp_server;
    mutable std::mutex tcp_mutex;           // 保护 bound_tcp_port
    int bound_tcp_port = 0;                 // Tcp 实际监听端口

    std::map<std::string, RpcServer::Handler> handlers;
    std::mutex handlers_mutex;

    // 请求日志器（可为空 = 不记录）；指针由外部持有，热更新无需加锁
    spdlog::logger* logger = nullptr;

    // ------------------ 过载保护与监控 ------------------
    AdmissionGate gate;                    // 并发处理上限
    RpcOverloadMode overload_mode = RpcOverloadMode::RejectImmediate;
    int queue_wait_ms = 10000;             // WaitInQueue 模式下排队等待上限
    int drain_timeout_ms = 3000;           // 优雅停机排空窗口；0 = 立即放弃
    int retry_after_seconds = 1;           // 429 响应的 Retry-After 头；0 = 不发送
    std::string overload_message = "server overloaded";
    std::size_t queue_warn_threshold = 0;  // 0 = 不告警
    std::size_t worker_threads = 0;        // 0 = httplib 默认线程数
    std::size_t max_queued_requests = 0;   // 队列硬上限（内存保护最后防线）

    // 本地传输连接线程簿记：done 标志让「中途回收」只 join 已结束的线程，
    // 不会卡在仍在阻塞读的连接线程上（连接池的常驻连接会长期处于读等待）
    struct LocalWorker
    {
        std::thread th;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<LocalWorker> worker_threads_runtime;
    std::mutex local_worker_mutex;  // 保护 worker_threads_runtime 的回收/追加

    // 收割已结束的派发线程（须持 local_worker_mutex；本地壳与 Tcp 壳共用）
    void reap_finished_locked()
    {
        if (worker_threads_runtime.empty()) {
            return;
        }
        std::vector<LocalWorker> keep;
        keep.reserve(worker_threads_runtime.size());
        for (auto& w : worker_threads_runtime) {
            if (w.done->load(std::memory_order_relaxed)) {
                if (w.th.joinable()) {
                    w.th.join();
                }
            } else {
                keep.push_back(std::move(w));
            }
        }
        worker_threads_runtime.swap(keep);
    }

    // 等待全部派发线程结束（停机时流水线在途请求的收尾）
    bool drain_dispatch_workers(int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(local_worker_mutex);
                reap_finished_locked();
                if (worker_threads_runtime.empty()) {
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

#ifdef _WIN32
    // 活跃服务管道注册表：停机时对每个仍在阻塞读的句柄 CancelIoEx，
    // 否则池化的常驻连接会让服务线程永远退出不了
    std::mutex active_pipes_mutex;
    std::set<HANDLE> active_pipes;
#else
    // 活跃 UDS 连接注册表：停机时 shutdown 所有连接解除 recv 阻塞
    std::mutex active_uds_mutex;
    std::set<int> active_uds;
#endif

    // 可监控计数（全部由 stats_mutex 保护）
    mutable std::mutex stats_mutex;
    std::size_t queued = 0;        // 队列中等待的请求数
    std::size_t max_queued = 0;    // 队列积压峰值
    std::size_t max_active = 0;    // 并发处理峰值
    std::size_t rejected_total = 0;
    std::size_t completed_total = 0;  // 已完成处理的请求数（含业务错误/异常）
    // 按传输分列的口径（handle_core 按传输参数记账；本地管道/UDS 合并
    // 在 local；请求级格式错误计入 malformed）。与合计同步累加
    std::size_t completed_http = 0;
    std::size_t rejected_http = 0;
    std::size_t completed_tcp = 0;
    std::size_t rejected_tcp = 0;
    std::size_t completed_local = 0;
    std::size_t rejected_local = 0;
    std::size_t completed_malformed = 0;
    bool queue_warned = false;     // 积压告警已发标志（队列清空后重新武装）
    LatencyHistogram latency;      // 处理耗时分布（不含 429 拒绝）
    // 分传输直方图：record 时按通道写入对应图；All 即 latency。
    // 三张图各自 O(96 桶) 内存恒定，与合计直方图开销同级
    LatencyHistogram latency_http;
    LatencyHistogram latency_tcp;
    LatencyHistogram latency_local;

    httplib::Server http;
    std::thread worker;        // HTTP：accept 线程；本地传输：服务循环
    std::thread accept_thread; // 本地传输：专门的 accept 线程

    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    bool started = false;     // 是否已调用过 run()/start_background()
    bool bind_done = false;   // bind 阶段已结束（成功或失败）
    bool bind_ok = false;
    std::atomic<bool> stopping{false};  // 停机中：新请求快速 429

    explicit Impl(int port_)
        : port(port_), transport(RpcTransport::Http), endpoint()
    {
        http.Post("/rpc", [this](const httplib::Request& req,
                                 httplib::Response& res) {
            handle(req, res);
        });
    }

    Impl(RpcTransport transport_, const std::string& endpoint_)
        : port(-1),
          transport(transport_),
          endpoint(transport_ == RpcTransport::LocalPipe
                       ? canonical_local_endpoint(endpoint_)
                       : endpoint_)
    {
        if (transport_ == RpcTransport::Tcp) {
            // 默认端点：端口 0 自动分配；parse_host_port 同时规范化 host
            parse_host_port(endpoint_.empty() ? "0" : endpoint_, tcp_host,
                            const_cast<int&>(port));
        }
    }

    // ------------------ 队列/统计回调（MonitoringQueue 调用）------------------
    void on_job_enqueued()
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        ++queued;
        if (queued > max_queued) {
            max_queued = queued;
        }
        // 积压告警：排队数首次达到阈值时发一条 warn（队列清空后重新武装）
        if (logger && queue_warn_threshold > 0 &&
            queued >= queue_warn_threshold && !queue_warned) {
            queue_warned = true;
            logger->warn("rpc queue backlog={}", queued);
        }
    }

    void on_job_dequeued()
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        if (queued > 0) {
            --queued;
        }
        if (queued == 0) {
            queue_warned = false;
        }
    }

    // 队列析构（Server stop 后）：只清瞬时值；峰值与累计计数保留到
    // 下一次启动（bind_and_listen）时才归零，供调用方在 stop 后读取
    void on_queue_destroyed()
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        queued = 0;
        queue_warned = false;
    }

    // ------------------ 统计采样 ------------------
    RpcServerStats stats() const
    {
        RpcServerStats s;
        s.active_requests = gate.current();
        s.max_active = 0;
        s.queued_requests = 0;
        s.max_queued = 0;
        s.rejected_total = 0;
        s.completed_total = 0;
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            s.queued_requests = queued;
            s.max_queued = max_queued;
            s.max_active = max_active;
            s.rejected_total = rejected_total;
            s.completed_total = completed_total;
            // 分列口径：单传输服务器只有自己那列非零
            s.http.completed = completed_http;
            s.http.rejected = rejected_http;
            s.tcp.completed = completed_tcp;
            s.tcp.rejected = rejected_tcp;
            s.local.completed = completed_local;
            s.local.rejected = rejected_local;
            s.malformed.completed = completed_malformed;
        }
        return s;
    }

    // ------------------ 延迟分位估算 ------------------
    // 先整体快照，再逐个取分位（分位估算内部自带同锁，无死锁风险）。
    // filter 选择合计图或分传输图；指定传输无样本时各分位为 -1
    RpcLatencyStats latency_stats(const std::vector<double>& extra,
                                  RpcTransportFilter filter) const
    {
        const LatencyHistogram& hist =
            filter == RpcTransportFilter::Http
                ? latency_http
                : filter == RpcTransportFilter::Tcp
                      ? latency_tcp
                      : filter == RpcTransportFilter::Local
                            ? latency_local
                            : latency;
        RpcLatencyStats out;
        hist.snapshot(out);
        out.p50_ms = hist.percentile(50.0);
        out.p90_ms = hist.percentile(90.0);
        out.p95_ms = hist.percentile(95.0);
        out.p99_ms = hist.percentile(99.0);
        out.percentiles.reserve(extra.size());
        for (std::size_t i = 0; i < extra.size(); ++i) {
            if (extra[i] >= 0.0 && extra[i] <= 100.0) {
                out.percentiles.push_back(hist.percentile(extra[i]));
            }
        }
        return out;
    }

    // ------------------ 启动 ------------------
    // 停止流程（优雅停机，两阶段）：
    //   1. 置停机标志：新请求立即得到 429 "server shutting down"
    //      （比 TCP 直接断连对客户端更友好，可据此重试其他实例）；
    //   2. 排空窗口：等待已接收的请求处理完毕（含排队等槽位者），
    //      窗口用尽后唤醒仍在排队者（统一 429）；
    //   3. 最后才 http.stop() 关闭监听——注意必须在所有 gate 等待者
    //      释放之后，否则 MonitoringQueue::shutdown 的 join 会被阻塞在
    //      acquire_for 上的工作线程拖满 queue_wait_ms。
    // 窗口为 0 时跳过等待，等价于立即放弃（旧行为）
    void shutdown()
    {
        std::unique_lock<std::mutex> lock(ready_mutex);
        ready_cv.wait_for(lock, std::chrono::seconds(10),
                          [this]() { return bind_done || !started; });
        lock.unlock();

        stopping = true;
        int drain = 3000;
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            drain = drain_timeout_ms;
        }
        if (drain > 0 && gate.wait_until_idle(drain)) {
            // 已接收的请求全部消化完毕
        } else {
            gate.shutdown();  // 唤醒仍在排队等槽位的请求（统一走 429）
        }
        if (transport == RpcTransport::LocalPipe) {
#ifdef _WIN32
            // 唤醒 accept 循环：SetEvent 让 WaitForMultipleObjects 返回，
            // 循环检查 stopping 后退出，并自行取消/清理挂起的管道实例
            if (local_stop_event) {
                ::SetEvent(local_stop_event);
            }
#endif
            if (worker.joinable()) {
                worker.join();
            }
#ifdef _WIN32
            if (local_stop_event) {
                ::CloseHandle(local_stop_event);
                local_stop_event = nullptr;
            }
#endif
            // 本地传输：在途流水线请求由连接线程的派发线程处理，
            // 停收连接后派发线程可能尚未完成——与 Tcp 一样排水，
            // 防止它们晚于统计/析构收尾（超时放弃不阻塞停机）
            drain_dispatch_workers(drain_timeout_ms);
        } else if (transport == RpcTransport::Tcp) {
            // Tcp：TcpServer::stop() 关闭 listener 与全部会话 socket，
            // 内部 join 全部线程后返回（顺序不能反）
            if (tcp_server) {
                tcp_server->stop();
            }
            if (worker.joinable()) {
                worker.join();
            }
            // 排水流水线派发线程：在途请求可能晚于连接关闭完成，
            // 给它们时间完成 handle_core（统计/gate 计数归零后再退出）；
            // 不持 stats_mutex——派发线程完成时要拿它记统计
            drain_dispatch_workers(drain_timeout_ms);
        } else {
            // HTTP：先停止监听（关闭 listen socket 使 WSAPoll 返回），
            // 再 join accept 线程——顺序反了 join 会永远等不到循环退出
            http.stop();
            if (worker.joinable()) {
                worker.join();
            }
        }
        // 兑现“析构时唤醒”承诺的最后防线（幂等）
        gate.shutdown();
    }

    // 重启前清零全部统计（含分列计数）
    void reset_stats_locked()
    {
        queued = 0;
        max_queued = 0;
        max_active = 0;
        rejected_total = 0;
        completed_total = 0;
        completed_http = 0;
        rejected_http = 0;
        completed_tcp = 0;
        rejected_tcp = 0;
        completed_local = 0;
        rejected_local = 0;
        completed_malformed = 0;
        queue_warned = false;
    }

    // 两段式启动：先绑定（拿到真实端口并发出就绪信号），再进入 accept 循环
    // 返回 false 表示绑定失败
    bool bind_and_listen()
    {
        stopping = false;
        if (transport == RpcTransport::LocalPipe) {
            // 本地传输：计数器重置与 gate/latency 复位逻辑同 HTTP（提取共用）
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                reset_stats_locked();
            }
            latency.reset();
            latency_http.reset();
            latency_tcp.reset();
            latency_local.reset();
            gate.reset();
            return bind_and_listen_local();
        }
        if (transport == RpcTransport::Tcp) {
            // Tcp 传输：计数器重置同其他传输；bind/start 细节在下方共用辅助
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                reset_stats_locked();
            }
            latency.reset();
            latency_http.reset();
            latency_tcp.reset();
            latency_local.reset();
            gate.reset();

            // 每条连接一个请求帧；handle_core 共用全部过载/统计逻辑
            const bool ok = start_tcp_listener();
            {
                std::lock_guard<std::mutex> lock(ready_mutex);
                bind_done = true;
                bind_ok = ok;
            }
            ready_cv.notify_all();
            if (!ok) {
                return false;
            }
            // 等待停止信号；监听与会话线程由 TcpServer 管理
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (stopping.load(std::memory_order_relaxed)) {
                    break;
                }
            }
            // shutdown() 已先 stop() 了 TcpServer；此处等待完全收尾
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            reset_stats_locked();
        }
        latency.reset();
        latency_http.reset();
        latency_tcp.reset();
        latency_local.reset();
        gate.reset();
        // 每次 listen 前设置队列工厂（httplib 在 listen 内构造 TaskQueue）
        http.new_task_queue = [this]() -> httplib::TaskQueue* {
            std::size_t threads = worker_threads;
            std::size_t queue_cap;
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                queue_cap = max_queued_requests;
                // WaitInQueue 模式下过载等待者由 AdmissionGate 管理
                // （超时后 429 回流），httplib 传输队列不设上限：
                // 这里若沿用 max_queued_requests，会把等待槽位的请求
                // 在 enqueue 阶段直接断连（无法回包），背压完全失效
                if (overload_mode == RpcOverloadMode::WaitInQueue) {
                    queue_cap = 0;
                }
            }
            if (threads == 0) {
                threads = default_worker_threads();
            }
            return new MonitoringQueue(this, threads, queue_cap);
        };

        int bound = -1;
        if (port == 0) {
            bound = http.bind_to_any_port("0.0.0.0");
        } else {
            bound = http.bind_to_port("0.0.0.0", port) ? port : -1;
        }

        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            bind_done = true;
            bind_ok = (bound >= 0);
            bound_port = bound;
        }
        ready_cv.notify_all();

        // 绑定失败直接返回；否则必须进入 accept 循环，
        // 停止只能通过 http.stop() 关闭监听 socket 来完成
        if (bound < 0) {
            return false;
        }
        return http.listen_after_bind();
    }

    // ------------------ 传输无关的请求核心 ------------------
    //
    // 输入：请求 JSON 文本与对端标识（日志用）；
    // 输出：CoreReply{http_status, retry_after_seconds, body_json}。
    // HTTP 与本地传输共用：过载闸门、背压、直方图、计数与日志全部在此，
    // 传输壳只负责收发与语义映射。
    struct CoreReply
    {
        int http_status = 200;       // 语义状态（本地传输映射为 status 字段）
        int retry_after_seconds = 0;  // >0 时由传输层决定如何透传
        std::string body_json;
        bool has_id = false;         // 请求带流水线 id（响应原样回带）
        std::uint64_t id = 0;
    };

    // 从请求 JSON 提取流水线 id（顶层 "id"）；无/非法返回 false
    static bool extract_request_id(const std::string& request_body,
                                   std::uint64_t& out_id)
    {
        try {
            const JsonValue req = JsonValue::parse(request_body);
            if (req.is_object() && req.contains("id") &&
                req["id"].is_number()) {
                const double v = req["id"].get<double>();
                if (v >= 0.0 && v <= 9007199254740992.0) {
                    out_id = static_cast<std::uint64_t>(v);
                    return true;
                }
            }
        } catch (const std::exception&) {
            // 请求体不是 JSON：后续处理会走 400，这里按无 id 继续
        }
        return false;
    }

    // 分列记账目标：-handle_core 按传输路由计数（本地管道/UDS 同属 local）
    enum class Channel { Http, Tcp, Local };

    // 过载拒绝分列记账（须持 stats_mutex）
    void record_rejected(Channel channel)
    {
        switch (channel) {
            case Channel::Http:  ++rejected_http;  break;
            case Channel::Tcp:   ++rejected_tcp;   break;
            case Channel::Local: ++rejected_local; break;
        }
    }

    // 完成计数的分列引用（须持 stats_mutex 时解引用）
    std::size_t& completed_for(Channel channel)
    {
        switch (channel) {
            case Channel::Tcp:   return completed_tcp;
            case Channel::Local: return completed_local;
            default:             return completed_http;
        }
    }

    // 所属传输的分列直方图（直方图自带锁，无并发约束）
    LatencyHistogram& latency_for(Channel channel)
    {
        switch (channel) {
            case Channel::Tcp:   return latency_tcp;
            case Channel::Local: return latency_local;
            default:             return latency_http;
        }
    }

    CoreReply handle_core(const std::string& request_body,
                          const std::string& peer,
                          Channel channel = Channel::Http)
    {
        CoreReply out;
        // 流水线 id：请求带则响应原样回带（传输壳负责并入响应 JSON）
        out.has_id = extract_request_id(request_body, out.id);

        // 停机中：立即过载拒绝，客户端可据此重试其他实例。
        // （不占用处理槽位，不进入直方图统计）
        if (stopping.load(std::memory_order_relaxed)) {
            out.http_status = 429;
            out.retry_after_seconds = retry_after_seconds;
            out.body_json = make_response(JsonValue(), "server shutting down");
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                ++rejected_total;
                record_rejected(channel);
            }
            return out;
        }

        // 结果分级：成功 info / 过载或未注册方法 warn / 服务器内部错误 error
        const auto start = std::chrono::steady_clock::now();
        const bool need_log = (logger != nullptr);
        auto elapsed_ms = [start]() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                .count();
        };

        JsonValue body;
        try {
            body = JsonValue::parse(request_body);
        } catch (const std::exception&) {
            if (need_log) {
                logger->warn("rpc method=\"<unknown>\" client={} elapsed={}ms "
                             "error=\"request body is not valid JSON\"",
                             peer, elapsed_ms());
            }
            out.http_status = 400;
            out.body_json =
                make_response(JsonValue(), "request body is not valid JSON");
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                ++completed_total;
                ++completed_malformed;
            }
            return out;
        }

        // 请求体约定：{"method": "...", "params": ...}
        if (!body.is_object() || !body.contains("method") ||
            !body["method"].is_string()) {
            if (need_log) {
                logger->warn("rpc method=\"<unknown>\" client={} elapsed={}ms "
                             "error=\"missing method field\"",
                             peer, elapsed_ms());
            }
            out.http_status = 400;
            out.body_json =
                make_response(JsonValue(), "missing \"method\" field");
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                ++completed_total;
                ++completed_malformed;
            }
            return out;
        }

        const std::string method = body["method"].get<std::string>();
        const std::string params =
            body.contains("params") ? body["params"].dump() : "null";

        // 过载拒绝：并发处理数已达上限时的行为由 overload_mode 决定。
        // （httplib 在 TaskQueue::enqueue 失败时直接断开连接、无法回包，
        // 所以必须在这里拒绝才能把错误文本送达客户端。）
        const bool reject_immediate =
            (overload_mode == RpcOverloadMode::RejectImmediate);
        bool admitted = false;
        std::size_t waited_ms = 0;
        if (reject_immediate) {
            admitted = gate.acquire();
        } else {
            // WaitInQueue：先占一个队列位，再等槽位；
            // 等待期间计入 queued_requests，峰值计入 max_queued。
            // 等待者数量由 queue_wait_ms 的超时自然回流（超时后 429），
            // 不受 max_queued_requests（httplib 传输队列硬上限，见
            // set_max_in_flight）约束，否则会把背压队列误判为已满
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                ++queued;
                if (queued > max_queued) {
                    max_queued = queued;
                }
                admitted = true;  // 已入队，等待结果见下
            }
            if (admitted) {
                const int wait_cap =
                    queue_wait_ms > 0 ? queue_wait_ms : 0;
                admitted = gate.acquire_for(wait_cap, waited_ms);
                {
                    std::lock_guard<std::mutex> lock(stats_mutex);
                    if (queued > 0) {
                        --queued;
                    }
                    if (queued == 0) {
                        queue_warned = false;
                    }
                }
            }
        }
        if (!admitted) {
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                ++rejected_total;
                record_rejected(channel);
            }
            if (need_log) {
                logger->warn("rpc method=\"{}\" client={} elapsed={}ms "
                             "error=\"overloaded\" active>={} waited={}ms",
                             method, peer, elapsed_ms(),
                             gate.current(), waited_ms);
            }
            out.http_status = 429;  // Too Many Requests
            out.retry_after_seconds = retry_after_seconds;
            out.body_json = make_response(JsonValue(),
                                          overload_message.c_str());
            return out;
        }

        // 离开 handle_core 时释放闸门，并记录完成数与处理耗时
        struct GateReleaser
        {
            AdmissionGate& gate;
            LatencyHistogram& latency;
            LatencyHistogram& channel_latency;  // 所属传输的分列直方图
            std::mutex& stats_mutex;
            std::size_t& completed_total;
            std::size_t& completed_channel;  // 所属传输的分列计数
            const std::chrono::steady_clock::time_point start;

            ~GateReleaser()
            {
                gate.release();
                const double ms = std::chrono::duration<
                    double, std::milli>(std::chrono::steady_clock::now() -
                                        start)
                    .count();
                latency.record(ms);
                channel_latency.record(ms);
                std::lock_guard<std::mutex> lock(stats_mutex);
                ++completed_total;
                ++completed_channel;
            }
        } releaser{gate, latency, latency_for(channel), stats_mutex,
                   completed_total, completed_for(channel), start};
        // 记录并发处理峰值
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            const std::size_t now_active = gate.current();
            if (now_active > max_active) {
                max_active = now_active;
            }
        }

        RpcServer::Handler handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex);
            auto it = handlers.find(method);
            if (it != handlers.end()) {
                handler = it->second;
            }
        }

        if (!handler) {
            if (need_log) {
                logger->warn("rpc method=\"{}\" client={} elapsed={}ms "
                             "error=\"method not found\"",
                             method, peer, elapsed_ms());
            }
            out.http_status = 404;
            const std::string msg = "method not found: " + method;
            out.body_json = make_response(JsonValue(), msg.c_str());
            return out;
        }

        try {
            const std::string result_json = handler(params);
            if (need_log) {
                logger->info("rpc method=\"{}\" client={} elapsed={}ms ok",
                             method, peer, elapsed_ms());
            }
            out.body_json =
                make_response(JsonValue::parse(result_json), nullptr);
        } catch (const std::exception& e) {
            if (need_log) {
                logger->error(
                    "rpc method=\"{}\" client={} elapsed={}ms error=\"{}\"",
                    method, peer, elapsed_ms(), e.what());
            }
            out.http_status = 500;
            out.body_json = make_response(JsonValue(), e.what());
        }
        return out;
    }

    // ------------------ HTTP 传输壳 ------------------
    void handle(const httplib::Request& req, httplib::Response& res)
    {
        const CoreReply r =
            handle_core(req.body, req.remote_addr, Channel::Http);
        res.status = r.http_status;
        if (r.http_status == 429 && r.retry_after_seconds > 0) {
            // Retry-After 建议客户端等待重试（秒）；0 = 不发送该头
            res.set_header("Retry-After",
                           std::to_string(r.retry_after_seconds));
        }
        res.set_content(r.body_json, "application/json");
    }

    // ------------------ 本地传输服务端（命名管道 / UDS）------------------
    //
    // accept 线程：接受连接 → 为每个连接开一个处理线程（连接数受
    // max_in_flight 约束的部分由 handle_core 的闸门控制，连接线程本身
    // 轻量，一连接一请求后即结束）。数据面与 HTTP 共享全部统计与过载逻辑。
#ifdef _WIN32
    HANDLE local_stop_event = nullptr;

    // 创建一个管道服务实例（不带 FILE_FLAG_FIRST_PIPE_INSTANCE：
    // 同名多实例是合法且必需的——每次连接需要一个独立实例）
    HANDLE create_pipe_instance() const
    {
        const std::wstring wname = internal::utf8_to_wide(endpoint);
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        // 默认安全描述符：继承进程 ACL，未指定 DACL 时用默认令牌 ACL
        sa.bInheritHandle = FALSE;
        HANDLE h = ::CreateNamedPipeW(
            wname.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            1024 * 64,   // out buffer
            1024 * 64,   // in buffer
            0,           // 默认超时
            &sa);
        return h;
    }

    // 单连接服务：读循环 + 派发。读循环单线程收帧，请求带流水线 id 时
    // 交给线程池异步处理（一连接并发多请求），响应经 send 串行锁回写；
    // 无 id（旧客户端）请求保持同步处理，行为与流水线开启前完全一致
    void serve_pipe_connection(HANDLE pipe)
    {
        {
            std::lock_guard<std::mutex> lock(active_pipes_mutex);
            active_pipes.insert(pipe);
        }
        std::mutex send_mutex;  // 响应帧写互斥（多派发线程并发回写）
        auto send_frame = [&](const std::string& payload) -> bool {
            std::string frame;
            put_u32_le(frame, static_cast<std::uint32_t>(payload.size()));
            frame += payload;
            std::lock_guard<std::mutex> lock(send_mutex);
            if (stopping.load(std::memory_order_relaxed)) {
                // 排水期尽力回包：排水后的响应没有送达意义（连接即将
                // 关闭，TCP/UDS 同样会在 stop 时断开），直接跳过发送。
                // 客户端看到断连 → 内置退避重试衔接新实例
                return false;
            }
            // 重叠句柄写入（句柄建为 FILE_FLAG_OVERLAPPED）
            return pipe_ov_write(
                pipe, frame.data(),
                static_cast<std::uint32_t>(frame.size()), INFINITE);
        };
        // 派发线程表：连接结束时统一 join（派发线程不会超连接生命周期）
        std::vector<std::thread> dispatch;
        bool send_failed = false;
        (void)send_failed;  // 回发失败仅作为跳出读循环的信号，无需进一步处理

        for (;;) {
            std::string request;
            {
                // 读 4 字节长度头（客户端关闭连接时 ReadFile 失败，自然退出）
                char head[4];
                if (!pipe_ov_read(pipe, head, 4, INFINITE)) {
                    break;
                }
                const std::uint32_t len = get_u32_le(head);
                if (len > kMaxFrameBytes) {
                    break;
                }
                request.resize(len);
                if (len > 0 &&
                    !pipe_ov_read(pipe, &request[0], len, INFINITE)) {
                    break;
                }
            }

            std::uint64_t pid = 0;
            const bool has_id = extract_request_id(request, pid);
            if (!has_id) {
                // 无 id：同步处理（旧客户端语义）
                const CoreReply r =
                    handle_core(request, "local-pipe", Channel::Local);
                const std::string payload = core_reply_to_local(
                    r.http_status, r.retry_after_seconds, r.body_json);
                if (!send_frame(payload)) {
                    send_failed = true;
                    break;
                }
                continue;
            }

            // 带 id：异步派发；捕获请求副本，读循环立即回去收下一帧
            dispatch.push_back(std::thread(
                [this, pipe, request, pid, send_frame]() {
                    const CoreReply r =
                        handle_core(request, "local-pipe", Channel::Local);
                    const std::string payload = core_reply_to_local(
                        r.http_status, r.retry_after_seconds, r.body_json,
                        true, pid);
                    (void)send_frame(payload);  // 失败由读循环/停机统一收尾
                }));
        }
        for (auto& t : dispatch) {
            if (t.joinable()) {
                t.join();
            }
        }
        {
            std::lock_guard<std::mutex> lock(active_pipes_mutex);
            active_pipes.erase(pipe);
        }
        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
    }
#else
    int local_listen_fd = -1;

    void local_accept_loop()
    {
        while (!stopping.load(std::memory_order_relaxed)) {
            const int fd = ::accept(local_listen_fd, NULL, NULL);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;  // listen fd 已关闭（停机）
            }
            // 一连接一线程；读循环 + 派发（带 id 请求异步处理，与管道一致）
            std::thread([this, fd]() {
                {
                    std::lock_guard<std::mutex> lock(active_uds_mutex);
                    active_uds.insert(fd);
                }
                std::mutex send_mutex;
                auto send_one = [&](const std::string& payload) -> bool {
                    std::lock_guard<std::mutex> lock(send_mutex);
                    if (stopping.load(std::memory_order_relaxed)) {
                        // 排水期尽力回包：跳过发送（与管道壳一致，
                        // 见 serve_pipe_connection 的 send_frame 注释）
                        return false;
                    }
                    return uds_send_frame(fd, payload);
                };
                std::vector<std::thread> dispatch;
                for (;;) {
                    std::string request;
                    if (!uds_recv_frame(fd, request)) {
                        break;  // 对端关闭或出错
                    }
                    std::uint64_t pid = 0;
                    if (!extract_request_id(request, pid)) {
                        const CoreReply r = handle_core(
                            request, "local-uds", Channel::Local);
                        const std::string payload = core_reply_to_local(
                            r.http_status, r.retry_after_seconds,
                            r.body_json);
                        if (!send_one(payload)) {
                            break;
                        }
                        continue;
                    }
                    dispatch.push_back(std::thread(
                        [this, fd, request, pid, send_one]() {
                            const CoreReply r = handle_core(
                                request, "local-uds", Channel::Local);
                            const std::string payload = core_reply_to_local(
                                r.http_status, r.retry_after_seconds,
                                r.body_json, true, pid);
                            (void)send_one(payload);
                        }));
                }
                for (auto& t : dispatch) {
                    if (t.joinable()) {
                        t.join();
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(active_uds_mutex);
                    active_uds.erase(fd);
                }
                ::close(fd);
            }).detach();
        }
    }
#endif

    // ------------------ Tcp 传输服务端 ------------------
    // 基于裸 TcpServer 的帧协议壳：每个 DATA 帧就是一个请求，
    // 处理与响应与本地传输完全同构（core_reply_to_local 包装）。
    // handle_core 在 TcpServer 的会话线程上执行——过载闸门与统计口径
    // 与其他传输完全一致。
    bool start_tcp_listener()
    {
        std::unique_ptr<TcpServer> srv(new TcpServer());
        srv->set_on_message([this](std::uint64_t conn_id,
                                   const std::string& payload) {
            std::uint64_t pid = 0;
            const bool has_id = extract_request_id(payload, pid);
            if (!has_id) {
                // 无 id：同步处理（旧客户端语义，响应顺序与请求一致）
                const CoreReply r =
                    handle_core(payload, "tcp", Channel::Tcp);
                const std::string resp = core_reply_to_local(
                    r.http_status, r.retry_after_seconds, r.body_json);
                if (tcp_server) {
                    tcp_server->send(conn_id, resp);
                }
                return;
            }
            // 带 id：异步派发。读回调立即返回去收下一帧，同一连接的
            // 多个请求并发处理；响应帧由 handle_core 回带 id，客户端
            // 按 id 匹配，乱序无害。捕获 tcp_server 副本保证停机时
            // TcpServer 在派发线程全部结束前不被析构
            //
            // 停机顺序（与 TCP 的 TIME_WAIT 语义共同成就断连重试衔接）：
            //  1. stopping 置位 → 新请求 429 "server shutting down"；
            //  2. 已在途请求完成处理，但排水期跳过回包（下方检查）
            //     → TcpServer::stop() 关闭会话 socket → 客户端读线程
            //     报连接断开 → 退避重试连接新实例；
            //  3. 排水超时的强断亦然（stop() 不等处理完成）
            std::shared_ptr<TcpServer> srv_keep = tcp_server;
            LocalWorker w;
            w.done = std::make_shared<std::atomic<bool>>(false);
            // C++11 兼容：不用 init-capture（GCC -std=c++11 拒绝），
            // 先复制 done 副本再按值捕获（w.done 随后 move 入表）
            std::shared_ptr<std::atomic<bool>> done = w.done;
            w.th = std::thread(
                [this, conn_id, payload, pid, srv_keep, done]() {
                    const CoreReply r =
                        handle_core(payload, "tcp", Channel::Tcp);
                    const std::string resp = core_reply_to_local(
                        r.http_status, r.retry_after_seconds, r.body_json,
                        true, pid);
                    if (!stopping.load(std::memory_order_relaxed)) {
                        // 排水期尽力回包：跳过发送（与本地壳一致），
                        // 客户端以断连信号触发重试衔接
                        srv_keep->send(conn_id, resp);
                    }
                    done->store(true, std::memory_order_relaxed);
                });
            {
                std::lock_guard<std::mutex> lock(local_worker_mutex);
                worker_threads_runtime.push_back(std::move(w));
                reap_finished_locked();
            }
        });
        const bool ok = srv->start(tcp_host,
                                   static_cast<std::uint16_t>(port));
        if (!ok) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(tcp_mutex);
            bound_tcp_port = static_cast<int>(srv->port());
        }
        tcp_server = std::move(srv);
        return true;
    }

    // 本地传输：bind/listen + 发就绪信号 + accept 循环
    bool bind_and_listen_local()
    {
#ifdef _WIN32
        local_stop_event = ::CreateEventW(NULL, TRUE, FALSE, NULL);
        if (local_stop_event == nullptr) {
            {
                std::lock_guard<std::mutex> lock(ready_mutex);
                bind_done = true;
                bind_ok = false;
            }
            ready_cv.notify_all();
            return false;
        }
        // 端点占用检测：首个实例创建成功后立即关闭再重新创建。
        // 之后的实例不再带该标志（见 create_pipe_instance），
        // 否则同一进程的第二个实例会创建失败，服务器只能服务一条连接
        {
            const HANDLE probe = ::CreateNamedPipeW(
                internal::utf8_to_wide(endpoint).c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1, 256, 256, 0, NULL);
            if (probe == INVALID_HANDLE_VALUE) {
                ::CloseHandle(local_stop_event);
                local_stop_event = nullptr;
                {
                    std::lock_guard<std::mutex> lock(ready_mutex);
                    bind_done = true;
                    bind_ok = false;  // 端点已被占用
                }
                ready_cv.notify_all();
                return false;
            }
            ::CloseHandle(probe);
        }
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            bind_done = true;
            bind_ok = true;
        }
        ready_cv.notify_all();

        // 服务循环：创建实例 → 异步等待连接 → 就绪后交给处理线程。
        // 每个实例用独立的 OVERLAPPED 事件（不能用共享的 stop_event：
        // ConnectNamedPipe 完成也会将其置为 signaled，污染后续等待）。
        // pending 连接在退出时统一关闭，避免泄漏悬挂实例。
        // 关键设计：先创建 N 个监听实例再逐个异步等待连接——
        // 同一时刻必须有多个实例处于“可连接”状态，否则并发客户端
        // 会因无空闲实例而得到 ERROR_PIPE_BUSY
        struct PendingPipe
        {
            HANDLE pipe;
            HANDLE event;
            OVERLAPPED* ov;
        };
        std::vector<PendingPipe> pending;
        // 启动一个连接服务线程（带 done 标志，供中途回收只收割已结束线程）
        auto spawn_pipe_worker = [this](HANDLE pipe) {
            auto done = std::make_shared<std::atomic<bool>>(false);
            LocalWorker w;
            w.done = done;
            w.th = std::thread([this, pipe, done]() {
                serve_pipe_connection(pipe);
                done->store(true, std::memory_order_relaxed);
            });
            std::lock_guard<std::mutex> lock(local_worker_mutex);
            worker_threads_runtime.push_back(std::move(w));
        };
        while (!stopping.load(std::memory_order_relaxed)) {
            // 维持一批可连接实例（先建后等，全部处于监听状态）
            while (pending.size() < 8 &&
                   !stopping.load(std::memory_order_relaxed)) {
                HANDLE pipe = create_pipe_instance();
                if (pipe == INVALID_HANDLE_VALUE) {
                    break;
                }
                OVERLAPPED* ov = new OVERLAPPED{};
                ov->hEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);
                if (ov->hEvent == nullptr) {
                    delete ov;
                    ::CloseHandle(pipe);
                    break;
                }
                const BOOL connected = ::ConnectNamedPipe(pipe, ov);
                if (!connected) {
                    const DWORD err = ::GetLastError();
                    if (err == ERROR_IO_PENDING) {
                        pending.push_back({pipe, ov->hEvent, ov});
                        continue;
                    }
                    if (err == ERROR_PIPE_CONNECTED) {
                        // 客户端在 ConnectNamedPipe 之前已连上：直接服务
                        spawn_pipe_worker(pipe);
                    }
                    // 其他错误（含 ERROR_NO_DATA）：丢弃实例继续
                } else {
                    // 同步完成：交给处理线程
                    spawn_pipe_worker(pipe);
                }
                ::CloseHandle(ov->hEvent);
                delete ov;
            }
            if (pending.empty() || stopping.load(std::memory_order_relaxed)) {
                break;
            }

            // 阻塞等待：任一挂起连接就绪，或停机信号
            HANDLE waits[MAXIMUM_WAIT_OBJECTS];
            DWORD count = 0;
            waits[count++] = local_stop_event;
            for (std::size_t i = 0;
                 i < pending.size() && count < MAXIMUM_WAIT_OBJECTS - 1;
                 ++i) {
                waits[count++] = pending[i].event;
            }
            ::WaitForMultipleObjects(count, waits, FALSE, INFINITE);
            if (stopping.load(std::memory_order_relaxed)) {
                break;
            }
            // 收割所有已完成的连接
            for (std::size_t i = pending.size(); i > 0; --i) {
                PendingPipe& p = pending[i - 1];
                if (::WaitForSingleObject(p.event, 0) != WAIT_OBJECT_0) {
                    continue;
                }
                spawn_pipe_worker(p.pipe);
                ::CloseHandle(p.event);
                delete p.ov;
                pending.erase(pending.begin() + (i - 1));
            }
            // 只回收已结束的处理线程（连接池的常驻连接会长期阻塞在读，
            // 不能 join 它们），防止 vector 无界增长
            if (worker_threads_runtime.size() > 64) {
                std::lock_guard<std::mutex> lock(local_worker_mutex);
                for (std::size_t i = worker_threads_runtime.size(); i > 0; --i) {
                    LocalWorker& w = worker_threads_runtime[i - 1];
                    if (w.done && w.done->load(std::memory_order_relaxed) &&
                        w.th.joinable()) {
                        w.th.join();
                        worker_threads_runtime.erase(
                            worker_threads_runtime.begin() +
                            static_cast<std::ptrdiff_t>(i - 1));
                    }
                }
            }
        }
        // 停机：唤醒等待，取消所有仍挂起的 ConnectNamedPipe 并清理；
        // 再取消所有活跃服务管道的阻塞读（连接池常驻连接否则永远不退出），
        // 最后才能安全 join 全部服务线程
        for (PendingPipe& p : pending) {
            ::CancelIoEx(p.pipe, NULL);
            ::DisconnectNamedPipe(p.pipe);
            ::CloseHandle(p.pipe);
            ::CloseHandle(p.event);
            delete p.ov;
        }
        {
            std::set<HANDLE> snapshot;
            {
                std::lock_guard<std::mutex> lock(active_pipes_mutex);
                snapshot = active_pipes;
            }
            for (HANDLE h : snapshot) {
                ::CancelIoEx(h, NULL);
            }
        }
        for (auto& w : worker_threads_runtime) {
            if (w.th.joinable()) {
                w.th.join();
            }
        }
        worker_threads_runtime.clear();
        return true;
#else
        // UDS：listen fd 常驻，停机时关闭使 accept 返回错误
        local_listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (local_listen_fd < 0) {
            {
                std::lock_guard<std::mutex> lock(ready_mutex);
                bind_done = true;
                bind_ok = false;
            }
            ready_cv.notify_all();
            return false;
        }
        ::unlink(endpoint.c_str());  // 残留 socket 文件会占用地址
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (endpoint.size() >= sizeof(addr.sun_path) ||
            ::bind(local_listen_fd,
                   reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
                0) {
            ::close(local_listen_fd);
            local_listen_fd = -1;
            {
                std::lock_guard<std::mutex> lock(ready_mutex);
                bind_done = true;
                bind_ok = false;
            }
            ready_cv.notify_all();
            return false;
        }
        if (::listen(local_listen_fd, 64) != 0) {
            ::close(local_listen_fd);
            local_listen_fd = -1;
            ::unlink(endpoint.c_str());
            {
                std::lock_guard<std::mutex> lock(ready_mutex);
                bind_done = true;
                bind_ok = false;
            }
            ready_cv.notify_all();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            bind_done = true;
            bind_ok = true;
        }
        ready_cv.notify_all();

        local_accept_loop();

        // 停机：解除所有活跃连接的 recv 阻塞（连接池常驻连接否则不会退出）
        {
            std::set<int> snapshot;
            {
                std::lock_guard<std::mutex> lock(active_uds_mutex);
                snapshot = active_uds;
            }
            for (int fd : snapshot) {
                ::shutdown(fd, SHUT_RDWR);
            }
        }

        ::close(local_listen_fd);
        local_listen_fd = -1;
        ::unlink(endpoint.c_str());
        return true;
#endif
    }

    // ------------------ 可监控工作队列 ------------------
    //
    // httplib 自带 ThreadPool 的队列深度是私有的，无法观测，因此实现一个
    // TaskQueue：行为与 httplib::ThreadPool 一致（支持线程数与队列上限配置），
    // 额外在入队/出队时通知属主，用于 stats() 采样与队列积压告警。
    struct MonitoringQueue : public httplib::TaskQueue
    {
        Impl* owner;
        const std::size_t max_queued_requests;

        std::vector<std::thread> workers;
        std::deque<std::function<void()>> jobs;

        std::mutex mutex;
        std::condition_variable cond;
        bool shutdown_flag = false;

        MonitoringQueue(Impl* owner_, std::size_t threads_,
                        std::size_t max_queued)
            : owner(owner_), max_queued_requests(max_queued)
        {
            workers.reserve(threads_);
            for (std::size_t i = 0; i < threads_; ++i) {
                workers.emplace_back([this]() { this->worker_loop(); });
            }
        }

        ~MonitoringQueue() override
        {
            // httplib 在销毁队列前已调用过 shutdown()；这里仅兜底并解绑属主
            shutdown();
            if (owner) {
                owner->on_queue_destroyed();
            }
            owner = nullptr;
        }

        MonitoringQueue(const MonitoringQueue&) = delete;
        MonitoringQueue& operator=(const MonitoringQueue&) = delete;

        bool enqueue(std::function<void()> fn) override
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (shutdown_flag) {
                    return false;
                }
                if (max_queued_requests > 0 &&
                    jobs.size() >= max_queued_requests) {
                    return false;
                }
                jobs.push_back(std::move(fn));
            }
            owner->on_job_enqueued();
            cond.notify_one();
            return true;
        }

        void shutdown() override
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                shutdown_flag = true;
            }
            cond.notify_all();
            for (auto& t : workers) {
                t.join();
            }
            workers.clear();
        }

    private:
        void worker_loop()
        {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cond.wait(lock, [this] {
                        return !jobs.empty() || shutdown_flag;
                    });
                    if (shutdown_flag && jobs.empty()) {
                        return;
                    }
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                owner->on_job_dequeued();
                job();
            }
        }
    };

    ~Impl()
    {
        // 兕底：外部忘记调用 RpcServer::stop() 时也能安全销毁
        shutdown();
    }
};

RpcServer::RpcServer(int port)
    : impl_(new Impl(port)) {}

RpcServer::RpcServer(const std::string& pipe_or_socket_path)
    : impl_(new Impl(RpcTransport::LocalPipe, pipe_or_socket_path)) {}

RpcServer::RpcServer(RpcTransport transport, const std::string& endpoint)
    : impl_(new Impl(transport, endpoint)) {}

RpcServer::~RpcServer()
{
    delete impl_;  // Impl 析构里兜底 stop + join
}

RpcServer::RpcServer(RpcServer&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

RpcServer& RpcServer::operator=(RpcServer&& other) noexcept
{
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void RpcServer::register_method(const std::string& name, Handler handler)
{
    std::lock_guard<std::mutex> lock(impl_->handlers_mutex);
    impl_->handlers[name] = std::move(handler);
}

void RpcServer::set_logger(spdlog::logger* logger)
{
    impl_->logger = logger;
}

void RpcServer::set_max_in_flight(std::size_t max_requests)
{
    impl_->gate.set_limit(max_requests);

    // 队列硬上限与处理上限一致：超出部分连接会被服务端直接关闭
    // （httplib 对 enqueue 失败的请求无法回 HTTP 响应），作为内存保护的最后防线
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    impl_->max_queued_requests = max_requests;
}

void RpcServer::set_overload_message(const std::string& message)
{
    impl_->overload_message = message;
}

void RpcServer::set_queue_warn_threshold(std::size_t queued)
{
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    impl_->queue_warn_threshold = queued;
    impl_->queue_warned = false;
}

void RpcServer::set_worker_threads(std::size_t threads)
{
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    impl_->worker_threads = threads;
}

void RpcServer::set_overload_mode(RpcOverloadMode mode)
{
    impl_->overload_mode = mode;
}

void RpcServer::set_queue_wait_ms(int ms)
{
    impl_->queue_wait_ms = ms;
}

void RpcServer::set_drain_timeout_ms(int ms)
{
    impl_->drain_timeout_ms = ms;
}

void RpcServer::set_retry_after_seconds(int seconds)
{
    impl_->retry_after_seconds = seconds;
}

RpcServerStats RpcServer::stats() const
{
    return impl_->stats();
}

RpcLatencyStats RpcServer::latency_stats(
    const std::vector<double>& extra_percentiles, RpcTransportFilter filter) const
{
    return impl_->latency_stats(extra_percentiles, filter);
}

void RpcServer::run()
{
    impl_->started = true;
    impl_->bind_and_listen();
}

void RpcServer::start_background()
{
    Impl* impl = impl_;
    if (impl->worker.joinable()) {
        impl->shutdown();  // 上一次启动未停止时先清理，避免重复启动挂死
    }
    impl->bind_done = false;
    impl->bind_ok = false;
    impl->started = true;
    impl->worker = std::thread([impl]() {
        impl->bind_and_listen();
    });
}

void RpcServer::stop()
{
    impl_->shutdown();
    {
        std::lock_guard<std::mutex> lock(impl_->ready_mutex);
        impl_->started = false;
        impl_->bind_done = false;
        impl_->bind_ok = false;
    }
    // 复位 worker：必须先 join（shutdown 已保证循环退出）再销毁——
    // 直接移动赋值空线程在 joinable 状态下会 std::terminate
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->worker = std::thread();
}

bool RpcServer::is_running() const
{
    if (impl_->transport == RpcTransport::LocalPipe) {
        return impl_->bind_ok && !impl_->stopping.load();
    }
    if (impl_->transport == RpcTransport::Tcp) {
        return impl_->tcp_server && impl_->tcp_server->is_running() &&
               !impl_->stopping.load();
    }
    return impl_->http.is_running();
}

int RpcServer::port() const
{
    if (impl_->transport == RpcTransport::Tcp) {
        std::lock_guard<std::mutex> lock(impl_->tcp_mutex);
        return impl_->bound_tcp_port > 0 ? impl_->bound_tcp_port : -1;
    }
    return impl_->bound_port;
}

RpcTransport RpcServer::transport() const
{
    return impl_->transport;
}

std::string RpcServer::endpoint() const
{
    if (impl_->transport == RpcTransport::Http ||
        impl_->transport == RpcTransport::Tcp) {
        // Http 客户端默认连 127.0.0.1；Tcp 端口 0 自动分配后回填真实端口
        const std::string host = (impl_->transport == RpcTransport::Tcp &&
                                  impl_->tcp_host != "0.0.0.0")
                                     ? impl_->tcp_host
                                     : "127.0.0.1";
        const int p = (impl_->transport == RpcTransport::Tcp)
                          ? impl_->bound_tcp_port
                          : impl_->bound_port;
        return host + ":" + std::to_string(p);
    }
    return impl_->endpoint;
}

void RpcServer::set_tcp_host(const std::string& host)
{
    if (impl_->transport == RpcTransport::Tcp && !host.empty()) {
        impl_->tcp_host = host;
    }
}

bool RpcServer::wait_until_ready(int timeout_ms)
{
    Impl* impl = impl_;
    std::unique_lock<std::mutex> lock(impl->ready_mutex);

    const bool done = impl->ready_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [impl]() { return impl->bind_done; });

    return done && impl->bind_ok;
}

}  // namespace libmini
