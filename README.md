# libmini

Windows（MSVC 2017 / x86）下的 C++11 日常工具库：字符串、编码、摘要、配置、调度、RPC 等常用能力开箱即用。CMake + Conan 构建，静态/动态库均支持。

## 构建

依赖通过 Conan 安装（zlib、gtest、nlohmann_json、spdlog、cpp-httplib、pugixml 等），首次构建先执行：

```bat
build.bat
```

等价于：

```bat
conan\conan_install_all.cmd     :: 安装依赖（含 x86 静态 Debug 工具链）
cmake --preset conan-default
cmake --build --preset conan-debug
```

- 产物：`out/conan/x86-static-debug/lib/`（libmini.lib）与 `bin/`（测试可执行文件）
- 测试：在构建目录执行 `ctest -C Debug`，套件为 `libmini_test` / `rpc_test` / `utils_test` / `common_test` / `args_test`
- 动态库：`-DLIBMINI_BUILD_SHARED=ON`（同时定义 `LIBMINI_EXPORTS` 导出符号）。
  导出面 = 公开头里标注 `LIBMINI_API` 的符号（宏定义见 `utils/export.h`，
  新增公开 API 必须标注，否则 DLL 不导出、消费者链接失败）
- C++ 标准为 C++11（MSVC 2017 兼容），源码统一 `/utf-8` 编译
- CI：`.github/workflows/ci.yml` —— 7 个变体 job：三平台 Release 基线、
  Debug、Shared（DLL/SO 导出面）、warnings-strict（`-Wall -Wextra -Werror`），
  全部走 conan + Ninja 构建、CTest 全套、安装后 `find_package` 冒烟
  （`ci/smoke_consumer`）；push/PR 到 main 时自动运行
- 编译器告警默认 `-Wall`/`-Wextra`（MSVC `/W4`）可见；
  `-DLIBMINI_WARNINGS_AS_ERRORS=ON` 升格为错误

## 接入方式

```cpp
#include "libmini.h"   // 一次引入全部模块；宏 LIBMINI_STATIC/LIBMINI_API 由 CMake 定义
```

可运行的完整示例见 `example/demo_libmini.cpp`（构建后为 `example`，即 `bin/Debug/example.exe`）：

```bat
example --list               # 列出 18 个演示节
example                      # 全部运行
example --only rpc,sqlite    # 只看指定节
```

演示覆盖全部模块，其中 RPC 节会本地起服务端并演示重试/过载保护回环，入口参数本身就是用 `Args` 解析的。

## 安装与分发（find_package）

库内置 CMake 包导出与 CPack 规则，下游项目用标准方式消费：

```bat
:: 从构建目录安装到任意 prefix（只装 runtime + devel 组件，不含 example/test）
cmake --install out/conan/x86-static-debug --config Debug --prefix C:/libs/libmini

:: 或直接打分发包（ZIP/TGZ），文件名含版本与架构：
cd out/conan/x86-static-debug && cpack -C Debug
::   产物 libmini-1.0.0-Windows-x86.zip / .tar.gz
```

下游 CMakeLists 的最小接入：

```cmake
find_package(libmini REQUIRED CONFIG)      # 六个第三方依赖自动 find_dependency
target_link_libraries(app PRIVATE libmini::libmini)

# MSVC 下需与 libmini 分发配置一致的两项设置：
#   add_compile_options(/utf-8)                      头文件含 UTF-8 中文注释
#   set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  静态包按 MT 运行库分发（否则 LNK2038）
```

包内容：`include/`（伞头文件 + utils/ 全部模块头）、`lib/libmini.lib`、`lib/cmake/libmini/`（Config/ConfigVersion/Targets 三件套）。依赖解析优先走 conan/vcpkg 的 Config 文件（`CMAKE_FIND_PACKAGE_PREFER_CONFIG`）；包版本校验含位数（x86 包不会被 64 位工程误链），版本策略 SameMajorVersion。已用独立消费者工程实测：find_package → 编译链接 → 运行（string/json/uuid/rpc/sqlite 五模块调用）全链路通过（见 `out/consumer_test/`）。

## 模块清单

