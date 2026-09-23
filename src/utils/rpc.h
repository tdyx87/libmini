#ifndef LIBMINI_RPC_H
#define LIBMINI_RPC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <future>
#include <memory>
#include <vector>

#include "libmini.h"
#ifndef LIBMINI_STATIC
#ifdef LIBMINI_EXPORTS
#define LIBMINI_API __declspec(dllexport)
#else
#define LIBMINI_API __declspec(dllimport)
#endif
#else
#define LIBMINI_API
#endif

namespace spdlog {
class logger;
}

namespace libmini {

// 过载处理模式：并发处理数达到 set_max_in_flight() 上限时的行为
enum class RpcOverloadMode {
    RejectImmediate,  // 立即返回 429（默认；延迟低，调用方负责退避重试）
    WaitInQueue,      // 排队等待处理槽位（默认上限 10 秒）；等待超时才返回 429
};

// 传输层：TCP 上跑 HTTP（可跨机），本机命名管道/Unix 域套接字（仅本机，
// 无回环网络开销，受文件系统 ACL 保护，不占端口），或裸 TCP 长连接
//（帧协议比 HTTP 更轻，适合内网高频调用）
enum class RpcTransport {
    Http,       // HTTP over TCP（默认；host/port 端点）
    LocalPipe,  // Windows 命名管道 / POSIX Unix 域套接字（path/pipe_name 端点）
    Tcp,        // 裸 TCP 长连接 + 帧协议（"host:port" 端点；语义与 HTTP 一致）
};

// 单一传输的完成/拒绝计数（completed 含业务错误/异常响应，不含 429）
struct RpcTransportCounters {
    std::size_t completed = 0;
    std::size_t rejected = 0;
};

// 服务器负载快照（所有值都为某一时刻的采样）
struct RpcServerStats {
    std::size_t active_requests;   // 正在处理中的请求数
    std::size_t queued_requests;   // 在队列中等待的请求数
    std::size_t max_active;        // 历史峰值：最大并发处理数
    std::size_t max_queued;        // 历史峰值：最大队列积压数
    std::size_t rejected_total;    // 因过载被拒绝的请求总数（全部传输合计）
    std::size_t completed_total;   // 已完成处理的请求总数（含业务错误/异常，全部传输合计）

    // 按传输分列的口径。单传输服务器只有自己那一列非零；
    // 本地传输的管道与 UDS 合并在 local 列（一个服务器实例只有一种本地实现）
    RpcTransportCounters http;    // HTTP over TCP 传输的 completed/rejected
    RpcTransportCounters tcp;     // 裸 TCP 帧协议传输的 completed/rejected
    RpcTransportCounters local;   // 本地传输（命名管道 / UDS）的 completed/rejected

    // 与传输无关的请求级错误（JSON 解析失败/缺 method 字段，未达传输壳层）
    // ——计入合计但不计入任何分列
    RpcTransportCounters malformed;
};

// latency_stats() 的传输过滤：All 为合计（原有行为），其余只统计
// 对应传输处理的请求（请求级格式错误不进任何直方图，不计入）
enum class RpcTransportFilter {
    All,     // 全部传输合计（默认）
    Http,    // 仅 HTTP over TCP 传输
    Tcp,     // 仅裸 TCP 帧协议传输
    Local,   // 仅本地传输（命名管道 / UDS 合并）
};

// 请求耗时分布快照（由服务器在内部按毫秒级对数分桶记录，仅统计
// 成功进入处理阶段的请求，即不含 429 拒绝与请求格式错误的耗时）。
// 通过 RpcServer::latency_stats() 获取；可按传输过滤分列查询。
struct RpcLatencyStats {
    std::size_t sample_count = 0;  // 样本总数
    double min_ms = 0.0;           // 最小耗时
    double max_ms = 0.0;           // 最大耗时
    double mean_ms = 0.0;          // 平均耗时
    // 估算分位数（毫秒）。样本量小时精度有限；仅在样本数为 0 时返回 -1。
    double p50_ms = -1.0;
    double p90_ms = -1.0;
    double p95_ms = -1.0;
    double p99_ms = -1.0;
    // 自定义分位数（取值 [0,100]），如 {99.9}；越界值忽略
    std::vector<double> percentiles;
};

// 客户端连接池快照（LocalPipe/Tcp 传输；HTTP 由 httplib 内置 keep-alive）
struct RpcClientPoolStats {
    std::size_t open_connections = 0;  // 当前打开的连接总数（空闲 + 使用中）
    std::size_t idle_connections = 0;  // 空闲可复用的连接数
    std::size_t busy_connections = 0;  // 正在执行请求的连接数
    std::uint64_t created_total = 0;   // 累计新建连接数
    std::uint64_t reused_total = 0;    // 累计复用池中连接的调用次数
    std::uint64_t closed_total = 0;    // 累计关闭连接数（出错丢弃/空闲回收/超容量）

