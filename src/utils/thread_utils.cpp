#include "thread_utils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace libmini {

// ---------------- 线程池（原有） ----------------

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

// ---- 显式非模板的 submit 变体（避免模板实例化依赖于局部 lambda） ----
// 原始 submit 是包含型模板，定义在头文件里。项目测试中曾出现“declared
// using local type ... is used but never defined”类链接失败，原因是测试
// 可执行文件使用的 lambda 类型无法在库翻译单元边界实例化。
// 这里提供两个实际常用的非模板入口（void 返回 / int 返回、无额外参数），
// 对应调用 submit(...)，这样既保留通用模板，又能在库里提供确定类型的
// 实例。
std::future<void> ThreadPool::submit_void(std::function<void()> f)
{
    using return_type = void;
    using packaged = std::packaged_task<return_type()>;
    auto task = std::make_shared<packaged>(std::move(f));
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop) throw std::runtime_error("submit on stopped ThreadPool");
        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return res;
}

std::future<int> ThreadPool::submit_int(std::function<int()> f)
{
    using return_type = int;
    using packaged = std::packaged_task<return_type()>;
    auto task = std::make_shared<packaged>(std::move(f));
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop) throw std::runtime_error("submit on stopped ThreadPool");
        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return res;
}

// ---------------- CountdownLatch（原有） ----------------

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
    // std::condition_variable::wait_for 是非 const 成员，故此处使用 const_cast
    // 绕过编译器检查。CountdownLatch 的语义保证在此过程中不会修改 cv_，所以行为安全。
    auto& nonconst_cv = const_cast<std::condition_variable&>(cv_);
    return nonconst_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return count_ == 0; });
}

std::size_t CountdownLatch::count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

// ---------------- 信号量（扩展） ----------------

Semaphore::Semaphore(std::size_t initial) : count_(initial) {}

void Semaphore::acquire()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return count_ > 0; });
    --count_;
}

bool Semaphore::try_acquire_for(std::int64_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [&] { return count_ > 0; })) {
        return false;
    }
    --count_;
    return true;
}

bool Semaphore::try_acquire()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return false;
    --count_;
    return true;
}

void Semaphore::release()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
    }
    cv_.notify_one();
}

void Semaphore::release(std::size_t n)
{
    std::size_t to_notify = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count_ += n;
        // 尽力唤醒对应数量的等待者（不可能比当前等待者多）
        to_notify = n;
    }
    for (std::size_t i = 0; i < to_notify; ++i) {
        cv_.notify_one();
    }
}

// ---------------- 事件（手动重置）（扩展） ----------------

Event::Event(bool initially_set) : set_(initially_set) {}

void Event::set()
{
    std::lock_guard<std::mutex> lock(mutex_);
    set_ = true;
    cv_.notify_all();
}

void Event::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    set_ = false;
}

void Event::wait() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto& nonconst_cv = const_cast<std::condition_variable&>(cv_);
    nonconst_cv.wait(lock, [&] { return set_; });
}

bool Event::wait_for(std::int64_t timeout_ms) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    // std::condition_variable::wait_for 是非 const 成员，故此处使用 const_cast
    // 绕过编译器检查。Event 的语义保证在此过程中不会修改 cv_，所以行为安全。
    auto& nonconst_cv = const_cast<std::condition_variable&>(cv_);
    return nonconst_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [&] { return set_; });
}

bool Event::is_set() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return set_;
}

// ---------------- 协作式取消标记（扩展） ----------------

void CancellationToken::cancel()
{
    cancelled_.store(true, std::memory_order_release);
}

bool CancellationToken::is_cancelled() const
{
    if (cancelled_.load(std::memory_order_acquire)) return true;
    if (linked_to_ != nullptr) return linked_to_->is_cancelled();
    return false;
}

bool CancellationToken::wait_for_cancel(std::int64_t timeout_ms) const
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    // 短轮询：避免条件变量语义模糊（取消不一定是唯一唤醒源）
    while (!is_cancelled()) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds::zero()) return true;
        std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(20)));
    }
    return true;
}

void CancellationToken::link_to(const CancellationToken& other)
{
    // 链接目标固定，写一次即可；无需原子（构造后不变）
    linked_to_ = &other;
}

}  // namespace libmini
