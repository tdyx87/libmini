#include "thread_utils.h"

namespace libmini {

ThreadPool::ThreadPool(size_t num_threads) : stop(false)
{
    for (size_t i = 0; i < num_threads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    condition.wait(lock,
                                   [this] { return stop || !tasks.empty(); });
                    if (stop && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop();
                    ++busy;  // 取到任务即计为忙（含正在执行的全程）
                }
                task();
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    --busy;
                    // 全部线程空闲且队列已空：wait_idle 的等待者放行
                    if (busy == 0 && tasks.empty()) {
                        idle_cv.notify_all();
                    }
                }
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers) {
        worker.join();
    }
}

void ThreadPool::wait_idle()
{
    std::unique_lock<std::mutex> lock(queue_mutex);
    idle_cv.wait(lock, [this] { return tasks.empty() && busy == 0; });
}

std::size_t ThreadPool::pending_tasks() const
{
    std::lock_guard<std::mutex> lock(queue_mutex);
    return tasks.size();
}

// ---------------- CountdownLatch ----------------

CountdownLatch::CountdownLatch(std::size_t count) : count_(count) {}

void CountdownLatch::count_down()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return;  // 已归零：一次性语义，后续调用无效
    if (--count_ == 0) {
        cv_.notify_all();
    }
}

void CountdownLatch::wait() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return count_ == 0; });
}

bool CountdownLatch::wait_for(std::int64_t timeout_ms) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return count_ == 0; });
}

std::size_t CountdownLatch::count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

}  // namespace libmini
