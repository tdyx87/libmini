#include "circuit_breaker.h"

#include "log_facade.h"

#include <chrono>
#include <mutex>

#include <spdlog/logger.h>

namespace libmini {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ---------------- 配置校验 ----------------

bool CircuitBreakerConfig::validate() const
{
    bool ok = true;
    spdlog::logger* log = LogFacade::logger();

    const auto warn = [&ok, log](bool bad, const char* what) {
        if (bad) {
            if (log) {
                log->warn("circuit breaker config invalid: {}", what);
            }
            ok = false;
        }
    };

    warn(failure_threshold <= 0, "failure_threshold must be positive");
    warn(open_duration_ms < 0, "open_duration_ms < 0");
    warn(half_open_max_calls <= 0, "half_open_max_calls must be positive");
    warn(success_threshold <= 0, "success_threshold must be positive");
    return ok;
}

// ============================ 实现 ============================

struct CircuitBreaker::Impl
{
    CircuitBreakerConfig config;

    mutable std::mutex mutex;
    CircuitState state = CircuitState::Closed;
    std::int64_t opened_at_ms = 0;      // 进入 Open 的时刻（冷却计时起点）
    int consecutive_failures = 0;       // Closed 下连续失败计数
    int half_open_in_flight = 0;        // HalfOpen 已放行未归结的探测数
    int half_open_successes = 0;        // HalfOpen 连续成功计数

    CircuitBreakerStats stats;          // 累计统计（快照字段除外）
    CircuitStateHandler on_state_change;

    // ---- 内部转移（须持 mutex）----
    void transition_locked(CircuitState to)
    {
        if (state == to) {
            return;
        }
        state = to;
        ++stats.state_changes;
        if (to == CircuitState::Open) {
            opened_at_ms = now_ms();
            half_open_in_flight = 0;
            half_open_successes = 0;
            consecutive_failures = 0;
        } else if (to == CircuitState::HalfOpen) {
            half_open_in_flight = 0;
            half_open_successes = 0;
            consecutive_failures = 0;
        } else {  // Closed
            consecutive_failures = 0;
            half_open_in_flight = 0;
            half_open_successes = 0;
        }
    }

    // Open 冷却到点 → HalfOpen（须持 mutex）。返回是否发生转移
    bool advance_time_locked()
    {
        if (state == CircuitState::Open &&
            now_ms() - opened_at_ms >= config.open_duration_ms) {
            transition_locked(CircuitState::HalfOpen);
            return true;
        }
        return false;
    }

    // 配置非法值兜底（阈值至少 1、冷却至少 0）
    int effective_failure_threshold() const
    {
        return config.failure_threshold > 0 ? config.failure_threshold : 1;
    }
    int effective_success_threshold() const
    {
        return config.success_threshold > 0 ? config.success_threshold : 1;
    }
    int effective_half_open_max() const
    {
        return config.half_open_max_calls > 0 ? config.half_open_max_calls : 1;
    }
};

CircuitBreaker::CircuitBreaker() : impl_(new Impl)
{
    impl_->config.validate();  // 只记日志（构造不拒绝），非法值走兜底
}

CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& config)
    : impl_(new Impl)
{
    impl_->config = config;
    impl_->config.validate();
}

CircuitBreaker::~CircuitBreaker()
{
    delete impl_;
}

bool CircuitBreaker::allow()
{
    CircuitStateHandler handler;
    bool fire = false;
    CircuitState from = CircuitState::Closed;
    CircuitState to = CircuitState::Closed;
    bool allowed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const CircuitState before = impl_->state;
        if (impl_->advance_time_locked() && impl_->on_state_change) {
            fire = true;
            from = before;
            to = impl_->state;
            handler = impl_->on_state_change;
        }
        switch (impl_->state) {
        case CircuitState::Closed:
            allowed = true;
            break;
        case CircuitState::Open:
            allowed = false;
            break;
        case CircuitState::HalfOpen:
            if (impl_->half_open_in_flight <
                impl_->effective_half_open_max()) {
                ++impl_->half_open_in_flight;
                allowed = true;
            } else {
                allowed = false;  // 探测名额已满
            }
            break;
        }
        if (allowed) {
            ++impl_->stats.allowed;
        } else {
            ++impl_->stats.rejected;
        }
    }
    if (fire && handler) {
        handler(from, to);  // 锁外：回调可重入 allow()/state()
    }
    return allowed;
}

