#include "timer_wheel.h"

#include <algorithm>

namespace libmini {

namespace {

// 向上取整到 tick 的整数倍（至少 1 tick）
std::int64_t round_up_ticks(std::int64_t ms, std::int64_t tick_ms)
{
    if (ms <= 0) {
        return 1;
    }
    return (ms + tick_ms - 1) / tick_ms;
}

}  // namespace

// ==================== Handle ====================

bool TimerWheel::Handle::cancel()
{
    if (wheel == nullptr || id == 0) {
        return false;
    }
    return wheel->cancel_timer(id);
}

// ==================== 构造 / 析构 ====================

TimerWheel::TimerWheel(std::chrono::milliseconds tick)
    : tick_(tick > std::chrono::milliseconds(0)
                ? tick
                : std::chrono::milliseconds(1)),
      slots_(kSlotCount),
      next_id_(1),
      current_tick_(0),
      pending_(0),
      stopping_(false)
{
    worker_ = std::thread(&TimerWheel::worker_loop, this);
}

TimerWheel::~TimerWheel()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

// ==================== 添加 / 取消 ====================

void TimerWheel::insert_timer(Timer t)
{
    const std::size_t slot = static_cast<std::size_t>(t.deadline) % kSlotCount;
    slots_[slot].timers.push_back(std::move(t));
}

TimerWheel::Handle TimerWheel::add_ms(std::int64_t delay_ms,
                                      std::function<void()> fn)
{
    Timer t;
    t.id = 0;
    t.interval = 0;
    t.fn = std::function<bool()>([fn]() mutable -> bool {
        fn();
        return false;  // 单次任务触发后移除
    });
    t.cancelled = false;

    std::lock_guard<std::mutex> lock(mutex_);
    t.id = next_id_++;
    t.deadline = current_tick_ + static_cast<std::uint64_t>(
                     round_up_ticks(delay_ms, tick_.count()));
    insert_timer(std::move(t));
    ++pending_;
    cv_.notify_all();
    Handle h;
    h.id = t.id;
    h.wheel = this;
    return h;
}

TimerWheel::Handle TimerWheel::add_periodic_ms(std::int64_t interval_ms,
                                               std::function<bool()> fn)
{
    Timer t;
    t.id = 0;
    t.interval = round_up_ticks(interval_ms, tick_.count());
    t.fn = std::move(fn);
    t.cancelled = false;

    std::lock_guard<std::mutex> lock(mutex_);
    t.id = next_id_++;
    t.deadline = current_tick_ + static_cast<std::uint64_t>(t.interval);
    insert_timer(std::move(t));
    ++pending_;
    cv_.notify_all();
    Handle h;
    h.id = t.id;
    h.wheel = this;
    return h;
}

bool TimerWheel::cancel_timer(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t s = 0; s < kSlotCount; ++s) {
        std::vector<Timer>& tv = slots_[s].timers;
        for (std::size_t i = 0; i < tv.size(); ++i) {
            if (tv[i].id == id && !tv[i].cancelled) {
                tv[i].cancelled = true;
                --pending_;
                idle_cv_.notify_all();
                return true;
            }
        }
    }
    return false;
}

void TimerWheel::cancel_timer_locked(std::uint64_t id)
{
    // 回调执行循环专用：锁已持有、pending 已扣，只补标 cancelled
    //（单次任务此时已被移出槽位，这里是 no-op）
    for (std::size_t s = 0; s < kSlotCount; ++s) {
        std::vector<Timer>& tv = slots_[s].timers;
        for (std::size_t i = 0; i < tv.size(); ++i) {
            if (tv[i].id == id && !tv[i].cancelled) {
                tv[i].cancelled = true;
                return;
            }
        }
    }
}

// ==================== 查询 / 等待 ====================

std::size_t TimerWheel::pending_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

bool TimerWheel::idle() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ == 0;
}

void TimerWheel::wait_idle() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] { return pending_ == 0; });
}

// ==================== 轮线程 ====================

void TimerWheel::worker_loop()
{
    const auto tick_dur = tick_;
    std::size_t cursor = 0;

    while (true) {
        std::vector<Timer> due;  // 本 tick 到期的任务（锁外执行）

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // 空轮挂起：等新任务或停机（不空转）
            if (pending_ == 0 && !stopping_) {
                cv_.wait(lock, [this] { return pending_ > 0 || stopping_; });
            }
            if (stopping_) {
                return;
            }

            // 快照到期任务；游标推进由 worker 独占，current_tick_ 无竞争
            ++current_tick_;
            cursor = static_cast<std::size_t>(current_tick_) % kSlotCount;
            Slot& slot = slots_[cursor];

            for (std::size_t i = 0; i < slot.timers.size();) {
                Timer& t = slot.timers[i];
                if (t.cancelled) {
                    // 惰性清除
                    t = std::move(slot.timers.back());
                    slot.timers.pop_back();
                    continue;
                }
                if (current_tick_ < t.deadline) {
                    ++i;
                    continue;
                }
                due.push_back(t);
                if (t.interval <= 0) {
                    // 单次任务：移除（执行后扣 pending）
                    t = std::move(slot.timers.back());
                    slot.timers.pop_back();
                    continue;
                }
                // 周期任务：推进 deadline 后按新槽位重新入轮
                t.deadline += static_cast<std::uint64_t>(t.interval);
                const std::size_t next_slot =
                    static_cast<std::size_t>(t.deadline) % kSlotCount;
                if (next_slot != cursor) {
                    slots_[next_slot].timers.push_back(t);
                    t = std::move(slot.timers.back());
                    slot.timers.pop_back();
                    continue;   // swap-pop 后重查当前位置
                }
                ++i;            // 恰好同槽（interval 是槽数的倍数）
            }
        }

        // 锁外执行到期回调；返回 false 的（单次任务 / 自停周期任务）
        // 需要把轮内记录移除：单次任务已从槽里删除，这里只补标 cancelled
        //（pending 一起扣掉）；周期任务自停则就地标记，等待惰性清除
        for (std::size_t i = 0; i < due.size(); ++i) {
            const bool keep = due[i].fn();
            if (!keep) {
                std::lock_guard<std::mutex> lock(mutex_);
                --pending_;
                cancel_timer_locked(due[i].id);
                idle_cv_.notify_all();
            }
        }

        // 固定节拍推进
        std::this_thread::sleep_for(tick_dur);
    }
}

}  // namespace libmini
