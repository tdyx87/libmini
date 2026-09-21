#ifndef LIBMINI_FILE_LOCK_H
#define LIBMINI_FILE_LOCK_H

#include <cstdint>
#include <string>

#include "libmini.h"
#ifndef LIBMINI_STATIC
#ifdef LIBMINI_EXPORTS
#define LIBMINI_API __declspec(dllexport)
#else
#define LIBMINI_API __declspec(dllimport)
#endif
#else
#define LIBMINI_API
#endif

namespace libmini {

// 跨进程独占文件锁。
//
// 用锁文件（lock file）实现：对指定路径的锁文件取得独占句柄即持有锁，
// 同一时刻系统中只有一个进程能持有。典型用途：
//   - 单实例应用（防止重复启动）
//   - 保护跨进程共享资源（缓存目录、配置写入、日志轮转等）
//
// 锁文件默认持久存在（内容为空），仅作为锁的载体，不删除——
// 删除会引入"检查-删除-创建"竞态。
//
// Windows 实现：CreateFileW 不带 FILE_SHARE_READ/WRITE（独占共享模式），
// 第二个进程打开同一文件即失败；POSIX 实现：open(O_CREAT|O_EXCL 语义由
// flock 提供) + flock(LOCK_EX|LOCK_NB)。
//
// 不可拷贝；持有锁的实例析构时自动释放。
class LIBMINI_API FileLock
{
public:
    // 锁模式：Exclusive = 任意时刻一个持有者；Shared = 允许多读者并存、
    // 与写者互斥（读多写少场景：多个进程同时读、配置更新时取写锁）
    enum class Mode { Exclusive, Shared };

    FileLock();
    explicit FileLock(const std::string& path, Mode mode = Mode::Exclusive);
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    // 尝试获取锁（不阻塞）。成功后持有直到 unlock()/析构。
    bool try_lock();

    // 以共享（读）模式尝试获取。与独占锁互斥、与其他共享锁兼容。
    // 同一 FileLock 实例的独占/共享模式由构造决定，不可混用
    bool try_lock_shared();

    // 阻塞获取，最多等待 timeout_ms 毫秒（0 = 立即返回，同 try_lock）。
    // 返回是否成功获得锁。
    bool lock_for(int timeout_ms);

    // 释放锁（未持有时调用是无害的 no-op）
    void unlock();

    // 当前是否持有锁
    bool is_locked() const;

    // 锁文件路径
    const std::string& path() const;

    // 当前锁模式
    Mode mode() const { return mode_; }

    // 更换锁文件路径（自动先释放已持有的锁）
    void set_path(const std::string& path);

private:
    std::string path_;
    void* handle_ = nullptr;  // 平台句柄（Windows: HANDLE，POSIX: 转型的 fd）
    bool locked_ = false;
    Mode mode_ = Mode::Exclusive;
};

// RAII 风格文件锁：构造时尝试/阻塞获取，析构自动释放。
// 阻塞策略与 FileLock 一致；未获得锁时 holds_lock() 为 false，析构安全。
class FileLockGuard
{
public:
    // try=true：尝试获取（不阻塞）；try=false：阻塞等待 timeout_ms
    FileLockGuard(const std::string& path, bool try_lock, int timeout_ms);
    ~FileLockGuard();

    FileLockGuard(const FileLockGuard&) = delete;
    FileLockGuard& operator=(const FileLockGuard&) = delete;

    bool holds_lock() const;

private:
    FileLock lock_;
};

}  // namespace libmini

#endif  // LIBMINI_FILE_LOCK_H