    // ---- 流水线通道（set_pipeline_max_in_flight() 开启后才有流量）----
    // 独立于连接池计数器：串行模式走池、流水线模式走专用单连接通道，
    // 两者不重叠。total 类计数器只增不清零（随客户端生命周期）
    std::size_t pipeline_in_flight = 0;        // 当前在途请求数（实时值）
    std::size_t pipeline_in_flight_peak = 0;   // 在途峰值
    std::uint64_t pipeline_reused_total = 0;   // 单连接复用次数（未断线连续承载的请求数）
    std::uint64_t pipeline_broken_total = 0;   // 通道断线重建次数
    std::uint64_t pipeline_slot_wait_total = 0;  // 在途槽位等待次数（竞争发生强度）
    std::uint64_t pipeline_slot_timeout_total = 0;  // 等待槽位超时（受在途上限钳制）
};

// 错误码：call() 失败时通过 last_error() 获取对应的文本描述。
// 注意：启用重试后，可重试的失败（过载/连接失败/超时）会在客户端内部自动
// 重试，重试次数用尽仍失败时才返回对应错误码。
enum class RpcError {
    OK = 0,               // 成功
    CONNECTION_FAILED,    // 无法连接到服务器
    TIMEOUT,              // 连接或请求超时
    SERVER_ERROR,         // 服务器返回 4xx/5xx 错误（不可重试的错误）
    PROTOCOL_ERROR,       // HTTP 响应不是 JSON 格式
    OVERLOADED,           // 服务器过载（429）；重试用尽后返回
    UNKNOWN               // 其他未知错误
};

// 简单 RPC 客户端
//
// 端点与传输：
//   RpcClient("127.0.0.1", 8080)          → HTTP over TCP
//   RpcClient("\\\\.\\pipe\\myrpc")          → Windows 命名管道
//   RpcClient("/tmp/myapp.rpc")           → POSIX Unix 域套接字
//   RpcClient(RpcTransport::LocalPipe, endpoint) → 显式指定本地传输
//   RpcClient(RpcTransport::Tcp, "10.0.0.5:9000") → 裸 TCP 帧协议（跨机内网）
//   （端点首个字符为 '/' 或包含 "\\\\.\\pipe\\" 前缀时自动识别为本地传输）
//
// 协议约定：发送 JSON 请求体
//   { "method": "...", "params": ... }
// 服务器返回 JSON 响应体
//   { "result": ..., "error": null } 或 { "result": null, "error": "..." }
// 本地传输以「4 字节小端长度 + JSON 文本」帧收发；
// Tcp 传输复用 tcp 模块的帧协议（DATA 帧），响应 JSON 内嵌 status 字段。
// 两种帧传输均支持一连接多请求（连接池默认开启）与请求流水线
//（请求带自增 id、响应回带 id 按序号匹配，见客户端连接池配置）。
//
// 重试机制：默认重试 3 次。可重试的失败包括：
//   - 过载响应：优先按服务器提示（HTTP Retry-After 头）等待，否则用指数退避；
//   - 连接失败与连接超时：按指数退避等待后重试。
// 退避间隔 = base_delay_ms * 2^(已重试次数)，上限 max_delay_ms；
// 总等待时间不超过 max_total_wait_ms（从第一次调用开始计）。
// 服务器错误（除过载外）、协议错误与请求格式错误不重试。
//
// 该类不可拷贝，可以移动。
class LIBMINI_API RpcClient {
public:
    // HTTP over TCP
    RpcClient(const std::string& host, int port);
    // 本地传输（Windows 命名管道 / POSIX UDS）。端点形如
    // "\\\\.\\pipe\\name" 或 "/tmp/app.sock"；不带前缀的名字自动补全
    explicit RpcClient(const std::string& pipe_or_socket_path);
    // 同上；transport 参数显式指定
    explicit RpcClient(RpcTransport transport,
                       const std::string& endpoint = std::string());
    ~RpcClient();

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    RpcClient(RpcClient&&) noexcept;
    RpcClient& operator=(RpcClient&&) noexcept;

