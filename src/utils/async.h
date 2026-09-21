#ifndef LIBMINI_ASYNC_H
#define LIBMINI_ASYNC_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "libmini.h"

namespace libmini {

// 任务取消句柄（可拷贝，指向调度器内部的任务记录）。
//   auto id = sched.run_after_ms(1000, fn);
//   ...
//   id.cancel();   // 尚未执行的任务被取消；已开始/已执行的取消无效
class LIBMINI_API TaskHandle {
public:
    TaskHandle() : id_(0), scheduler_(nullptr) {}

    // 取消任务。返回 true 表示成功取消（任务尚未运行）。
    bool cancel();

private:
    friend class AsyncScheduler;
    TaskHandle(std::uint64_t id, class AsyncScheduler* sched)
        : id_(id), scheduler_(sched)
    {
    }

    std::uint64_t id_;
    class AsyncScheduler* scheduler_;
};

// 异步任务调度器：单后台线程 + 最小堆定时轮。
//   - run_after_ms:  延时执行一次
//   - run_every_ms:  固定间隔重复执行（返回 false 停止）
//   - run_every_ms 组合 std::bind 可实现 "N 次后停止"
// 线程安全：任意线程可提交/取消；任务在工作线程串行执行，
// 长任务会推迟后续任务（本类不做并发执行，需要并发请配合 ThreadPool）。
class LIBMINI_API AsyncScheduler {
public:
    AsyncScheduler();
    ~AsyncScheduler();

    AsyncScheduler(const AsyncScheduler&) = delete;
    AsyncScheduler& operator=(const AsyncScheduler&) = delete;

    // 延时 delay_ms 后执行 fn，返回可取消句柄
    TaskHandle run_after_ms(std::int64_t delay_ms, std::function<void()> fn);

    // 每隔 interval_ms 执行一次 fn，从提交时刻起算（尽量不受执行耗时影响）。
    // fn 返回 false 时自动停止该任务。
    TaskHandle run_every_ms(std::int64_t interval_ms,
                            std::function<bool()> fn);

    // 已注册（未执行/未取消/未停止）的任务数
    std::size_t pending_count() const;

    // 取消所有未执行任务（已提交回调不影响）
    void cancel_all();

private:
    friend class TaskHandle;

    struct Task {
        std::uint64_t id;       // 全局唯一 id（TaskHandle 用它取消）
        std::int64_t next_run;  // steady_clock 时间戳（毫秒）
        std::int64_t interval;  // 0 = 单次
        std::function<bool()> fn;
        bool cancelled;
    };

    void worker_loop();
    bool cancel_task(std::uint64_t id);

    // 按到期时间排序的任务表（ multimap: key=next_run ）
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::multimap<std::int64_t, Task> tasks_;
    std::uint64_t next_id_;
    bool stopping_;
    std::thread worker_;
};

// 令牌桶限流器：以固定速率补充令牌，桶容量即突发上限。
//   RateLimiter limiter(10.0, 5);   // 稳态 10 次/秒，最多突发 5 次
//   if (limiter.try_acquire()) { do_request(); }
//   limiter.acquire();              // 阻塞直到拿到令牌
//   limiter.acquire(3);             // 一次取 3 个令牌
class LIBMINI_API RateLimiter {
public:
    // rate_per_sec: 每秒补充令牌数（> 0）
    // burst_size:   桶容量（> 0），也是初始令牌数
    RateLimiter(double rate_per_sec, double burst_size);

    // 非阻塞尝试获取 permits 个令牌
    bool try_acquire(double permits = 1.0);

    // 阻塞获取 permits 个令牌（返回等待的毫秒数，便于观测）
    std::int64_t acquire(double permits = 1.0);

    // 当前可用令牌数（瞬时值）
    double available() const;

private:
    // const 方法（available()）也要做惰性补充，故相关字段为 mutable
    void refill_locked() const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    double rate_;         // 令牌/秒
    double burst_;        // 桶容量
    mutable double tokens_;       // 当前令牌
    mutable std::chrono::steady_clock::time_point last_refill_;
};

}  // namespace libmini

#endif  // LIBMINI_ASYNC_H