| 模块 | 头文件 | 内容 |
|---|---|---|
| string_utils | `utils/string_utils.h` | split / trim / replace / to_upper / to_lower |
| string_algo | `utils/string_algo.h` | starts_with / ends_with / iequals / replace_all / split_string / join |
| lexical_cast | `utils/lexical_cast.h` | 字符串↔数值转换（严格模式，失败抛 bad_lexical_cast） |
| time_utils | `utils/time_utils.h` | 时间戳、格式化 + 日历运算（月份加减/星期/月末/日期串互转） |
| stopwatch | `utils/stopwatch.h` | 高精度计时（pause/resume/restart） |
| file_utils | `utils/file_utils.h` | 读/写/追加文件、原子写（write_file_atomic：temp+fsync+rename，崩溃不截断）、流式文件摘要 sha256_file_hex/md5_file_hex、目录创建（单级/递归）、文件与目录树复制/移动、递归删除、目录详单（类型/大小/mtime）、时间戳、临时路径（W 版 API，UTF-8 中文路径无码页问题） |
| path_utils | `utils/path_utils.h` | path_join（多段）/dirname/basename/extension/stem、normalize（解析 ./.. 与盘符/UNC）、绝对化、parent_path、分隔符转换、路径等价比较 |
| thread_utils | `utils/thread_utils.h` | 线程池（submit 返回 future、wait_idle 排空、pending_tasks 队列深度）+ BlockingQueue 多生产者多消费者阻塞队列（容量上限/close 语义）+ CountdownLatch 倒计时门闩 |
| json_utils | `utils/json_utils.h` | 基于 nlohmann 的解析/序列化与转义 |
| xml_utils | `utils/xml_utils.h` | 基于 pugixml 的 XML 解析/序列化 + SimpleXmlNode 轻量结构 |
| serialization | `utils/serialization.h` | 基于 nlohmann 的通用 JSON 序列化 + XML 树映射序列化 |
| rpc | `utils/rpc.h` | JSON RPC（HTTP / Windows 命名管道 / POSIX UDS / 裸 TCP 帧四种传输，一套 API）：客户端连接池/重试/抖动/日志，服务端过载保护（立即拒绝或排队背压）/延迟分位/队列监控/spdlog 日志 |
| uuid | `utils/uuid.h` | RFC 4122 v4 生成与解析 |
| crc | `utils/crc.h` | CRC-32（zlib）/ CRC-16 Modbus / CRC-64 XZ / Adler-32，均支持增量计算 |
| encoding | `utils/encoding.h` | Base64 / Hex / URL 编解码 |
| scope_guard | `utils/scope_guard.h` | RAII 作用域守卫（dismiss / 可移动） |
| optional | `utils/optional.h` | C++11 版 optional（value_or / emplace） |
| random_utils | `utils/random_utils.h` | 随机整数/浮点/字符串/挑选 |
| env | `utils/env.h` | 环境变量读写与 `%VAR%` 展开（W 版 API，UTF-8） |
| digest | `utils/digest.h` | MD5 / SHA-256（增量计算） |
| ini_config | `utils/ini_config.h` | INI 读写：注释/引号/类型化取值/往返保存 |
| gzip | `utils/gzip.h` | gzip 压缩解压（返回 optional） |
| async | `utils/async.h` | 延时/周期任务调度器 + 令牌桶限流器 |
| args | `utils/args.h` | 命令行解析：--key=value、flag、位置参数、自动 usage |
| file_lock | `utils/file_lock.h` | 跨进程文件锁：独占/共享（读多写少）双模式，try_lock / 超时等待 / RAII Guard |
| dir_watcher | `utils/dir_watcher.h` | 目录变化监听（创建/修改/删除/重命名，支持子树），ReadDirectoryChangesW |
| win_service | `utils/win_service.h` | Windows 服务封装：ServiceControl（安装/卸载/启停/查询）+ ServiceApp（一体化入口 + PAUSE/CONTINUE 支持） |
| hmac | `utils/hmac.h` | HMAC-SHA256 / HMAC-MD5（RFC 2104，增量与一次性 API，云服务签名请求） |
| process | `utils/process.h` | 子进程执行：捕获 stdout/退出码、超时强杀、stdin 输入、UTF-8 命令行（CreateProcessW / fork+exec） |
| lru_cache | `utils/lru_cache.h` | 容量上限 LRU 缓存：get_or_compute（防击穿）、命中率统计、线程安全 |
| base32 | `utils/base32.h` | RFC 4648 Base32 编解码（宽松/严格模式，容空白字符） |
| console | `utils/console.h` | 控制台退出信号封装：Ctrl+C/Ctrl+Break/SIGTERM 触发回调 + stop_requested 轮询，前台与服务模式共用清理逻辑 |
| tcp | `utils/tcp.h` | 裸 TCP 长连接：帧协议（免粘包）、心跳保活、大帧完整性、服务端多连接/广播，零第三方依赖 |
| retry | `utils/retry.h` | poll_until 指数退避轮询（抖动防风暴、deadline 变体） |
| object_pool | `utils/object_pool.h` | 线程安全对象池：RAII Lease 借出归还、工厂创建、归还重置钩子 |
| zip | `utils/zip.h` | ZIP 包读写（zlib deflate/store，UTF-8 文件名，CRC 校验），零新增依赖 |
| aes_gcm | `utils/aes_gcm.h` | AES-256-GCM 认证加密（Windows CNG / OpenSSL EVP），seal/open 落盘格式 |
| sqlite | `utils/sqlite.h` | SQLite 封装：参数绑定（索引/命名）、事务 RAII、行遍历、带类型值读取，错误不抛异常 |
| system_info | `utils/system_info.h` | 主机名/PID/可执行文件路径/CPU 数/物理内存/磁盘容量与剩余 |
| http_client | `utils/http_client.h` | HttpClient：GET/POST/PUT/DELETE/通用方法、query 编码拼装、默认头、超时、重定向；headers 键统一小写；status=0 表示传输层错误 |
| log_facade | `utils/log_facade.h` | LogFacade：一行初始化 spdlog（控制台+滚动文件、级别、格式、可选异步），运行期调级，幂等 init/shutdown |
| config_facade | `utils/config_facade.h` | 分层配置门面：默认值 → 文件（JSON/INI 按扩展名）→ 环境变量三层合并，键路径取值（get_int/get_bool/...），source_of 查来源 |
| net_addr | `utils/net_addr.h` | socket 地址工具：端点解析（host:port / 纯端口 / IPv6 括号）、域名解析（IPv4 优先）、IPv4 格式化往返；RPC/TCP 统一使用 |
| timer_wheel | `utils/timer_wheel.h` | 层级时间轮：海量定时器 O(1) 添加/取消（单次/周期），固定节拍，适合万级连接超时管理；少量精准任务用 async 的 AsyncScheduler |

