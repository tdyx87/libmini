#include "file_lock.h"

#include <chrono>
#include <thread>

#include "path_utils.h"
#include "win_utf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace libmini {

FileLock::FileLock() = default;

FileLock::FileLock(const std::string& path, Mode mode)
    : path_(path), mode_(mode)
{
}

FileLock::~FileLock()
{
    unlock();
}

const std::string& FileLock::path() const
{
    return path_;
}

void FileLock::set_path(const std::string& path)
{
    unlock();
    path_ = path;
}

bool FileLock::is_locked() const
{
    return locked_;
}

#ifdef _WIN32

bool FileLock::try_lock()
{
    if (locked_) {
        return true;  // 幂等：已持有
    }
    if (path_.empty()) {
        return false;
    }
    if (mode_ == Mode::Shared) {
        return try_lock_shared();
    }
    // 独占共享模式：其他进程（或本进程的其他句柄）无法再打开该文件。
    // 不指定任何共享位即阻止他人以任何模式打开；失败 = 他人持有中。
    HANDLE h = ::CreateFileW(internal::utf8_to_wide(path_).c_str(),
                             GENERIC_READ | GENERIC_WRITE, 0 /* 不共享 */,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    handle_ = h;
    locked_ = true;
    return true;
}

// 共享（读）模式：允许多个读持有者并存，与独占写互斥。
// Windows 用 LockFileEx 的区间锁模拟（锁首字节，共享语义）——
// 注意：对方必须也用 LockFileEx 加锁才互斥；纯 CreateFileW 共享位
// 无法表达读写语义，故共享模式统一走区间锁。
bool FileLock::try_lock_shared()
{
    if (locked_) {
        return true;
    }
    if (path_.empty()) {
        return false;
    }
    // 打开句柄：允许并发打开（共享读写），互斥由区间锁保证
    HANDLE h = ::CreateFileW(internal::utf8_to_wide(path_).c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    OVERLAPPED ov = {};
    if (!::LockFileEx(h, 0 /* 共享锁 */, 0, 1, 0, &ov)) {
        ::CloseHandle(h);
        return false;  // 写者持有中
    }
    handle_ = h;
    locked_ = true;
    return true;
}

#else  // POSIX

bool FileLock::try_lock()
{
    if (locked_) {
        return true;
    }
    if (path_.empty()) {
        return false;
    }
    if (mode_ == Mode::Shared) {
        return try_lock_shared();
    }
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    locked_ = true;
    return true;
}

bool FileLock::try_lock_shared()
{
    if (locked_) {
        return true;
    }
    if (path_.empty()) {
        return false;
    }
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return false;
    }
    if (::flock(fd, LOCK_SH | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    locked_ = true;
    return true;
}

#endif

bool FileLock::lock_for(int timeout_ms)
{
    if (locked_) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (try_lock()) {
            return true;
        }
        if (timeout_ms <= 0 ||
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        // 10ms 轮询：文件锁等待是低频操作，轮询足够且可中断
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void FileLock::unlock()
{
    if (!locked_) {
        return;
    }
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(handle_);
    if (mode_ == Mode::Shared) {
        OVERLAPPED ov = {};
        ::UnlockFileEx(h, 0, 1, 0, &ov);  // 释放共享区间锁
    }
    ::CloseHandle(h);
#else
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    handle_ = nullptr;
    locked_ = false;
}

// ------------------ FileLockGuard ------------------

FileLockGuard::FileLockGuard(const std::string& path, bool try_lock,
                             int timeout_ms)
{
    if (try_lock) {
        lock_.set_path(path);
        lock_.try_lock();
    } else {
        lock_.set_path(path);
        lock_.lock_for(timeout_ms);
    }
}

FileLockGuard::~FileLockGuard() = default;

bool FileLockGuard::holds_lock() const
{
    return lock_.is_locked();
}

}  // namespace libmini
