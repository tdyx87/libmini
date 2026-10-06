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

    // 提交一个无参数、void 返回的任务（不依赖模板实例化，供测试/调用方直接使用）
    std::future<void> submit_void(std::function<void()> f);

    // 提交一个无参数、int 返回的任务（不依赖模板实例化，供测试/调用方直接使用）
    std::future<int> submit_int(std::function<int()> f);

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

// ============================================================================
// 线程同步扩展（libmini 扩展）
// ============================================================================

// ---------------- 信号量 ----------------

// 计数信号量。对标 std::counting_semaphore（C++20），此处提供 C++11 实现。
//
//   Semaphore sem(0);              // 初始可用 0
//   sem.acquire();                 // 消费一个单位（阻塞到可用）
//   sem.try_acquire_for(1000);     // 最多等 1s
//   sem.release(3);                // 释放 3 个单位，唤醒最多 3 个等待者
//
// 注意：release 增加信号量计数，不与 std::mutex 释放语义混淆。
class LIBMINI_API Semaphore {
public:
    explicit Semaphore(std::size_t initial = 0);

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    // 获取一个单位；不可用时阻塞
    void acquire();

    // 获取一个单位；超时返回 false
    bool try_acquire_for(std::int64_t timeout_ms);

    // 非阻塞获取一个单位，成功返回 true
    bool try_acquire();

    // 释放一个单位（至少唤醒一个等待者，若有）
    void release();

    // 释放 n 个单位（尽力唤醒对应数量的等待者）
    void release(std::size_t n);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t count_;
};

// ---------------- 事件（手动重置） ----------------

// 手动重置事件（manual-reset event）。对标 Win32 CreateEvent(EVENT_MODE_MANUAL_RESET)
// 与 std::experimental::barrier 前身的简单版；用于「某条件达成，所有等待者放行」。
//
//   Event ev;
//   ev.set();                      // 置位：后续 wait 皆立即返回
//   ev.wait();                     //（若已置位）立即返回
//   ev.wait_for(1000);             // 最多等 1 秒
//   ev.reset();                    // 清除置位，恢复为未通知状态
//
// 注意：这是一个简单的线程通知工具，不带自动重置语义。若需要「通知一个
// 等待者后自动清除」的行为，考虑 Semaphore(0) + release/acquire。
class LIBMINI_API Event {
public:
    explicit Event(bool initially_set = false);

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    // 置位事件；随后所有等待者立即放行
    void set();

    // 清除事件（非阻塞）
    void reset();

    // 等待事件被置位；若已置位则立即返回
    void wait() const;

    // 最多等待 timeout_ms；返回 true 表示已置位
    bool wait_for(std::int64_t timeout_ms) const;

    // 查询当前是否已置位（快照）
    bool is_set() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool set_;
};

// ---------------- 只运行一次（线程安全） ----------------

// 线程安全的一次性执行控制。对标 std::call_once / std::once_flag。
//
//   OnceFlag flag;
//   flag.call_once([] { init_once(); });
//
// 与 std::call_once 语义一致：即便传入的函数抛出，once_flag 也不认为已成功
// 完成，后续调用会重试（与 std::once_flag 在 C++11 中的抛出后行为一致）。
//
// 此封装存在的理由是让 libmini 不依赖 C++11 的 <once>（某些旧工具链/编码
// 规范倾向显式封装），且便于后续追踪/替换实现。
class LIBMINI_API OnceFlag {
public:
    OnceFlag() = default;

    OnceFlag(const OnceFlag&) = delete;
    OnceFlag& operator=(const OnceFlag&) = delete;

    // 若尚未成功运行过给定的函数，则以线程安全方式运行它。
    // 返回 true 表示本次调用实际执行了 fn（可能是本线程第一次，
    // 也可能是过去抛出过而本次重跑）。若已有线程成功执行过 fn，
    // 返回 false。
    template <class Callable>
    bool call_once(Callable&& fn);

private:
    enum class State : std::int32_t { Idle = 0, Running = 1, Done = 2 };
    std::atomic<State> once_state_{State::Idle};
};

// ---------------- 协作式取消标记 ----------------

// 轻量级协作式取消标记。适合传递给长耗时操作，令其尽快退出。
//
// 用法示例：
//   CancellationToken ct;
//   auto task = std::thread([&]{ long_work(ct); });
//   ...
//   ct.cancel();
//   task.join();
//
// 实现不使用条件变量唤醒，仅原子标记与短睡眠轮询搭配，因此适合作为传递参数。
// 若需要「取消即唤醒阻塞线程」，结合 Event 或 Semaphore 自行构造。
class LIBMINI_API CancellationToken {
public:
    CancellationToken() = default;

    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;

    // 请求取消。后续 is_cancelled() 返回 true。
    // 可调用多次（幂等）。
    void cancel();

    // 当前是否已请求取消（线程安全快照）
    bool is_cancelled() const;

    // 阻塞直到取消被请求或超时（毫秒）。返回 true 表示已取消。
    // 本函数采用短睡眠轮询，非精确实时唤醒。
    bool wait_for_cancel(std::int64_t timeout_ms) const;

    // 将本标记链接到另一个标记：当 other 被 cancel 时，本标记也变为
    // is_cancelled() == true（仅增加观察，不互相 cancel）。
    // 只能链接一次（重复链接是幂等操作）。
    void link_to(const CancellationToken& other);

private:
    std::atomic<bool> cancelled_{false};
    const CancellationToken* linked_to_ = nullptr;  // 若非空，is_cancelled 还看它
};

template <class Callable>
bool OnceFlag::call_once(Callable&& fn)
{
    State expected = State::Idle;
    if (!once_state_.compare_exchange_strong(
            expected, State::Running,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        // 已有线程抢先进入 Running 或已完成
        if (expected == State::Done) return false;
        // expected == State::Running：另一个线程在运行中，当前线程轮候
        while (once_state_.load(std::memory_order_acquire) != State::Done) {
            std::this_thread::yield();
        }
        return false;
    }
    // 抢到运行权
    try {
        std::forward<Callable>(fn)();
        once_state_.store(State::Done, std::memory_order_release);
        return true;
    } catch (...) {
        once_state_.store(State::Idle, std::memory_order_release);
        throw;
    }
}

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