## 用法示例

### 字符串

```cpp
using namespace libmini;
std::vector<std::string> parts = split_string("a::b::c", "::");
std::string upper = to_upper("abc");
EXPECT_TRUE(starts_with("hello", "he"));
EXPECT_EQ(replace_all("a-b-c", "-", "+"), "a+b+c");
EXPECT_EQ(join(parts, "|"), "a|b|c");

int n = lexical_cast<int>("42");                 // 失败抛 bad_lexical_cast
int safe = lexical_cast_or<int>("x", -1);        // 失败返回默认值
```

### 时间与计时

```cpp
long long ts = current_timestamp_ms();
Stopwatch sw;
do_work();
std::cout << "cost " << sw.elapsed_ms() << "ms";   // elapsed_us/ns/seconds/elapsed_string
sw.pause();  sw.resume();  sw.restart();
```

### 文件与路径

```cpp
if (file_exists("a.txt")) std::string data = read_file("a.txt");
write_file("a.txt", "hello");
append_file("a.txt", "world");
remove_file("a.txt");

// 目录：递归创建、详单（类型/大小/修改时间，按名称排序）
make_directories("logs/2026/09");
std::vector<DirEntry> entries = list_directory_detailed("logs");
// entries[i].name/.path/.kind(File|Directory|Symlink)/.size/.mtime_ms

// 复制 / 移动 / 递归删除
copy_file("a.txt", "b.txt");
copy_tree("src_dir", "dst_dir");            // 递归复制目录树
move_path("old.txt", "new.txt");            // 优先原子 rename，跨盘符回退复制+删除
remove_tree("dst_dir");                     // 递归删除（也可用于文件）

// 路径处理（纯字符串操作）
std::string full = path_join("logs", "2026", "app.log"); // 多段拼接
std::string dir = dirname(full), name = basename(full), ext = extension(full);
std::string no_ext = stem("archive.tar.gz");              // "archive.tar"
replace_extension("report.txt", ".md");                   // "report.md"
normalize_path("a/b/../c/./d");                           // "a/c/d"（解析 ./..，不越过盘符根）
path_is_absolute("C:/x");                                 // 盘符/UNC/POSIX 根均识别
path_absolute("cfg.json", "D:/app");                      // "D:/app/cfg.json"
parent_path("a/b/c.txt");                                 // "a/b"（无父级返回 ""）
path_equivalent("A\\B\\c.txt", "a/b/c.TXT");              // Windows 下分隔符与大小写不敏感

// 临时文件
std::string tmp = temp_directory_path();
std::string p = unique_temp_path("myapp_");               // 唯一命名，避免碰撞

// 跨进程文件锁：单实例应用 / 保护共享资源
FileLockGuard guard("myapp.lock", /*try=*/true, 0);
if (guard.holds_lock()) {
    run_main_instance();   // 析构自动释放；lock_for(ms) 可带超时等待
}

// 目录监听：配置热更新 / 文件同步
DirWatcher watcher;
watcher.set_callback([](const WatchNotification& n) {
    // n.event: Created/Modified/Removed/RenamedOld/RenamedNew
    // n.name: 相对被监听目录的路径；n.dir: 被监听目录；n.is_dir
    if (n.event == WatchEvent::Modified) reload(n.dir + "/" + n.name);
});
watcher.start("config", /*watch_subtree=*/true);
// ... watcher.stop(); 之后可再次 start()
```

### Windows 服务

```cpp
// main() 里一段代码，同一个 exe 兼容服务模式、管理命令与前台调试：
int main(int argc, char* argv[]) {
    libmini::ServiceApp app("myapp", "My Application Service");
    app.set_description("示例服务");
    app.set_run_callback([](const std::atomic<bool>& stop) {
        while (!stop.load()) { do_work(); libmini::sleep_for_ms(500); }
        return 0;   // 返回值成为服务退出码
    });
    return app.run(argc, argv);
}

// 管理命令（管理员权限）：
//   myapp.exe install    安装（bin_path 自动指向本 exe，AutoStart）
//   myapp.exe uninstall  卸载（运行中会先停止，幂等）
//   myapp.exe start/stop 启停控制
//   myapp.exe            无参数 = console 调试模式（前台跑回调，Ctrl+C 触发 stop）
//
// 也可以直接用 ServiceControl 管理任意服务：
ServiceControl::install(desc);                       // ServiceInstallDesc 描述
auto st = ServiceControl::query("myapp");            // state/pid/exit_code/accepted_stop
ServiceControl::set_start_type("myapp", ServiceStartType::DemandStart);
ServiceControl::wait_for_state("myapp", ServiceState::Stopped, 10000);
```

