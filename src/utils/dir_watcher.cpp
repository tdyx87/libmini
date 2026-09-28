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
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <cstdint>
#include <map>

namespace libmini {

#ifndef _WIN32
namespace {

// POSIX 轮询实现的快照条目
struct SnapEntry
{
    bool is_dir = false;
    std::int64_t mtime = 0;
    std::int64_t size = 0;
    std::int64_t inode = 0;
};

bool stat_path(const std::string& p, SnapEntry& e)
{
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) {
        return false;
    }
    e.is_dir = S_ISDIR(st.st_mode) != 0;
    e.mtime = static_cast<std::int64_t>(st.st_mtime);
    e.size = static_cast<std::int64_t>(st.st_size);
    e.inode = static_cast<std::int64_t>(st.st_ino);
    return true;
}

// 递归扫描目录（deep=false 只扫顶层），rel 前缀为相对 base 的路径
void scan_recursive(const std::string& base, const std::string& rel_prefix,
                    bool deep, std::map<std::string, SnapEntry>& out)
{
    const std::string dir_path =
        rel_prefix.empty() ? base : base + "/" + rel_prefix;
    DIR* d = ::opendir(dir_path.c_str());
    if (d == nullptr) {
        return;
    }
    while (struct dirent* de = ::readdir(d)) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string rel =
            rel_prefix.empty() ? name : rel_prefix + "/" + name;
        SnapEntry e;
        if (!stat_path(base + "/" + rel, e)) {
            continue;  // 竞态窗口内被删（或无权限），跳过
        }
        out[rel] = e;
        if (e.is_dir && deep) {
            scan_recursive(base, rel, deep, out);
        }
    }
    ::closedir(d);
}

}  // namespace
#endif

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
#else
    // POSIX：轮询快照 diff。stop 靠 10ms 分片睡眠响应（无系统级唤醒原语）
    bool subtree = true;
    std::map<std::string, SnapEntry> snapshot;

    static void diff_and_dispatch(const std::map<std::string, SnapEntry>& prev,
                                  const std::map<std::string, SnapEntry>& now,
                                  const std::string& dir_copy,
                                  const WatchCallback& cb)
    {
        if (!cb) {
            return;
        }
        WatchNotification n;
        n.dir = dir_copy;
        n.is_dir = false;
        for (std::map<std::string, SnapEntry>::const_iterator it = now.begin();
             it != now.end(); ++it) {
            std::map<std::string, SnapEntry>::const_iterator old =
                prev.find(it->first);
            if (old == prev.end()) {
                n.event = WatchEvent::Created;
                n.name = it->first;
                n.is_dir = it->second.is_dir;
                cb(n);
            } else if (old->second.mtime != it->second.mtime ||
                       old->second.size != it->second.size ||
                       old->second.inode != it->second.inode) {
                if (it->second.is_dir) {
                    continue;  // 目录 mtime 随子项变化，不单独报告
                }
                n.event = WatchEvent::Modified;
                n.name = it->first;
                cb(n);
            }
        }
        for (std::map<std::string, SnapEntry>::const_iterator it =
                 prev.begin();
             it != prev.end(); ++it) {
            if (now.find(it->first) == now.end()) {
                n.event = WatchEvent::Removed;
                n.name = it->first;
                n.is_dir = it->second.is_dir;
                cb(n);
            }
        }
    }

    void posix_run_loop()
    {
        // 起始快照（启动前已存在的内容不报告）
        scan_recursive(directory, "", subtree, snapshot);
        for (;;) {
            // 分片睡眠：响应 stop()（≤10ms）
            for (int i = 0; i < 20; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!running) {
                        return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::map<std::string, SnapEntry> now;
            scan_recursive(directory, "", subtree, now);
            WatchCallback cb;
            std::string dir_copy;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running) {
                    return;
                }
                cb = callback;
                dir_copy = directory;
            }
            diff_and_dispatch(snapshot, now, dir_copy, cb);
            snapshot.swap(now);
        }
    }
#endif

    bool start_impl(const std::string& dir, bool watch_subtree)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (running) {
            return false;  // 已在运行；先 stop()
        }
#ifndef _WIN32
        if (dir.empty()) {
            return false;
        }
        // 确认目录可打开（fail-fast，与 Windows CreateFileW 语义对齐）
        {
            DIR* probe = ::opendir(dir.c_str());
            if (probe == nullptr) {
                return false;
            }
            ::closedir(probe);
        }
        subtree = watch_subtree;
        directory = dir;
        running = true;
        lock.unlock();
        worker = std::thread([this]() { posix_run_loop(); });
        return true;
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
