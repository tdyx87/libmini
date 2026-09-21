#ifndef LIBMINI_THREAD_UTILS_H
#define LIBMINI_THREAD_UTILS_H

#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <future>

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

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
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