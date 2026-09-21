#include "async.h"

#include <algorithm>

namespace libmini {

namespace {

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ============================== AsyncScheduler =============================

AsyncScheduler::AsyncScheduler()
    : next_id_(1), stopping_(false), worker_(&AsyncScheduler::worker_loop, this)
{
}

AsyncScheduler::~AsyncScheduler()
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

TaskHandle AsyncScheduler::run_after_ms(std::int64_t delay_ms,
                                        std::function<void()> fn)
{
    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;

        Task task;
        task.id = id;
        task.next_run = now_ms() + std::max<std::int64_t>(0, delay_ms);
        task.interval = 0;
        task.cancelled = false;
        task.fn = [fn]() {
            fn();
            return false;  // 单次任务执行后自动移除
        };
        tasks_.insert(std::make_pair(task.next_run, task));
    }
    cv_.notify_all();  // 新的更早到期任务可能改变等待时间
    return TaskHandle(id, this);
}

TaskHandle AsyncScheduler::run_every_ms(std::int64_t interval_ms,
                                        std::function<bool()> fn)
{
    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;

        Task task;
        task.id = id;
        task.next_run = now_ms() + std::max<std::int64_t>(1, interval_ms);
        task.interval = std::max<std::int64_t>(1, interval_ms);
        task.cancelled = false;
        task.fn = fn;
        tasks_.insert(std::make_pair(task.next_run, task));
    }
    cv_.notify_all();
    return TaskHandle(id, this);
}

std::size_t AsyncScheduler::pending_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (std::multimap<std::int64_t, Task>::const_iterator it = tasks_.begin();
         it != tasks_.end(); ++it) {
        if (!it->second.cancelled) {
            ++n;
        }
    }
    return n;
}

void AsyncScheduler::cancel_all()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::multimap<std::int64_t, Task>::iterator it = tasks_.begin();
             it != tasks_.end(); ++it) {
            it->second.cancelled = true;
        }
    }
    cv_.notify_all();
}

bool AsyncScheduler::cancel_task(std::uint64_t id)
{
    // 取消是低频操作，线性扫描即可
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::multimap<std::int64_t, Task>::iterator it = tasks_.begin();
             it != tasks_.end(); ++it) {
            if (it->second.id == id) {
                if (it->second.cancelled) {
                    return false;  // 已取消过
                }
                it->second.cancelled = true;
                cv_.notify_all();  // 工作线程可能按到它
                return true;
            }
        }
    }
    return false;  // 不存在（已执行完/已移除）
}

void AsyncScheduler::worker_loop()
{
    for (;;) {
        std::int64_t next_wake;
        Task ready;
        bool has_ready = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // 清理已取消/已到期待处理的头部
            for (;;) {
                if (stopping_) {
                    return;
                }
                if (tasks_.empty()) {
                    next_wake = -1;  // 无任务，等待提交
                    break;
                }

                std::multimap<std::int64_t, Task>::iterator front =
                    tasks_.begin();
                if (front->second.cancelled) {
                    tasks_.erase(front);
                    continue;
                }
                if (front->first <= now_ms()) {
                    // 取出执行：多期任务先安排下一期（保持注册状态，
                    // 执行期间 cancel() 仍可让下一期失效）
                    ready = front->second;
                    has_ready = true;
                    if (ready.interval > 0) {
                        Task next = ready;
                        next.next_run = front->first + ready.interval;
                        tasks_.insert(std::make_pair(next.next_run, next));
                    }
                    tasks_.erase(front);
                    break;
                }
                next_wake = front->first;
                break;
            }

            if (stopping_) {
                return;
            }
        }

        if (has_ready) {
            const bool again = ready.fn();
            if (!again && ready.interval > 0) {
                // 回调要求停止：移除已预注册的后续期次
                std::lock_guard<std::mutex> lock(mutex_);
                for (std::multimap<std::int64_t, Task>::iterator it =
                         tasks_.begin();
                     it != tasks_.end();) {
                    if (it->second.id == ready.id) {
                        it = tasks_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            continue;  // 立刻检查下一个到期任务
        }

        // 空闲等待：等新任务提交或到最近到期点
        std::unique_lock<std::mutex> lock(mutex_);
        if (next_wake < 0) {
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        } else {
            cv_.wait_until(lock,
                           std::chrono::steady_clock::time_point(
                               std::chrono::milliseconds(next_wake)));
        }
    }
}

// ================================ TaskHandle ===============================

bool TaskHandle::cancel()
{
    if (scheduler_ == nullptr || id_ == 0) {
        return false;
    }
    return scheduler_->cancel_task(id_);
}

// =============================== RateLimiter ===============================

RateLimiter::RateLimiter(double rate_per_sec, double burst_size)
    : rate_(rate_per_sec > 0.0 ? rate_per_sec : 1.0),
      burst_(burst_size > 0.0 ? burst_size : 1.0),
      tokens_(burst_),
      last_refill_(std::chrono::steady_clock::now())
{
}

void RateLimiter::refill_locked() const
{
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    const double elapsed_sec =
        std::chrono::duration<double>(now - last_refill_).count();
    if (elapsed_sec > 0.0) {
        tokens_ = std::min(burst_, tokens_ + elapsed_sec * rate_);
        last_refill_ = now;
    }
}

bool RateLimiter::try_acquire(double permits)
{
    if (permits <= 0.0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    refill_locked();
    if (tokens_ >= permits) {
        tokens_ -= permits;
        return true;
    }
    return false;
}

std::int64_t RateLimiter::acquire(double permits)
{
    if (permits <= 0.0) {
        return 0;
    }
    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        refill_locked();
        if (tokens_ >= permits) {
            tokens_ -= permits;
            break;
        }
        // 需要多久才能攒够？
        const double deficit = permits - tokens_;
        const double wait_sec = deficit / rate_;
        const std::int64_t wait_ms =
            static_cast<std::int64_t>(wait_sec * 1000.0) + 1;
        cv_.wait_for(lock, std::chrono::milliseconds(wait_ms));
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

double RateLimiter::available() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    refill_locked();
    return tokens_;
}

}  // namespace libmini