// 路径统一按 UTF-8 处理：中文路径（如 "配置/中文文件名.txt"）在任意代码页下均可正常读写
```

### 线程池

```cpp
ThreadPool pool(4);
auto fut = pool.submit([](int a, int b) { return a + b; }, 1, 2);
int sum = fut.get();
```

### JSON

```cpp
JsonValue v = parse_json(R"({"name":"x","n":1})");
std::string compact = to_json_string(v);
std::string norm = parse_json_simple("{ \"a\" : 1 }");      // {"a":1}，非法输入返回 ""
std::string lit = to_json_string_simple("say \"hi\"");       // "say \"hi\""，合法 JSON 字面量
std::string manual = std::string("{\"msg\":\"") + escape_json_string(user_input) + "\"}";
```### RPC

> RPC 选型文档两份：
> - [docs/rpc_transport_guide.md](docs/rpc_transport_guide.md)——传输选型与连接复用（三传输 QPS、连接池 vs 流水线、决策树）
> - [docs/rpc_concurrency_guide.md](docs/rpc_concurrency_guide.md)——并发模型选型（同步 / 异步回调 / 异步 future / 流水线，example 实测数据）
> - [docs/config_practices.md](docs/config_practices.md)——服务配置实践（键命名约定、分层覆盖矩阵、排错方法）

```cpp
// 服务端：四种传输任选（构造时确定，其余 API 完全一致）
RpcServer server(8080);                              // HTTP over TCP
// RpcServer("\\\\.\\pipe\\myrpc");                    // Windows 命名管道
// RpcServer("/tmp/myapp.rpc");                      // POSIX Unix 域套接字
// RpcServer(RpcTransport::Tcp, "0.0.0.0:9000");     // 裸 TCP 帧协议（端口 0 自动分配）
server.register_method("add", [](const std::string& params) {
    JsonValue p = parse_json(params);
    return to_json_string(JsonValue{{"sum", p["a"].get<int>() + p["b"].get<int>()}});
});
server.set_max_in_flight(64);            // 并发处理上限（超限返回 429 + 自定义文本）
server.set_queue_warn_threshold(32);     // 队列积压告警
server.set_logger(spdlog::default_logger().get());
server.run();                            // 或 start_background() + wait_until_ready() + stop()

// 过载保护可选用两种模式（默认 RejectImmediate）：
//   RejectImmediate：超限立即 429，调用方退避重试（低延迟，配合客户端重试最佳）
//   WaitInQueue：超限请求在队列中排队等槽位（公平 FIFO），超时才 429——
//                削峰不丢弃，适合突发流量场景
server.set_overload_mode(RpcOverloadMode::WaitInQueue);
server.set_queue_wait_ms(5000);          // 排队等待上限
server.set_retry_after_seconds(2);       // 429 响应的 Retry-After（0 = 不发送）

// 从分层配置门面批量应用启动配置（环境变量 > 文件 > 代码默认值）：
//   ConfigFacade cfg;
//   cfg.set_default("rpc.max_in_flight", "2");
//   cfg.load_file("svc.json");            // {"rpc": {"max_in_flight": 3}}
//   cfg.set_env_prefix("SVC_");           // SVC_RPC_MAX_IN_FLIGHT=4
//   cfg.refresh_env();
//   server.apply_config(cfg, "rpc.");     // 未出现的键保持当前值，未知键忽略
// 可配键：port / host / worker_threads / max_in_flight / queue_wait_ms /
//         drain_timeout_ms / retry_after_seconds / queue_warn_threshold /
//         overload_message / overload_mode（"reject"|"wait"）

// 服务端延迟分位（对数分桶直方图，处理耗时 O(log) 记录、内存恒定）
RpcLatencyStats ls = server.latency_stats({99.9});   // 可追加自定义分位
// ls.p50_ms / p90_ms / p95_ms / p99_ms、min/max/mean、sample_count
// 按传输过滤分列（与 stats() 的分列口径一致；无样本传输分位为 -1）
RpcLatencyStats tcp_ls = server.latency_stats(
    {}, RpcTransportFilter::Tcp);     // All / Http / Tcp / Local

// stats()：completed_total/rejected_total 合计 + 按传输分列（单传输服务器
// 只有自己那列非零；分列之和 = 合计；请求级格式错误计入 malformed 列）
RpcServerStats st = server.stats();
// st.http.completed / st.tcp.rejected / st.local.completed / st.malformed.completed

