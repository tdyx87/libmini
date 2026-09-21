#ifndef LIBMINI_RETRY_H
#define LIBMINI_RETRY_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <thread>

#include "libmini.h"

namespace libmini {

namespace retry_detail {

// 抖动随机源：自包含，不依赖 random_utils（避免与 libmini.h 循环 include）
inline double uniform(double lo, double hi)
{
    thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(gen);
}

}  // namespace retry_detail

// 带指数退避 + 抖动的轮询重试。典型场景：等服务就绪、等文件出现、弱依赖重试。
//
//   bool ok = poll_until([&] { return file_exists(flag_path); },
//                        /*max_attempts=*/10, /*base_delay_ms=*/100);
//
// 语义：
//   - attempt() 返回 true 立即结束（成功）；false 等一个退避间隔后重试；
//   - 最多尝试 max_attempts 次（含第一次），全部失败返回 false；
//   - 退避序列 base * 2^n，封顶 max_delay_ms；jitter > 0 时按 ±jitter 比例
//     随机化，避免多客户端同步重试（对齐 RpcClient 的抖动语义）；
//   - 每次尝试后可收到回调 on_attempt(attempt_index, ok) 做日志/指标。

// 询问第 attempt 次（0 起）退避多久（含抖动）
inline int backoff_delay_ms(int attempt, int base_delay_ms, int max_delay_ms, double jitter)
{
    int delay = base_delay_ms;
    for (int i = 0; i < attempt; ++i) {
        delay *= 2;
        if (delay >= max_delay_ms) {
            delay = max_delay_ms;
            break;
        }
    }
    delay = std::min(delay, max_delay_ms);
    if (jitter > 0.0 && delay > 0) {
        const double factor = 1.0 + retry_detail::uniform(-jitter, jitter);
        delay = std::max(1, static_cast<int>(static_cast<double>(delay) * factor));
    }
    return delay;
}

// 轮询直到成功或用尽次数。返回是否成功。
inline bool poll_until(const std::function<bool()>& attempt,
                       int max_attempts,
                       int base_delay_ms = 100,
                       int max_delay_ms = 5000,
                       double jitter = 0.2,
                       const std::function<void(int index, bool ok)>& on_attempt = nullptr)
{
    for (int index = 0; index < max_attempts; ++index) {
        if (attempt()) {
            if (on_attempt) {
                on_attempt(index, true);
            }
            return true;
        }
        if (on_attempt) {
            on_attempt(index, false);
        }
        if (index + 1 < max_attempts) {
            const int delay = backoff_delay_ms(index, base_delay_ms, max_delay_ms, jitter);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    return false;
}

// 带 deadline 的变体：到点即停（次数与时间双约束，哪个先到算哪个）。
// max_attempts 传 0 表示不限次数、只看时间。
inline bool poll_until_deadline(const std::function<bool()>& attempt,
                                int timeout_ms,
                                int base_delay_ms = 100,
                                int max_delay_ms = 5000,
                                double jitter = 0.2)
{
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    int try_index = 0;
    for (;;) {
        if (attempt()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto next = now + std::chrono::milliseconds(
                                  backoff_delay_ms(try_index, base_delay_ms, max_delay_ms, jitter));
        if (next >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(next - now);
        ++try_index;
    }
}

}  // namespace libmini

#endif  // LIBMINI_RETRY_H
