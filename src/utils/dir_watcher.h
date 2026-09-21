#ifndef LIBMINI_DIR_WATCHER_H
#define LIBMINI_DIR_WATCHER_H

#include <functional>
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

// 目录变化事件类型
enum class WatchEvent {
    Created,  // 新建文件/目录
    Modified, // 内容修改
    Removed,  // 删除
    RenamedOld, // 重命名/移动：旧名（先到）
    RenamedNew, // 重命名/移动：新名（后到）
};

// 目录变化通知。
//   dir      ：被监听的目录（UTF-8）
//   name     ：相对被监听目录的路径（UTF-8）。顶层文件为纯文件名；
//              watch_subtree 下子目录内的变化包含子目录前缀（如 "sub/x.txt"）
//   is_dir   ：变化对象是否为目录
// 回调在监听线程中执行——必须快速返回，耗时工作请转交线程池；
// 回调内不要调用 watcher 的 stop()/析构（会死锁）。
struct WatchNotification
{
    WatchEvent event;
    std::string dir;
    std::string name;
    bool is_dir;
};

using WatchCallback = std::function<void(const WatchNotification&)>;

// 目录变化监听器（Windows：ReadDirectoryChangesW；其他平台当前无实现，
// start 返回 false）。
//
//   DirWatcher watcher;
//   watcher.set_callback([](const WatchNotification& n) {
//       if (n.event == WatchEvent::Modified) reload_config(n.dir + "/" + n.name);
//   });
//   watcher.start("config", /*watch_subtree=*/true);
//   ...  运行 ...
//   watcher.stop();
//
// 支持移动后继续使用：stop() 之后可再次 start()。
// 不可拷贝。
class LIBMINI_API DirWatcher
{
public:
    DirWatcher();
    ~DirWatcher();

    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

    // 设置回调（须在 start 之前调用）
    void set_callback(WatchCallback callback);

    // 开始监听目录。watch_subtree = 是否包含子目录。
    // 目录不存在或平台不支持时返回 false。
    bool start(const std::string& dir, bool watch_subtree = true);

    // 停止监听并回收监听线程（阻塞直到线程退出）。未启动时调用是 no-op。
    void stop();

    // 是否正在监听
    bool is_running() const;

    // 被监听的目录（未启动为空）
    const std::string& directory() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_DIR_WATCHER_H
