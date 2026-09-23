#ifndef LIBMINI_THREAD_UTILS_H
#define LIBMINI_THREAD_UTILS_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "export.h"

namespace libmini {

// 简单线程池
class LIBMINI_API ThreadPool {
public:
    ThreadPool(size_t num_threads);
    ~ThreadPool();

    // 提交任务
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type>;

    // 阻塞等待：已提交任务全部执行完毕后返回（此后仍可继续 submit）。
    // 注意：任务内部再向同一线程池 submit 可能死锁（无空闲线程时）
    void wait_idle();

    // 当前排队未执行的任务数（瞬时值）
    std::size_t pending_tasks() const;

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    mutable std::mutex queue_mutex;
    std::condition_variable condition;
    // idle 通知专用条件变量：任务取空时广播，避免与工作线程的取任务
    // 等待互相打扰
    std::condition_variable idle_cv;
    bool stop;
    std::size_t busy = 0;  // 正在执行任务的线程数
};

// ---------------- 生产者-消费者阻塞队列 ----------------

// 多生产者多消费者线程安全队列：空时 pop 阻塞，满时 push 阻塞
//（capacity <= 0 视为无界）。典型用途：工作线程与 IO 线程解耦。
//
//   BlockingQueue<int> q(16);
//   q.push(42);                        // 生产端
//   int v = q.pop();                   // 消费端（阻塞）
//   if (q.try_pop(v, 100)) { ... }     // 最多等 100ms
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    // 入队；队列已满时阻塞直到有空位或队列被关闭
    void push(T value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [&] {
            return closed_ || capacity_ == 0 || queue_.size() < capacity_;
        });
        if (closed_) return;
        queue_.push_back(std::move(value));
        not_empty_.notify_one();
    }

    // 出队；队列空时阻塞。返回 false 表示队列已关闭且已取空
    bool pop(T& out)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return false;  // closed 且取空
        out = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    // 出队但最多等 timeout_ms；超时返回 false
    bool try_pop(T& out, std::int64_t timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [&] { return !queue_.empty() || closed_; })) {
            return false;
        }
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    // 关闭队列：唤醒全部等待者；已入队元素仍可继续取（pop 到空返回 false）。
    // 关闭后 push 直接丢弃
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> queue_;
    std::size_t capacity_;  // 0 = 无界
    bool closed_ = false;
};

// ---------------- 阶段同步倒计时门闩 ----------------

// 一次性倒计时同步：n 个事件全部发生后，所有等待者放行。
// 对标 java.util.CountDownLatch / C# CountdownEvent。
//
//   CountdownLatch latch(3);
//   // 工作线程各自完成后：latch.count_down();
//   latch.wait();                       // 主线程等三件事都完成
class LIBMINI_API CountdownLatch {
public:
    explicit CountdownLatch(std::size_t count);

    CountdownLatch(const CountdownLatch&) = delete;
    CountdownLatch& operator=(const CountdownLatch&) = delete;

    // 计数减一；减到 0 时唤醒全部等待者。计数已为 0 时调用无效
    void count_down();

    // 计数到 0 后立即返回；否则阻塞直到计数归零
    void wait() const;

    // 最多等 timeout_ms；返回 true 表示计数已归零
    bool wait_for(std::int64_t timeout_ms) const;

    // 当前计数（瞬时值）
    std::size_t count() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::size_t count_;
};

template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type>
{
    using return_type = typename std::result_of<F(Args...)>::type;
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop) throw std::runtime_error("submit on stopped ThreadPool");
        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return res;
}

}  // namespace libmini

#endif  // LIBMINI_THREAD_UTILS_H