    // 调用远程方法，返回服务器响应的 JSON 文本
    // 失败时返回空字符串，可通过 last_error() / last_error_message() 获取原因
    std::string call(const std::string& method, const std::string& params);

    // 单次调用级超时版本：本调用的连接建立、请求收发与池排队等待均
    // 受 timeout_ms 约束，不改全局 set_timeout_ms 配置（并发安全）。
    // timeout_ms <= 0 时与双参 call() 等价（使用客户端默认超时）。
    // 说明：
    //   - Tcp/本地/流水线路径完全按本次超时执行；
    //   - 重试的每一次尝试都使用本次超时（重试次数/退避仍走全局配置）；
    //   - HTTP 传输的超时是客户端连接级配置，本调用前会临时切换；
    //     并发调用若使用互不相同的单次超时存在极小配置竞窗，
    //     需要严格隔离时请使用独立 RpcClient 实例。
    std::string call(const std::string& method, const std::string& params,
                     int timeout_ms);

    // ------------------ 异步调用 ------------------
    //
    // call_async 在内部线程池上执行与 call() 完全相同的请求路径
    //（重试/退避/连接池/流水线/超时全部生效），不阻塞调用线程。
    // 线程安全：可在多线程并发调用，与 call() 混用也安全。

    // 异步调用：返回 future，get() 时得到与 call() 相同语义的结果
    //（失败为空字符串；错误原因在回调/future 就绪后经 last_error()
    //  读取——多线程混用时建议改用回调形态携带独立错误信息）
    std::future<std::string> call_async(const std::string& method,
                                        const std::string& params);

    // 单次调用级超时版本（语义同三参 call；timeout_ms <= 0 用默认）
    std::future<std::string> call_async(const std::string& method,
                                        const std::string& params,
                                        int timeout_ms);

    // 回调形态：完成时在内部线程上执行 callback，参数为
    //   result  ：与 call() 返回值相同（失败为空字符串）
    //   error   ：本次调用的错误码（回调专属副本，不受其他调用影响）
    //   message ：错误文本描述（成功为空）
    // 回调里不要再调用本客户端的阻塞方法（可调用 call_async）。
    // 客户端析构语义：析构等待所有在途异步调用结束后才完成——回调中
    // 捕获了 this 或引用时不会遭遇悬空（析构会 join 工作线程）。
    using RpcAsyncCallback = std::function<void(const std::string& result,
                                                RpcError error,
                                                const std::string& message)>;
    void call_async(const std::string& method, const std::string& params,
                    RpcAsyncCallback callback);

    // 单次调用级超时版本（语义同三参 call；timeout_ms <= 0 用默认）
    void call_async(const std::string& method, const std::string& params,
                    int timeout_ms, RpcAsyncCallback callback);

    // 异步执行器线程数，默认 8，0 = 恢复默认。控制 call_async 的并发
    // 上限（并发 = 同时处于请求路径的执行器线程数）。
    //   - 首个 call_async 前设置：执行器按新值创建；
    //   - 执行器已存在时设置：排空在途调用后替换为新线程池，返回时
    //     已提交的回调/future 全部完成；等待期间不阻塞同步 call()，
    //     回调里提交的 call_async 会落到新池。
    // 请在发起异步调用前配置：与并发 call_async 竞态调用的行为未定义
    void set_async_workers(std::size_t workers);

    // 当前异步执行器线程数：尚未创建执行器（还没有异步调用）时返回
    // 配置值，已创建时返回实际线程数
    std::size_t async_workers() const;

    // 设置单次请求超时（毫秒），默认 5000
    void set_timeout_ms(int timeout_ms);

    // ------------------ 重试与退避配置 ------------------

