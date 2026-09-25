// 万级连接会话超时管理基准：轮询（wheel_liveness=false）vs TimerWheel 事件化。
// 场景：N 条空闲连接挂 10 秒（无业务数据），对比进程 CPU 时间增量。
//
//   tcp_bench [connections] [seconds]
//
// 预期：轮询模式 CPU 随连接数线性增长（每会话 10Hz select 唤醒）；
//       wheel 模式 CPU 几乎恒定（空闲连接零唤醒，轮任务每超时/2 tick 复查）。
//
// 说明：用 Testbench ServerConfig 时轮询模式靠 wait_readable 100ms 超时推进
// running 检查——空闲连接上这就是纯粹的 CPU 燃烧来源；wheel 模式 2s 才醒一次。

#include "libmini.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <sys/resource.h>
#endif

using namespace libmini;

namespace {

// Windows 进程 CPU 时间（内核态 + 用户态，毫秒）
std::uint64_t process_cpu_ms()
{
#ifdef _WIN32
    FILETIME create, exit, kernel, user;
    if (!::GetProcessTimes(::GetCurrentProcess(), &create, &exit, &kernel, &user)) {
        return 0;
    }
    auto to_ms = [](const FILETIME& ft) {
        return static_cast<std::uint64_t>(ft.dwHighDateTime) << 32 |
               ft.dwLowDateTime;  // 100ns 单位
    };
    return (to_ms(kernel) + to_ms(user)) / 10000;
#else
    ::rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::uint64_t>(ru.ru_utime.tv_sec) * 1000 +
           ru.ru_utime.tv_usec / 1000 +
           static_cast<std::uint64_t>(ru.ru_stime.tv_sec) * 1000 +
           ru.ru_stime.tv_usec / 1000;
#endif
}

struct BenchResult
{
    std::uint64_t cpu_ms = 0;
    std::size_t connections = 0;
};

// 起 1 个服务端 + N 个客户端（心跳开启但全部空闲），挂 duration 秒后收摊
BenchResult bench_idle_sessions(std::size_t n, int duration_s, bool wheel,
                                int ping_ms, int hb_timeout_ms)
{
    BenchResult result;
    TcpConfig scfg;
    scfg.wheel_liveness = wheel;
    scfg.heartbeat_interval_ms = ping_ms;
    scfg.heartbeat_timeout_ms = hb_timeout_ms;
    TcpServer server(scfg);
    server.set_on_message([](std::uint64_t, const std::string&) {});
    if (!server.start("127.0.0.1", 0)) {
        std::printf("server start failed\n");
        return result;
    }
    const std::uint16_t port = server.port();

    TcpConfig ccfg;
    ccfg.wheel_liveness = wheel;
    ccfg.heartbeat_interval_ms = ping_ms;
    ccfg.heartbeat_timeout_ms = hb_timeout_ms;
    ccfg.connect_timeout_ms = 3000;

    std::printf("  connecting %zu clients...\n", n);
    std::size_t ok = 0;
    {
        std::vector<std::unique_ptr<TcpClient>> clients;
        clients.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto c = std::unique_ptr<TcpClient>(new TcpClient(ccfg));
            if (c->connect("127.0.0.1", port)) {
                ++ok;
            }
            clients.push_back(std::move(c));
        }
        result.connections = ok;

        // CPU 只计空闲窗口：连接建立（1 万次 accept + 会话线程创建）
        // 是双方共同的固定成本，计入会淹没调度机制的差异
        std::printf("  idling %ds (wheel=%d)...\n", duration_s, wheel ? 1 : 0);
        const std::uint64_t cpu0 = process_cpu_ms();
        std::this_thread::sleep_for(std::chrono::seconds(duration_s));
        const std::uint64_t cpu1 = process_cpu_ms();
        result.cpu_ms = cpu1 - cpu0;
    }   // clients 析构：断开全部连接

    server.stop();
    return result;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::size_t n = argc > 1 ? static_cast<std::size_t>(std::atoi(argv[1])) : 1000;
    const int secs = argc > 2 ? std::atoi(argv[2]) : 10;
    // 心跳频率可调：PING 越稀疏，越能隔离「超时调度机制本身」的开销
    const int ping_ms = argc > 3 ? std::atoi(argv[3]) : 1000;
    const int hb_timeout_ms = argc > 4 ? std::atoi(argv[4]) : 5000;

    std::printf("=== TCP liveness benchmark: %zu idle connections, %ds ===\n", n, secs);
    std::printf("heartbeat: client PING every %dms, timeout %dms\n\n", ping_ms, hb_timeout_ms);

    const BenchResult wheel = bench_idle_sessions(n, secs, true, ping_ms, hb_timeout_ms);
    std::printf("  [wheel=true ] conns=%zu  cpu=%llu ms\n\n",
                wheel.connections, static_cast<unsigned long long>(wheel.cpu_ms));

    const BenchResult poll = bench_idle_sessions(n, secs, false, ping_ms, hb_timeout_ms);
    std::printf("  [wheel=false] conns=%zu  cpu=%llu ms\n\n",
                poll.connections, static_cast<unsigned long long>(poll.cpu_ms));

    if (poll.cpu_ms > 0) {
        std::printf("=== wheel CPU is %.2f%% of polling (%.1fx saving) ===\n",
                    poll.cpu_ms > 0 ? 100.0 * wheel.cpu_ms / poll.cpu_ms : 0.0,
                    poll.cpu_ms > 0 ? static_cast<double>(poll.cpu_ms) /
                                          static_cast<double>(wheel.cpu_ms + 1)
                                    : 0.0);
    }
    return 0;
}
