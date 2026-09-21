#ifndef LIBMINI_OBJECT_POOL_H
#define LIBMINI_OBJECT_POOL_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "libmini.h"

namespace libmini {

// 线程安全对象池：借出/归还语义（RAII），适合昂贵对象复用
//（数据库连接、缓冲区、压缩上下文等）。
//
//   ObjectPool<std::vector<char>> pool(4, [] { return std::vector<char>(1024); });
//   {
//       auto buf = pool.acquire();            // Lease RAII 借出
//       (*buf)->resize(100);                  // 使用对象
//   }                                          // 离开作用域自动归还
//
// 语义：
//   - 池容量上限 max_size；池空时 acquire 阻塞等待（或 try_acquire 返回空）；
//   - 归还的对象直接复用（不做重置——由调用方在归还前清理，或给 reset 钩子）；
//   - 对象由池持有到最后（析构时全部销毁）；Lease 析构即归还。

template <typename T>
class ObjectPool
{
public:
    using Factory = std::function<std::unique_ptr<T>()>;
    using Resetter = std::function<void(T&)>;

    ObjectPool(std::size_t max_size, Factory factory)
        : max_size_(max_size), factory_(std::move(factory))
    {
    }

    // 归还前清理钩子（可选）：归还时调用
    void set_resetter(Resetter resetter) { resetter_ = std::move(resetter); }

    // 预填充若干对象（通常在启动时做，避免首批请求现场创建）
    void prefill(std::size_t count)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t n = std::min(count, max_size_);
        for (std::size_t i = 0; i < n; ++i) {
            idle_.push_back(factory_());
        }
    }

    // RAII 借出句柄：析构自动归还
    class Lease
    {
    public:
        Lease() = default;  // 空 Lease（try_acquire 失败）
        Lease(ObjectPool* pool, std::unique_ptr<T> obj)
            : pool_(pool), obj_(std::move(obj))
        {
        }
        Lease(Lease&& other) noexcept
            : pool_(other.pool_), obj_(std::move(other.obj_))
        {
            other.pool_ = nullptr;
        }
        Lease& operator=(Lease&& other) noexcept
        {
            if (this != &other) {
                release();
                pool_ = other.pool_;
                obj_ = std::move(other.obj_);
                other.pool_ = nullptr;
            }
            return *this;
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        ~Lease() { release(); }

        T* operator->() const { return obj_.get(); }
        T& operator*() const { return *obj_; }
        explicit operator bool() const { return obj_ != nullptr; }

    private:
        void release()
        {
            if (pool_ != nullptr && obj_ != nullptr) {
                pool_->give_back_(std::move(obj_));
                pool_ = nullptr;
                obj_ = nullptr;
            }
        }

        ObjectPool* pool_ = nullptr;
        std::unique_ptr<T> obj_;
    };

    // 阻塞借出：池空时等待（可被 max_wait_ms 限制，<0 = 无限等）。
    // 超时返回空 Lease
    Lease acquire(int max_wait_ms = -1)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        bool ok;
        if (max_wait_ms < 0) {
            cv_.wait(lock, [this] { return !idle_.empty() || created_ < max_size_; });
            ok = true;
        } else {
            ok = cv_.wait_for(lock, std::chrono::milliseconds(max_wait_ms), [this] {
                return !idle_.empty() || created_ < max_size_;
            });
        }
        if (!ok) {
            return Lease();  // 超时
        }
        return take_one_();
    }

    // 非阻塞借出：池空且已到容量上限时返回空 Lease
    Lease try_acquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idle_.empty() && created_ >= max_size_) {
            return Lease();
        }
        return take_one_();
    }

    std::size_t max_size() const { return max_size_; }

    // 空闲对象数（诊断用）
    std::size_t idle_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_.size();
    }

private:
    Lease take_one_()
    {
        if (idle_.empty()) {
            // 到容量上限但池空 = 全部借出（不可能，acquire 谓词保证）；
            // 否则现场创建
            ++created_;
            return Lease(this, factory_());
        }
        auto obj = std::move(idle_.back());
        idle_.pop_back();
        return Lease(this, std::move(obj));
    }

    // Lease 归还入口（内部）
    void give_back_(std::unique_ptr<T> obj)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (resetter_) {
                resetter_(*obj);
            }
            idle_.push_back(std::move(obj));
        }
        cv_.notify_one();
    }

    std::size_t max_size_;
    Factory factory_;
    Resetter resetter_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<T>> idle_;
    std::size_t created_ = 0;  // 已创建总数（含借出中的）
};

}  // namespace libmini

#endif  // LIBMINI_OBJECT_POOL_H
