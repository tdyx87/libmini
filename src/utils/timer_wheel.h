#ifndef LIBMINI_TIMER_WHEEL_H
#define LIBMINI_TIMER_WHEEL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "libmini.h"

namespace libmini {

// 层级时间轮：海量定时器的 O(1) 添加/取消，固定节拍推进。
// 适用场景：万级连接超时管理、批量会话保活——远超 AsyncScheduler
// 最小堆的 O(log n) 在大规模下的表现。
//
//   TimerWheel wheel(std::chrono::milliseconds(10));  // 10ms 节拍
//   TimerWheel::Handle h = wheel.add_ms(5000, [] { on_timeout(); });
//   h.cancel();                                        // O(1) 取消
//   wheel.add_periodic_ms(100, [] -> bool { ... });    // 返回 false 停止
//   wheel.wait_idle();                                 // 无 pending 任务时返回
//
// 线程模型：单后台线程按 tick 推进并触发到期回调；回调在轮线程上串行
// 执行（长回调会推迟后续 tick，需要并发请配合 ThreadPool）。任意线程
// 可 add/cancel/wait_idle；析构自动停机并等待线程退出。
//
// 与 AsyncScheduler 的关系：AsyncScheduler 面向"少量精准任务"（堆按
// 绝对到期时间唤醒，不空转）；TimerWheel 面向"海量等间隔轮询"（固定
// 节拍，O(1) 增删，代价是到期精度 ±1 tick）。
class LIBMINI_API TimerWheel {
public:
    // 取消句柄（可拷贝；指向轮内 slot 中的槽位记录）
    struct Handle {
        Handle() : id(0), wheel(nullptr) {}

        // 取消定时器。返回 true 表示成功取消（尚未触发）。
        bool cancel();

        std::uint64_t id;
        class TimerWheel* wheel;
    };

    // tick: 轮拍间隔（>0），决定到期精度下限
    explicit TimerWheel(std::chrono::milliseconds tick);
    ~TimerWheel();

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    // 延时 delay_ms 后执行 fn 一次。返回可取消句柄。
    // delay_ms 会被向上取整到 tick 的整数倍（精度 ±1 tick）。
    Handle add_ms(std::int64_t delay_ms, std::function<void()> fn);

    // 每隔 interval_ms 执行一次 fn；fn 返回 false 时自动停止。
    // interval_ms 会被向上取整到 tick 的整数倍（至少 1 tick）。
    Handle add_periodic_ms(std::int64_t interval_ms, std::function<bool()> fn);

    // 当前未触发/未取消的定时器数（瞬时值）
    std::size_t pending_count() const;

    // 阻塞直到无 pending 定时器。注意：周期任务会一直 pending，
    // 需先 cancel 其句柄（或让 fn 返回 false 自停）才能 idle。
    void wait_idle() const;

    // 是否所有定时器已触发/取消（wait_idle 的非阻塞版）
    bool idle() const;

private:
    friend struct Handle;

    struct Timer {
        std::uint64_t id;
        std::int64_t interval;   // 0 = 单次；>0 = 周期（tick 数）
        std::uint64_t deadline;  // 绝对到期 tick（全局计数器口径）
        std::function<bool()> fn;  // 包装后的统一签名（单次返回 false）
        bool cancelled;
    };

    struct Slot {
        std::vector<Timer> timers;
    };

    void worker_loop();
    bool cancel_timer(std::uint64_t id);
    void insert_timer(Timer t);
    // 按 id 标记取消（不加锁不扣 pending——回调循环内专用，锁已在外层持有）
    void cancel_timer_locked(std::uint64_t id);

    // 参数
    std::chrono::milliseconds tick_;

    // 环形槽位。定时器放在 deadline % kSlotCount 槽；游标每 tick 前进一格，
    // 到达该槽且全局计数 ≥ deadline 时触发（绝对到期，无 rounds 计数）。
    static constexpr std::size_t kSlotCount = 256;
    std::vector<Slot> slots_;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;      // 停机 / 新任务唤醒
    mutable std::condition_variable idle_cv_;  // wait_idle 等待（const 方法）
    std::uint64_t next_id_;
    std::uint64_t current_tick_;          // 全局 tick 计数（worker 专属推进）
    std::size_t pending_;
    bool stopping_;
    std::thread worker_;
};

}  // namespace libmini

#endif  // LIBMINI_TIMER_WHEEL_H
