#include "dir_watcher.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "win_utf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace libmini {

struct DirWatcher::Impl
{
    WatchCallback callback;
    std::string directory;

    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    bool running = false;

#ifdef _WIN32
    HANDLE dir_handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped = {};
    std::vector<std::uint8_t> buffer = std::vector<std::uint8_t>(64 * 1024);
    std::vector<std::uint8_t> backup = std::vector<std::uint8_t>(64 * 1024);
    OVERLAPPED backup_overlapped = {};
    HANDLE stop_event = nullptr;
    bool pending_read = false;
#endif

    ~Impl() { stop_impl(); }

    void stop_impl()
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!running) {
            return;
        }
        running = false;
#ifdef _WIN32
        if (stop_event) {
            ::SetEvent(stop_event);  // 唤醒可能挂在 Wait 上的路径
        }
        if (dir_handle != INVALID_HANDLE_VALUE) {
            // CancelIoEx 取消挂起的 ReadDirectoryChangesW（不关闭句柄，
            // 避免与完成回调竞争）；GetOverlappedResult 阻塞等取消落地，
            // 保证 run_loop 的 GetOverlappedResult 已返回、线程即将退出
            ::CancelIoEx(dir_handle, &overlapped);
            if (pending_read) {
                DWORD bytes = 0;
                ::GetOverlappedResult(dir_handle, &overlapped, &bytes, TRUE);
                pending_read = false;
            }
        }
#endif
        lock.unlock();
        // 工作线程此刻至多在退出路径上，直接 join
        if (worker.joinable()) {
            worker.join();
        }
#ifdef _WIN32
        if (dir_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(dir_handle);
            dir_handle = INVALID_HANDLE_VALUE;
        }
        if (stop_event) {
            ::CloseHandle(stop_event);
            stop_event = nullptr;
        }
#endif
    }

#ifdef _WIN32
    static WatchEvent map_event(DWORD action)
    {
        switch (action) {
            case FILE_ACTION_ADDED:            return WatchEvent::Created;
            case FILE_ACTION_MODIFIED:         return WatchEvent::Modified;
            case FILE_ACTION_REMOVED:          return WatchEvent::Removed;
            case FILE_ACTION_RENAMED_OLD_NAME: return WatchEvent::RenamedOld;
            case FILE_ACTION_RENAMED_NEW_NAME: return WatchEvent::RenamedNew;
            default:                           return WatchEvent::Modified;
        }
    }

    void dispatch(const FILE_NOTIFY_INFORMATION* info)
    {
        WatchCallback cb;
        std::string dir_copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cb = callback;
            dir_copy = directory;
        }
        if (!cb) {
            return;
        }
        WatchNotification n;
        n.event = map_event(info->Action);
        n.dir = dir_copy;
        // 文件名为 UTF-16 半宽字符计数（不含结尾 0）
        const int wlen = static_cast<int>(
            info->FileNameLength / sizeof(WCHAR));
        n.name = internal::wide_to_utf8(
            std::wstring(info->FileName, wlen));
        n.is_dir = false;  // ReadDirectoryChangesW 不直接给出；按文件处理
        cb(n);
    }

    void run_loop()
    {
        for (;;) {
            DWORD bytes = 0;
            // GetOverlappedResult 等待目录变化或取消
            if (!::GetOverlappedResult(dir_handle, &overlapped, &bytes,
                                       TRUE) ||
                bytes == 0) {
                break;  // 句柄关闭/取消/出错
            }

            // 复制快照后解析（回调执行期间不阻塞下一次读取）
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running) {
                    break;
                }
                backup = buffer;
                backup_overlapped = overlapped;
            }
            const std::uint8_t* p = backup.data();
            const std::uint8_t* end = backup.data() + bytes;
            while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
                const FILE_NOTIFY_INFORMATION* info =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                if (info->NextEntryOffset == 0) {
                    dispatch(info);
                    break;
                }
                dispatch(info);
                p += info->NextEntryOffset;
            }

            // 重新投递监听
            std::lock_guard<std::mutex> lock(mutex);
            if (!running) {
                break;
            }
            std::memset(&overlapped, 0, sizeof(overlapped));
            overlapped.hEvent = stop_event;
            DWORD returned = 0;
            const BOOL ok = ::ReadDirectoryChangesW(
                dir_handle, buffer.data(),
                static_cast<DWORD>(buffer.size()), subtree ? TRUE : FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_CREATION,
                &returned, &overlapped, NULL);
            if (!ok) {
                break;
            }
            pending_read = true;
        }
    }

    bool subtree = true;
#endif

    bool start_impl(const std::string& dir, bool watch_subtree)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (running) {
            return false;  // 已在运行；先 stop()
        }
#ifndef _WIN32
        (void)dir;
        (void)watch_subtree;
        return false;  // 非 Windows 平台暂无实现
#else
        if (dir.empty()) {
            return false;
        }
        subtree = watch_subtree;
        dir_handle = ::CreateFileW(
            internal::utf8_to_wide(dir).c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        if (dir_handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        stop_event = ::CreateEventW(NULL, FALSE, FALSE, NULL);  // auto-reset（overlapped 完成通知的标准用法）
        directory = dir;
        running = true;
        lock.unlock();

        // 先投递一次监听，再启动解析线程
        std::memset(&overlapped, 0, sizeof(overlapped));
        overlapped.hEvent = stop_event;
        DWORD returned = 0;
        const BOOL ok = ::ReadDirectoryChangesW(
            dir_handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            subtree ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_CREATION,
            &returned, &overlapped, NULL);
        if (!ok) {
            std::lock_guard<std::mutex> relock(mutex);
            running = false;
            ::CloseHandle(stop_event);
            stop_event = nullptr;
            ::CloseHandle(dir_handle);
            dir_handle = INVALID_HANDLE_VALUE;
            return false;
        }
        pending_read = true;

        worker = std::thread([this]() { run_loop(); });
        return true;
#endif
    }
};

DirWatcher::DirWatcher()
    : impl_(new Impl())
{
}

DirWatcher::~DirWatcher()
{
    delete impl_;
}

void DirWatcher::set_callback(WatchCallback callback)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callback = std::move(callback);
}

bool DirWatcher::start(const std::string& dir, bool watch_subtree)
{
    return impl_->start_impl(dir, watch_subtree);
}

void DirWatcher::stop()
{
    impl_->stop_impl();
}

bool DirWatcher::is_running() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running;
}

const std::string& DirWatcher::directory() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->directory;
}

}  // namespace libmini