    // 最大重试次数（不含首次调用），默认 3，设 0 禁用重试
    void set_max_retries(int retries);

    // 指数退避基础间隔（毫秒），默认 100：
    // 第 1 次重试等 100ms、第 2 次等 200ms、第 3 次等 400ms……
    void set_retry_base_delay_ms(int ms);

    // 单次退避间隔上限（毫秒），默认 4000
    void set_retry_max_delay_ms(int ms);

    // 单次 call() 内累计等待时间上限（毫秒），默认 10000。
    // 达到上限后即使还有剩余重试次数也直接失败（返回最后一个错误）。
    void set_retry_max_total_wait_ms(int ms);

    // 相等抖动（equal jitter）：实际等待 = 退避间隔的一半 + [0, 一半) 的随机量，
    // 避免多个客户端同时失败后同一时刻发起重试（重试风暴）。
    // 默认关闭；开启后不影响 Retry-After 等待（服务器指定值不随机化）。
    void set_retry_jitter(bool enable);

    // 单次 call() 的总体耗时统计（含重试等待），最后一次调用结束后有效；
    // 未调用过或出错清零路径返回 -1
    double last_call_elapsed_ms() const;

    // 设置重试日志器（spdlog）。设置后每次重试前记录：
    //   "rpc retry method=\"...\" attempt=2/3 wait=118ms reason=\"...\""
    // 默认不记录。
    // 指针由调用方持有并保证在 RpcClient 生命周期内有效（可为空以取消日志）。
    void set_logger(spdlog::logger* logger);

    // 最近一次 call() 的错误码与文本描述
    RpcError last_error() const;
    std::string last_error_message() const;

    // 传输类型与端点（HTTP 端点为 "host:port"，本地为管道/套接字路径）
    RpcTransport transport() const;
    std::string endpoint() const;

    // ------------------ 连接池（LocalPipe/Tcp 传输）------------------
    //
    // 复用连接避免每次调用重连（三次握手/管道建立是本地与 Tcp 传输的
    // 主要单调用开销）。池语义：
    //   - 每个连接同一时刻只服务一个请求（请求-响应严格串行），
    //     并发调用从池中各取一条，相当于把并发上限前置到客户端；
    //   - 超过 max_connections 时调用方阻塞等待其他调用归还连接，
    //     等待超过单请求超时仍拿不到则本次调用报 TIMEOUT；
    //   - 空闲连接超过 idle_timeout_ms 后在下次取用/归还时回收；
    //   - 池中连接失效（服务器重启/对端关闭）时自动丢弃并换新连接
    //     立即重试一次，对调用方透明；
    //   - max_connections 设 0 禁用池：每次调用新建连接、用完即断（旧行为）。
    // HTTP 传输不受影响（httplib 内置 keep-alive）。

    // 池的最大连接数（= 并发调用上限），默认 8；0 = 禁用池。可随时调整
    void set_connection_pool_max(std::size_t max_connections);

    // 空闲连接回收时间（毫秒），默认 30000。可随时调整
    void set_connection_pool_idle_ms(int ms);

    // ------------------ 请求流水线（LocalPipe/Tcp 传输）------------------
    //
    // 一条连接上并发多个在途请求：请求带自增序号（响应按序号匹配，
    // 无顺序要求），连接不再被单个慢请求独占。仅当连接池启用时生效，
    // 每连接在途请求数上限为该值；0 = 关闭（每连接同时只跑一个请求，
    // 默认，行为与流水线开启前一致）。
    // 服务器自动支持（对帧协议响应多帧并发处理），无需配置；
    // 与旧客户端互操作不受影响（带序号的新客户端请求对旧服务器
    // 是普通请求，只是 id 字段被忽略、响应无 id，此时客户端自动
    // 退化为串行交换）。

    // 每连接在途请求上限（1 = 单在途但走流水线路径），默认 0 = 关闭
    void set_pipeline_max_in_flight(std::size_t max_in_flight);

    // 连接池快照（线程安全，可随时调用）
    RpcClientPoolStats pool_stats() const;

private:
    struct Impl;
    Impl* impl_;