// 优雅停机（三传输一致）：stop() 后——
//   1. 置停机标志：新请求立即 429 "server shutting down"（带 Retry-After）；
//   2. 排空窗口内等已接收的请求（含排队者/流水线在途）处理完；
//      窗口用尽才放弃剩余排队者（HTTP）或超时强断连接（Tcp/本地）；
//   3. 排水完成后再关闭监听与连接
server.set_drain_timeout_ms(5000);       // 默认 3000；0 = 立即放弃（旧行为）

// 客户端衔接：滚动重启场景下，429/断连由内置重试自动接住——
// 429 按 Retry-After 退避，断连按指数退避重连，连接新实例后继续服务。
// （服务器排水期不回包：响应只会送达活着的服务器，断连信号即重试信号）

// 客户端：重试、指数退避、抖动、Retry-After、请求日志
RpcClient client("127.0.0.1", 8080);
client.set_timeout_ms(3000);
client.set_max_retries(3);               // 429/连接失败自动重试，退避 100→200→400ms
client.set_retry_jitter(true);           // 相等抖动，防重试风暴
client.set_logger(logger.get());         // 每次重试记录原因与等待时长
std::string reply = client.call("add", R"({"a":1,"b":2})");
if (reply.empty()) {
    // RpcError 错误码 + 人类可读描述（含服务器自定义过载文本）
    std::cout << "rpc failed: " << client.last_error_message() << "\n";
}
double cost = client.last_call_elapsed_ms();  // 本次 call 总耗时（含重试等待）

// 本地传输：同协议跑在管道/UDS 上，重试、过载语义（429 ↔ status:"overloaded"）、
// 统计与直方图与 HTTP 完全一致；客户端可用 transport()/endpoint() 查询传输与端点
RpcClient pipe_client("\\\\.\\pipe\\myrpc");     // UDS 路径 "/tmp/myapp.rpc" 自动识别
std::string r2 = pipe_client.call("add", R"({"a":1,"b":2})");

// Tcp 传输：帧协议 over 裸 TCP，比 HTTP 少协议头开销，适合内网高频调用；
// 语义与 HTTP/本地传输完全一致（重试/退避/429/统计全部复用）
RpcServer tcp_srv(RpcTransport::Tcp, "0.0.0.0:9000");  // 端口 0 自动分配，port() 取真实值
RpcClient tcp_client(RpcTransport::Tcp, "10.0.0.5:9000");

// 连接池（LocalPipe/Tcp 传输默认开启）：复用连接避免每次调用重连，
// 并发上限前置到客户端（超上限的调用排队等归还）；失效连接自动重建，调用方无感知
// HTTP 传输不受影响（httplib 内置 keep-alive）
tcp_client.set_connection_pool_max(8);    // 最大连接数 = 并发上限，0 = 禁用（一调用一连接）
tcp_client.set_connection_pool_idle_ms(30000);  // 空闲连接回收时间
const RpcClientPoolStats ps = tcp_client.pool_stats();
// ps.open/idle/busy_connections + created/reused/closed_total
// 实测（Tcp，300 次顺序调用）：池关 422 QPS → 池开 2439 QPS（5.8x）

// 请求流水线（LocalPipe/Tcp）：单连接并发多个在途请求，响应按请求 id
// 匹配。线程数远超连接数时（handler 有处理耗时），并发上限从「连接数」
// 解耦为「在途上限」——32 线程共享 1 客户端、handler 耗时 5ms 场景：
// 池模式 514 QPS（并发=8 条连接）→ 流水线 2003 QPS（单连接在途 64）
tcp_client.set_pipeline_max_in_flight(64);  // 0 = 关闭（默认），走连接池

// 异步调用：在客户端内部线程池（默认 8 线程，即并发上限）上执行，与 call()
// 完全相同的请求路径（重试/退避/连接池/流水线/超时全部生效），不阻塞调用线程。
// future 形态：
std::future<std::string> f = client.call_async("add", R"({"a":1,"b":2})");
// ... 做其他事 ...
std::string r3 = f.get();  // 与 call() 相同语义（失败为空串）

// 回调形态：完成时在内部线程执行，错误码/文本是本次调用专属副本
//（并发调用互不覆盖，弥补同步 last_error() 共享状态的局限）
client.call_async("add", params, [](const std::string& result,
                                     RpcError err, const std::string& msg) {
    if (err != RpcError::OK) { handle_error(err, msg); }
});
// 线程安全：多线程并发 call_async、与 call() 混用均安全；
// 客户端析构会等所有在途异步调用结束，回调中捕获 this/引用不会悬空
//
// 异步执行器线程数可配置（默认 8，0 = 恢复默认；并发上限 = 执行器线程数）。
// 建议在首个 call_async 前设置；执行器已存在时设置会排空在途调用后换新池：
client.set_async_workers(16);   // 排水语义见 rpc.h；回调内调用不会自锁死
const std::size_t aw = client.async_workers();  // 读取当前线程数
//
// 形态选型（example 实测，Tcp handler 5ms、1600 请求）：两种形态
// 吞吐与同步多线程同档（~1.0x），提交开销都在 1µs 以内——需要结果
// 对象或集中收口选 future，轻量通知/流式处理选回调，直白易调试
// 选同步。三形态对比数据：example --only rpc