void CircuitBreaker::record_success()
{
    CircuitStateHandler handler;
    bool fire = false;
    CircuitState from = CircuitState::Closed;
    CircuitState to = CircuitState::Closed;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->stats.successes;
        switch (impl_->state) {
        case CircuitState::Closed:
            impl_->consecutive_failures = 0;  // 成功清零连续失败
            break;
        case CircuitState::Open:
            break;  // 迟到的旧调用结果，不影响冷却
        case CircuitState::HalfOpen:
            if (impl_->half_open_in_flight > 0) {
                --impl_->half_open_in_flight;
            }
            ++impl_->half_open_successes;
            if (impl_->half_open_successes >=
                impl_->effective_success_threshold()) {
                from = impl_->state;
                impl_->transition_locked(CircuitState::Closed);
                to = CircuitState::Closed;
                fire = impl_->on_state_change != nullptr;
                if (fire) {
                    handler = impl_->on_state_change;
                }
            }
            break;
        }
    }
    if (fire && handler) {
        handler(from, to);
    }
}

void CircuitBreaker::record_failure()
{
    CircuitStateHandler handler;
    bool fire = false;
    CircuitState from = CircuitState::Closed;
    CircuitState to = CircuitState::Closed;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->stats.failures;
        switch (impl_->state) {
        case CircuitState::Closed:
            ++impl_->consecutive_failures;
            if (impl_->consecutive_failures >=
                impl_->effective_failure_threshold()) {
                from = impl_->state;
                impl_->transition_locked(CircuitState::Open);
                to = CircuitState::Open;
                fire = impl_->on_state_change != nullptr;
                if (fire) {
                    handler = impl_->on_state_change;
                }
            }
            break;
        case CircuitState::Open:
            break;  // 已熔断，重复失败不重置冷却计时
        case CircuitState::HalfOpen:
            // 探测失败：立即回 Open 重新冷却
            if (impl_->half_open_in_flight > 0) {
                --impl_->half_open_in_flight;
            }
            from = impl_->state;
            impl_->transition_locked(CircuitState::Open);
            to = CircuitState::Open;
            fire = impl_->on_state_change != nullptr;
            if (fire) {
                handler = impl_->on_state_change;
            }
            break;
        }
    }
    if (fire && handler) {
        handler(from, to);
    }
}

CircuitOutcome CircuitBreaker::execute(const std::function<bool()>& call)
{
    if (!allow()) {
        return CircuitOutcome::Rejected;
    }
    bool ok = false;
    try {
        ok = call();
    } catch (...) {
        record_failure();  // 异常同样算失败，然后原样上抛
        throw;
    }
    if (ok) {
        record_success();
        return CircuitOutcome::Success;
    }
    record_failure();
    return CircuitOutcome::Failure;
}

CircuitState CircuitBreaker::state() const
{
    CircuitStateHandler handler;
    bool fire = false;
    CircuitState from = CircuitState::Closed;
    CircuitState to = CircuitState::Closed;
    CircuitState result;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const CircuitState before = impl_->state;
        if (impl_->advance_time_locked() && impl_->on_state_change) {
            fire = true;
            from = before;
            to = impl_->state;
            handler = impl_->on_state_change;
        }
        result = impl_->state;
    }
    if (fire && handler) {
        handler(from, to);
    }
    return result;
}

void CircuitBreaker::reset()
{
    CircuitStateHandler handler;
    bool fire = false;
    CircuitState from = CircuitState::Closed;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->state != CircuitState::Closed) {
            from = impl_->state;
            impl_->transition_locked(CircuitState::Closed);
            fire = impl_->on_state_change != nullptr;
            if (fire) {
                handler = impl_->on_state_change;
            }
        } else {
            impl_->consecutive_failures = 0;
            impl_->half_open_in_flight = 0;
            impl_->half_open_successes = 0;
        }
    }
    if (fire && handler) {
        handler(from, CircuitState::Closed);
    }
}

void CircuitBreaker::reset_stats()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stats = CircuitBreakerStats();
}

CircuitBreakerStats CircuitBreaker::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CircuitBreakerStats snap = impl_->stats;
    snap.consecutive_failures = impl_->consecutive_failures;
    snap.half_open_successes = impl_->half_open_successes;
    return snap;
}

void CircuitBreaker::set_on_state_change(CircuitStateHandler handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->on_state_change = std::move(handler);
}

const CircuitBreakerConfig& CircuitBreaker::config() const
{
    return impl_->config;
}

}  // namespace libmini
