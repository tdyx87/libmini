#ifndef LIBMINI_CIRCUIT_BREAKER_H
#define LIBMINI_CIRCUIT_BREAKER_H

#include <cstdint>
#include <functional>

#include "export.h"

namespace libmini {

// 熔断器三态：
//   Closed   —— 正常放行，统计连续失败；
//   Open     —— 已熔断，allow() 立即拒绝（快速失败，保护下游/本端线程池）；
//   HalfOpen —— 冷却到点后放行少量探测调用试探恢复：
//               连续成功达 success_threshold → 回 Closed；
//               任一探测失败 → 立即回 Open 并重新计时。
enum class CircuitState {
    Closed,
    Open,
    HalfOpen,
};

// execute() 的结果（Rejected = 被熔断拒绝，call 未执行）
enum class CircuitOutcome {
    Success,
    Failure,
    Rejected,
};

// 状态转移回调（熔断器内部线程语义：在状态变更的调用线程上同步触发，
// 回调内可安全调用 allow()/state()，但不要长时间阻塞）
typedef std::function<void(CircuitState from, CircuitState to)>
    CircuitStateHandler;

struct LIBMINI_API CircuitBreakerConfig {
    int failure_threshold = 5;      // Closed 下连续失败达到此数 → Open
    int open_duration_ms = 30000;   // Open 冷却时长，到点 → HalfOpen（探测）
    int half_open_max_calls = 1;    // HalfOpen 同时放行的探测调用数
    int success_threshold = 2;      // HalfOpen 连续成功达到此数 → Closed

    // 非法项逐条经 LogFacade 记 warn，任一命中返回 false：
    //   - failure_threshold / half_open_max_calls / success_threshold <= 0；
    //   - open_duration_ms < 0
    // CircuitBreaker 构造时也会自动调用一次（只记日志，不拒绝构造）；
    // 非法值运行期按最小值 1 / 0 兜底，不会出现除零或负数等待
    bool validate() const;
};

// 累计统计（自构造 / reset_stats 起；reset() 不清零本结构）
struct CircuitBreakerStats
{
    std::uint64_t allowed = 0;    // allow() 放行次数
    std::uint64_t rejected = 0;   // allow() 拒绝次数
    std::uint64_t successes = 0;  // record_success 次数
    std::uint64_t failures = 0;   // record_failure 次数
    std::uint64_t state_changes = 0;  // 状态转移次数
    int consecutive_failures = 0;     // 当前 Closed 连续失败数（快照）
    int half_open_successes = 0;      // 当前 HalfOpen 连续成功数（快照）
};

// 熔断器（线程安全，内部一把互斥锁；对齐 resilience4j 经典三态模型）。
//
// 典型用法——包住弱依赖调用：
//
//   CircuitBreaker breaker;                        // 5 次连续失败熔断 30s
//   if (breaker.allow()) {                         // Open 时立即 false
//       bool ok = call_weak_dependency();
//       ok ? breaker.record_success() : breaker.record_failure();
//   }                                              // 拒绝时走降级/快速失败
//
// 或一体化（自动记录，含异常路径）：
//
//   CircuitOutcome r = breaker.execute([&] { return ping_downstream(); });
//   if (r == CircuitOutcome::Rejected) { use_fallback(); }
//
// 语义：
//   - allow() 与 record_success()/record_failure() 必须成对（放行才记录）；
//     execute() 负责成对——call 抛异常也记为 Failure 后原样上抛；
//   - HalfOpen 探测名额 half_open_max_calls 控制并发探测数，名额用完
//     的调用一律拒绝，防止冷却瞬间打爆刚恢复的服务；
//   - 状态转移回调在触发转移的线程上同步执行（锁外），回调内重入
//     allow()/state()/stats() 安全；
//   - 时间源为 steady_clock，不受系统时钟调整影响。
class LIBMINI_API CircuitBreaker
{
public:
    CircuitBreaker();
    explicit CircuitBreaker(const CircuitBreakerConfig& config);
    ~CircuitBreaker();

    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;

    // 调用前放行检查（时间推进也在此发生：Open 冷却到点自动转 HalfOpen）。
    // true = 应执行调用，随后必须记录一次结果；false = 熔断中，快速失败
    bool allow();

    // 记录一次放行调用的成功结果
    void record_success();

    // 记录一次放行调用的失败结果（Closed 达阈值 → Open；HalfOpen → 立即回 Open）
    void record_failure();

    // 一体化执行：先 allow()，再调 call 并按其返回值记录。
    //   Success   —— call 返回 true（已记录成功）
    //   Failure   —— call 返回 false 或抛异常（已记录失败；异常原样上抛）
    //   Rejected  —— 熔断拒绝，call 未执行
    CircuitOutcome execute(const std::function<bool()>& call);

    // 当前状态（含惰性时间推进：Open 冷却到点的查询即转 HalfOpen）
    CircuitState state() const;

    // 复位状态机到 Closed（清零连续失败/探测计数并触发转移回调；
    // 累计统计保留——与 reset_stats() 分开）
    void reset();

    // 清零累计统计（allowed/rejected/.../state_changes），状态不变
    void reset_stats();

    // 累计统计快照
    CircuitBreakerStats stats() const;

    // 状态转移回调（可运行期随时替换；空回调 = 不通知）
    void set_on_state_change(CircuitStateHandler handler);

    const CircuitBreakerConfig& config() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_CIRCUIT_BREAKER_H