// Tcp 客户端异步连接（DNS/握手在内部线程，受 connect_timeout_ms 约束）
TcpClient tc;
auto cf = tc.async_connect("10.0.0.5", 9000);
if (cf.get()) { tc.send("hello"); }

// 流水线通道统计（pool_stats() 的 pipeline_* 字段，未开启时恒 0）
const RpcClientPoolStats pls = tcp_client.pool_stats();
// pls.pipeline_in_flight / pipeline_in_flight_peak   实时在途数与峰值
// pls.pipeline_reused_total                          单连接复用次数（承载的请求数）
// pls.pipeline_broken_total                          通道断线重建次数
// pls.pipeline_slot_wait_total                       槽位等待次数（在途上限竞争强度）
// pls.pipeline_slot_timeout_total                    等待槽位超时（上限偏小的信号）
```

### 编码与摘要

```cpp
std::string b64 = Base64::encode(raw);
Hex::decode("DEADBEEF", out);
std::string q = UrlEncode::encode("a b&c=中文");

std::string md5 = Md5::hex(data);                  // 32 位小写
Md5 m; m.update(chunk1); m.update(chunk2);         // 增量
std::string sha = Sha256::hex(data);

// HMAC：API 签名请求（另见 utils/hmac.h，含增量 API 与 HmacMd5）
std::string sig = HmacSha256::hex(secret_key, payload);

Crc32 crc; crc.update(data); std::uint32_t v = crc.value();

// 子进程：调用外部工具（另见 utils/process.h：stdin 输入 / 超时强杀）
ProcessResult r = run_process("git", {"status", "--short"});
if (r.exit_code == 0) { use(r.stdout_text); }

// LRU 缓存：带命中率统计（另见 utils/lru_cache.h）
LruCache<int, std::string> cache(100);
cache.put(1, "a");
std::string v1 = cache.get_or_compute(2, [] { return expensive(); });

// Base32（另见 utils/base32.h，decode 支持宽松/严格模式）
std::string b32 = Base32::encode("foobar");

// 控制台优雅退出（另见 utils/console.h）：前台程序 Ctrl+C 后收尾
ConsoleExit ce;
ce.set_handler([&] { flush_and_stop(); });   // 信号到达后在内部线程执行
while (!ce.stop_requested()) { serve_one(); }
```

### UUID

```cpp
Uuid id = Uuid::generate();
std::string text = id.to_string();        // f47ac10b-58cc-4372-a567-0e02b2c3d479
Uuid parsed; bool ok = Uuid::parse(text, parsed);
```

### optional / scope_guard

```cpp
optional<int> v = maybe_parse(s);
int n = v.value_or(-1);

FILE* f = fopen("a.txt", "r");
auto guard = make_scope_guard([f] { if (f) fclose(f); });
// 异常/提前 return 均保证 fclose；成功路径 guard.dismiss();
```

### 配置与环境

```cpp
IniConfig cfg;
cfg.load("app.ini");
int port = cfg.get_int("server", "port", 8080);      // 缺失用默认值
bool verbose = cfg.get_bool("server", "verbose");
cfg.set("server", "port", "9090");
cfg.save_to("app.ini");

std::string path = env_get("APP_HOME", "C:\\app");
std::string expanded = env_expand("%APP_HOME%\\logs");
```

### 压缩

```cpp
optional<std::string> z = gzip_compress(big_text, 9);     // 失败返回 nullopt
optional<std::string> raw = gzip_decompress(*z);          // 兼容 gzip/zlib 流
```

### SQLite（另见 utils/sqlite.h）

```cpp
SqliteDatabase db("app.db");                      // ":memory:" = 内存库；UTF-8 路径
if (!db.is_open()) { use(db.error_message()); }
db.exec("CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY, name TEXT, score REAL)");

// 事务 RAII：析构未提交自动回滚（异常路径安全）；嵌套 BEGIN 失败不抛异常
{
    SqliteTransaction tx(db);
    SqliteStatement ins(db, "INSERT INTO users(name, score) VALUES(?, ?)");
    for (int i = 0; i < 100; ++i) {
        ins.bind_text(1, "user" + std::to_string(i));   // bind 自动 reset，可直接换参重跑
        ins.bind_double(2, i * 0.5);
        ins.step();                                     // INSERT → StepDone
    }
    tx.commit();
}   // 不 commit 则回滚

// 行遍历（? 索引 1-based；列读取 0-based）
SqliteStatement st(db, "SELECT id, name, score FROM users WHERE score >= ?");
st.bind_double(1, 10.0);
while (st.step() == SqliteStatement::StepRow) {
    int id = st.column_int(0);
    std::string name = st.column_text(1);
}