    // 同步/异步共用的执行核心：构建请求 → 重试循环 → 结果与错误记录。
    // async 状态指针非空时（异步路径）错误写入专属副本，
    // 为空时（同步路径）写入 last_error()/last_message()
    // call_timeout_ms <= 0 = 用客户端默认超时（set_timeout_ms）
    std::string call_core(const std::string& method, const std::string& params,
                          RpcError* async_error, std::string* async_message,
                          int call_timeout_ms = 0);
};
//
// 端点与传输（三选一，构造时确定）：
//   RpcServer(8080)                        → HTTP over TCP（0 = 自动分配端口）
//   RpcServer("\\\\.\\pipe\\myrpc")          → Windows 命名管道
//   RpcServer("/tmp/myapp.rpc")            → POSIX Unix 域套接字
//   RpcServer(RpcTransport::LocalPipe, endpoint) → 显式指定本地传输
//   RpcServer(RpcTransport::Tcp, "0.0.0.0:9000") → 裸 TCP 帧协议（端口 0 自动分配）
//
// 注册名为 "method" 的处理器后，客户端请求体中的 "method" 字段与其匹配时，
// 处理器收到的参数是请求体中 "params" 字段的原始 JSON 文本，
// 处理器的返回值将作为响应体中 "result" 字段的值。
//
// stats() 返回的 completed/rejected 除合计外按传输分列（http/tcp/local，
// 请求级格式错误单列 malformed），合计 = 各分列之和。
// 未注册的 method 返回 {"result":null,"error":"method not found"}（HTTP 404；
// 本地与 Tcp 传输以 status:"not_found" 表达）；处理器抛出的异常返回
// {"result":null,"error":<e.what()>}（HTTP 500 / status:"handler_error"）。
// 过载拒绝在所有传输下统一为 429 语义（本地与 Tcp 以 status:"overloaded" 表达）。
//
// 过载保护、队列监控、延迟直方图、日志与优雅停机在所有传输下行为一致。
//
// 该类不可拷贝，可以移动。
class LIBMINI_API RpcServer {
public:
    using Handler = std::function<std::string(const std::string&)>;

    // HTTP over TCP（port = 0 表示自动分配）
    explicit RpcServer(int port);
    // 本地传输（Windows 命名管道 / POSIX UDS）。端点形如
    // "\\\\.\\pipe\\name" 或 "/tmp/app.sock"；UDS 会自动 unlink 旧文件。
    // 不带 "\\\\.\\pipe\\" 前缀的名字会自动补全
    explicit RpcServer(const std::string& pipe_or_socket_path);
    // 同上；transport 参数显式指定（Http + 字符串端点不合法，按 LocalPipe 处理）
    explicit RpcServer(RpcTransport transport,
                       const std::string& endpoint = std::string());
    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;
    RpcServer(RpcServer&&) noexcept;
    RpcServer& operator=(RpcServer&&) noexcept;

    // 注册方法
    void register_method(const std::string& name, Handler handler);

    // 启动服务器，阻塞当前线程直到调用 stop()
    void run();

    // 在另一个线程启动服务器，立即返回；配合 wait_until_ready() 使用
    void start_background();

    // 设置请求日志器（spdlog）。设置后每次调用都会记录：
    // 方法名、客户端地址、耗时（毫秒）与结果：
    //   成功 -> info 级别；业务错误 -> warn 级别；服务器内部错误 -> error 级别
    // 另外，队列积压超过 set_queue_warn_threshold() 时会额外发出 warn：
    //   "rpc queue backlog=N"
    // 未设置（默认）时不记录任何日志。
    // 指针由调用方持有并保证在 RpcServer 生命周期内有效（可为空以取消日志）。
    void set_logger(spdlog::logger* logger);

    // 停止服务器（阻塞直到所有监听线程退出）。
    // 优雅停机：先关闭监听 socket（新请求立即连接失败，客户端可重试别的实例），
    // 再等待排空窗口（见 set_drain_timeout_ms），让已接收的请求处理完毕；
    // 窗口用尽后剩余的排队请求才被放弃（客户端收到 429/连接断开）。
    // stop() 可重复调用；之后可重新 run()/start_background()。
    void stop();

    // 优雅停机的排空窗口（毫秒），默认 3000。
    // 仅影响 stop() 的行为：窗口内排队等待槽位的请求仍会被处理；
    // 设为 0 则立即放弃所有排队请求（旧行为）。
    void set_drain_timeout_ms(int ms);