// 命名参数 :name / 全量读取（类型保留）
SqliteStatement q(db, "SELECT * FROM users WHERE name = :name");
q.bind_text_by_name(":name", "user42");
auto rows = q.query_all();          // vector<vector<SqliteValue>>，type 区分 Null/Integer/Real/Text/Blob
db.set_busy_timeout_ms(2000);       // 多连接写冲突的等待上限
db.last_insert_rowid(); db.changes();
```

### 网络与重试

```cpp
// 裸 TCP 长连接（帧协议免粘包，见 utils/tcp.h）
TcpServer server;
server.set_on_message([&server](std::uint64_t conn, const std::string& msg) {
    server.send(conn, "echo:" + msg);       // 或 broadcast()
});
server.start("127.0.0.1", 9000);            // 端口 0 = 系统分配，用 port() 取实际值

TcpConfig cfg;
cfg.heartbeat_interval_ms = 30000;          // 空闲 30s 发 PING，服务端自动回 PONG
cfg.heartbeat_timeout_ms = 90000;           // 超时无入站帧判死
TcpClient client(cfg);
client.set_on_message([](const std::string& msg) { handle(msg); });
client.connect("127.0.0.1", 9000);
client.send(payload);                       // 单帧上限 max_frame_bytes（默认 16MB）

// 指数退避轮询（见 utils/retry.h）：等服务就绪/等文件出现
bool ok = poll_until([&] { return file_exists(flag_path); },
                     /*max_attempts=*/10, /*base_delay_ms=*/100);

// CRC 家族（均支持增量）
Crc16Modbus crc16; crc16.update(frame);     // Modbus RTU
std::uint64_t v64 = Crc64::compute(big_data);  // 归档校验
std::uint32_t ad = Adler32::compute(chunk);    // zlib 块校验

// 对象池：昂贵对象复用（RAII Lease）
ObjectPool<std::vector<char>> pool(8, [] { return std::make_unique<std::vector<char>>(1024); });
{
    auto buf = pool.acquire();              // 池空阻塞等待；try_acquire 非阻塞
    (*buf)->resize(100);
}                                           // 自动归还

// ZIP 压缩包（zlib 实现，UTF-8 文件名）
ZipWriter zw;
zw.add_file("doc.txt", text);               // deflate；ZipMethod::Store 直接存
const std::string zip_bytes = zw.finish();
ZipReader zr;
if (zr.open(zip_bytes)) {
    std::string back = zr.extract("doc.txt");  // CRC 校验，篡改返回空
}

// AES-256-GCM：本地敏感数据落盘（seal = 随机 nonce||密文||tag）
std::string sealed = Aes256Gcm::seal(key32, "db_password=s3cret");
std::string plain = Aes256Gcm::open(key32, sealed);  // 篡改返回空

// 日历运算
int dim = days_in_month(2024, 2);              // 29
std::int64_t d = date_from_string("2024-02-29");
std::int64_t next = add_months(d, 1);          // 2024-03-29（日号自动钳月末）
std::int64_t next_mon = next_weekday(d, 1);    // 下周一
```

### 调度与限流

```cpp
AsyncScheduler sched;
sched.run_after_ms(5000, reconnect);                // 延时任务
TaskHandle h = sched.run_every_ms(60000, [] {       // 周期任务，返回 false 自停
    refresh();
    return true;
});
h.cancel();                                         // 取消未执行/后续期次

RateLimiter limiter(10.0, 5);                       // 稳态 10/s，突发上限 5
if (limiter.try_acquire()) send_request();          // 非阻塞
limiter.acquire();                                  // 阻塞等待令牌
```

### 随机

```cpp
int dice = random_int(1, 6);
std::string token = random_string(16);              // 字母+数字
std::string hex_id = random_string(8, "0123456789abcdef");
```

### XML 与序列化

```cpp
const XmlDocument doc = parse_xml("<root><item id='1'>v</item></root>");  // 非法输入抛异常
std::string text = to_xml_string(doc);
SimpleXmlNode node = parse_xml_simple("<a x='1'>hi</a>");   // 非法输入返回空节点

// ---- JSON：任意 nlohmann 可转换的类型，自定义结构体一行宏 ----
struct Host { std::string name; int port; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Host, name, port)   // 放在类型同命名空间

std::string json = serialize_to_json(Host{"db", 5432});
Host h = deserialize_from_json<Host>(json);            // 非法输入抛 json::exception
Host h2 = deserialize_from_json_or<Host>("{broken", h); // 不抛版，失败返回 fallback
std::vector<int> nums = deserialize_from_json<std::vector<int> >("[1,2,3]");

// ---- XML：同一套类型映射为 XML（对象→子元素、数组→<item>）----
std::string xml = serialize_to_xml(h);                 // <value type="object">…
Host h3 = deserialize_from_xml<Host>(xml);             // 非法输入抛 std::runtime_error
Host h4 = deserialize_from_xml_or<Host>("<broken", h); // 不抛版
// std::string 特化：数字样文本（"0089"）往返不变形；
// JSON↔XML 原始互转另见 json_to_xml()/xml_to_json()
```

### 命令行解析

```cpp
int main(int argc, char** argv)
{
    libmini::Args args("demo", "1.0", "示例程序");
    args.add_option("host", "H", "服务器地址", std::string("127.0.0.1"));
    args.add_int("port", "p", "端口", 8080);
    args.add_flag("verbose", "v", "详细输出");
    args.add_positional("input", "输入文件");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 1;   // 错误/help 均已打印
    }

    std::string host = args.get_string("host");       // 默认值或 --host/-H 提供的值
    int port = args.get_int("port");                  // 严格校验，"80x" 直接报错
    bool verbose = args.has_flag("verbose");
    std::string in = args.positional("input");
}
```

支持 `--key=value`、`--key value`、`-k value`、`-kvalue`、`-k=value`；`--` 之后全部视为位置参数；内置 `-h/--help` 自动打印 usage。Windows 下用 `wmain` + `libmini::args_from_wmain(argc, argv)` 可避免中文参数乱码。

### 格式化辅助

```cpp
using namespace libmini;
format_bytes(1536);        // "1.5 KB"（1024 进制，自动去尾零）
format_duration_ms(65500); // "1m 05.5 s"
format_duration_ms(15);    // "15 ms"
```

### 线程原语

```cpp
using namespace libmini;
ThreadPool pool(4);
pool.submit([] { return 1 + 1; });
pool.wait_idle();          // 等全部已提交任务完成（不停止线程池）

BlockingQueue<Task> q(64); // 有界阻塞队列：满时 push 阻塞、空时 pop 阻塞
q.push(task);
Task t;
if (q.try_pop(t, 100)) { process(t); }
q.close();                 // 关闭：消费端取尽后 pop 返回 false 自然退出

CountdownLatch latch(3);   // 三件事都完成后放行所有等待者
latch.count_down();
latch.wait();
```

### HTTP 客户端

```cpp
using namespace libmini;
HttpClient c("http://api.example.com");
c.set_timeout_ms(3000);
c.set_default_header("Authorization", "Bearer ...");

HttpResponse r = c.get("/items", {{"page", "1"}});   // query 自动编码拼装
if (r.ok()) { use(r.body); }                          // 2xx；r.headers 键统一小写

HttpResponse p = c.post_json("/items", R"({"name":"x"})");
```

### 统一日志门面

```cpp
using namespace libmini;
LogFacade::Options o;
o.file_path = "logs/app.log";   // 自动建目录、10MB×5 滚动
o.level = LogLevel::Debug;
o.async_mode = true;            // 后台线程写，退出前须 LogFacade::shutdown()
LogFacade::init(o);

LogFacade::logger()->info("service started, pid={}", current_pid());
LogFacade::set_level(LogLevel::Warn);   // 运行期动态调级
LogFacade::shutdown();
```

### 系统信息

```cpp
using namespace libmini;
LOG_FMT("host={} pid={} cpu={} mem={}", hostname(), current_pid(),
        cpu_count(), format_bytes(total_physical_memory()));
sha256_file_hex("download.zip");   // 大文件流式摘要，校验下载完整性
write_file_atomic("config.json", new_json);  // 崩溃安全的配置落盘
```

### 分层配置门面

```cpp
using namespace libmini;
ConfigFacade cfg;
cfg.set_default("server.port", "8080");          // ① 默认值层
cfg.load_file("config.json");                    // ② 文件层（.json/.ini 按扩展名）
cfg.set_env_prefix("MYAPP_");                    // ③ 环境变量层：MYAPP_SERVER_PORT
cfg.refresh_env();                               //    （'.'/'_' 归一化匹配）

const int port = cfg.get_int("server.port");     // env > file > default
cfg.source_of("server.port");                    // "env"/"file"/"default"
```

### socket 地址工具

```cpp
using namespace libmini;
std::string host; int port;
parse_endpoint("[::1]:8080", host, port);   // host="::1", port=8080
parse_endpoint_or_default("9000", host, port);  // host="0.0.0.0", port=9000

resolve_host("localhost", "");               // 地址列表（IPv4 优先）
ipv4_to_string(ipv4_from_string("10.0.0.7")); // 点分往返
```

### 时间轮

```cpp
using namespace libmini;
TimerWheel wheel(std::chrono::milliseconds(10));  // 10ms 节拍
TimerWheel::Handle h = wheel.add_ms(5000, on_conn_timeout);   // O(1) 添加
h.cancel();                                                    // O(1) 取消
wheel.add_periodic_ms(100, [&] -> bool { return keepalive(); });  // false 自停
wheel.wait_idle();                                             // 全部触发后返回
```

## 注意事项与已知限制

- **目标平台**：Windows / MSVC 2017 / x86。cpp-httplib 0.28 对 32 位 Windows 仅是“不再官方支持”警告，实测编译运行正常；如需彻底规避可锁定 0.18.x。
- `test/` 下另有一个独立的 Win32 图形调试程序 `swf_viewer`（SWF 环查找可视化），可用 `-DLIBMINI_BUILD_SWF_VIEWER=OFF` 关闭；它不属于单元测试。