    // 服务器是否已开始监听
    bool is_running() const;

    // 实际监听的端口；构造时传 0 表示自动分配，bind 成功后可通过此方法获取
    // 未启动或绑定失败时返回 -1；本地传输恒为 -1
    int port() const;

    // 设置 Tcp 传输监听地址，默认 "0.0.0.0"。须在启动前设置
    void set_tcp_host(const std::string& host);

    // 传输类型与端点（HTTP/Tcp 端点为 "host:port"；本地为管道/套接字路径）
    RpcTransport transport() const;
    std::string endpoint() const;

    // 阻塞等待服务器开始监听（供 start_background() 后同步）
    bool wait_until_ready(int timeout_ms = 5000);

    // ------------------ 过载保护与监控 ------------------
    //
    // 服务器内部使用固定线程池处理请求。未在处理中的请求在队列里等待，
    // 队列深度与并发数可随时通过 stats() 采样。
    //
    // 当同时在处理中的请求数达到 set_max_in_flight() 设置的上限时，
    // 新请求立即返回 429 Too Many Requests，
    // 响应体为 {"result":null,"error":"<自定义文本>"}，
    // 该拒绝同样计入日志与 stats() 的 rejected_total。
    //
    // max_in_flight 与 worker_threads 须在 start_background()/run() 之前设置；
    // 过载文本与积压告警阈值可在运行中随时调整。

    // 最大同时处理中的请求数（排队中的不计入）。默认 0 = 不限制。
    void set_max_in_flight(std::size_t max_requests);

    // 过载拒绝时返回的 error 文本，默认 "server overloaded"
    void set_overload_message(const std::string& message);

    // 队列积压告警阈值：排队数首次达到该值时发一条 warn（回落到 0 后重新武装），
    // 默认 0 = 不告警
    void set_queue_warn_threshold(std::size_t queued);

    // 工作线程数。默认 0 = 使用 httplib 默认值（max(8, CPU 核数-1)）。
    // 须在启动前设置。
    void set_worker_threads(std::size_t threads);

    // ------------------ 过载模式与背压 ------------------

    // 过载处理模式，默认 RejectImmediate。须在启动前设置。
    //   RejectImmediate：达到 max_in_flight 后新请求立即 429；
    //   WaitInQueue：超过槽位的请求在队列中等待槽位释放（公平排队），
    //                等待超过 set_queue_wait_ms() 仍拿不到槽位才 429。
    // 两种模式下被 429 拒绝的请求都计入 rejected_total；
    // WaitInQueue 下等待中的请求计入 queued_requests，峰值计入 max_queued。
    void set_overload_mode(RpcOverloadMode mode);

    // WaitInQueue 模式下排队等待槽位的上限（毫秒），默认 10000。
    // 仅在 set_overload_mode(WaitInQueue) 后生效，可运行中调整。
    void set_queue_wait_ms(int ms);

    // 429 响应携带的 Retry-After 头（秒）。0 = 不发送该头（默认发送 1）。
    // 客户端会按该值等待后重试；0 时客户端改用自身指数退避。
    // 可运行中调整。
    void set_retry_after_seconds(int seconds);

    // 请求耗时分布快照（线程安全，可在运行中随时调用）。
    // 仅统计成功进入处理阶段的请求；服务器重启（重新 run/start_background）后清零。
    // filter 指定传输过滤：All = 全部合计（默认），Http/Tcp/Local = 只统计
// 该传输处理的请求（与 stats() 的分列口径一致）。指定传输无样本时
    // sample_count 为 0、各分位为 -1
    RpcLatencyStats latency_stats(
        const std::vector<double>& extra_percentiles
            = std::vector<double>(),
        RpcTransportFilter filter = RpcTransportFilter::All) const;

    // 当前负载快照（线程安全，可在运行中随时调用）。
    // completed/rejected 除合计外还按传输分列（http/tcp/local 各自的
    // completed/rejected；请求级格式错误计入 malformed 列），分列与合计
    // 满足：completed_total = http.completed + tcp.completed + local.completed
    //       + malformed.completed（rejected 同理）
    RpcServerStats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif // LIBMINI_RPC_H