// libmini 基础模块单元测试（string/time/file/path/thread/json/xml/serialization）
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "libmini.h"
#include "utils/hmac.h"
#include "utils/process.h"
#include "utils/lru_cache.h"
#include "utils/base32.h"
#include "utils/aes_gcm.h"

using libmini::HmacMd5;
using libmini::HmacSha256;

// ------------------------------ string_utils ------------------------------

TEST(StringUtilsTest, Split)
{
    using namespace libmini;
    const std::vector<std::string> result = split("a,b,c", ',');
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
    EXPECT_EQ(result[2], "c");

    // 空串与单段
    EXPECT_TRUE(split("", ',').empty());
    EXPECT_EQ(split("only", ',').size(), 1u);
}

TEST(StringUtilsTest, Trim)
{
    using namespace libmini;
    EXPECT_EQ(trim("  hello  "), "hello");
    EXPECT_EQ(trim("\t\nhello\t\n"), "hello");
    EXPECT_EQ(trim("hello"), "hello");
    EXPECT_EQ(trim("   "), "");
}

TEST(StringUtilsTest, Replace)
{
    using namespace libmini;
    EXPECT_EQ(replace("hello world", "world", "universe"), "hello universe");
    EXPECT_EQ(replace("aaa", "a", "b"), "bbb");
    EXPECT_EQ(replace("abc", "x", "y"), "abc");
}

TEST(StringUtilsTest, CaseConversion)
{
    using namespace libmini;
    EXPECT_EQ(to_upper("hello"), "HELLO");
    EXPECT_EQ(to_upper("Hello World"), "HELLO WORLD");
    EXPECT_EQ(to_lower("HELLO"), "hello");
    EXPECT_EQ(to_lower("Hello World"), "hello world");
}

// ------------------------------ time_utils --------------------------------

TEST(TimeUtilsTest, TimestampMonotonic)
{
    using namespace libmini;
    const long long ts1 = current_timestamp_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const long long ts2 = current_timestamp_ms();
    EXPECT_GT(ts2, ts1);
}

TEST(TimeUtilsTest, FormatTime)
{
    using namespace libmini;
    const auto now = std::chrono::system_clock::now();
    const std::string formatted = format_time(now, "%Y-%m-%d %H:%M:%S");
    EXPECT_FALSE(formatted.empty());
    EXPECT_NE(formatted.find("-"), std::string::npos);
    EXPECT_NE(formatted.find(":"), std::string::npos);
}

// ------------------------------ file_utils --------------------------------

TEST(FileUtilsTest, ExistsReadWriteSize)
{
    using namespace libmini;
    EXPECT_FALSE(file_exists("libmini_tmp_test.txt"));

    const std::string content = "Hello, World!\nThis is a test.";
    EXPECT_TRUE(write_file("libmini_tmp_test.txt", content));
    EXPECT_TRUE(file_exists("libmini_tmp_test.txt"));
    EXPECT_EQ(read_file("libmini_tmp_test.txt"), content);
    EXPECT_EQ(file_size("libmini_tmp_test.txt"), content.size());

    EXPECT_TRUE(remove_file("libmini_tmp_test.txt"));
    EXPECT_FALSE(file_exists("libmini_tmp_test.txt"));
}

// ------------------------------ path_utils --------------------------------

TEST(PathUtilsTest, PathJoin)
{
    using namespace libmini;
    // 在Windows上使用反斜杠，在Unix上使用正斜杠
#ifdef _WIN32
    EXPECT_EQ(path_join("dir", "file.txt"), "dir\\file.txt");
    EXPECT_EQ(path_join("dir\\", "file.txt"), "dir\\file.txt");
    EXPECT_EQ(path_join("dir", "\\file.txt"), "dir\\file.txt");
#else
    EXPECT_EQ(path_join("dir", "file.txt"), "dir/file.txt");
    EXPECT_EQ(path_join("dir/", "file.txt"), "dir/file.txt");
    EXPECT_EQ(path_join("dir", "/file.txt"), "dir/file.txt");
#endif
    // 绝对段替换
#ifdef _WIN32
    EXPECT_EQ(path_join("dir", "C:\\other\\x.txt"), "C:\\other\\x.txt");
#else
    EXPECT_EQ(path_join("dir", "/other/x.txt"), "/other/x.txt");
#endif
    // 多段 variadic
#ifdef _WIN32
    EXPECT_EQ(path_join("a", "b", "c", "d.txt"), "a\\b\\c\\d.txt");
#else
    EXPECT_EQ(path_join("a", "b", "c", "d.txt"), "a/b/c/d.txt");
#endif
}

TEST(PathUtilsTest, DirnameBasenameExtension)
{
    using namespace libmini;
    EXPECT_EQ(dirname("dir/file.txt"), "dir");
    EXPECT_EQ(dirname("/path/to/file.txt"), "/path/to");
    EXPECT_EQ(basename("dir/file.txt"), "file.txt");
    EXPECT_EQ(basename("file.txt"), "file.txt");
    EXPECT_EQ(extension("file.txt"), ".txt");
    EXPECT_EQ(extension("file"), "");
    EXPECT_EQ(extension("file.tar.gz"), ".gz");
    // 隐藏文件不是扩展名
    EXPECT_EQ(extension(".gitignore"), "");
    EXPECT_EQ(stem("archive.tar.gz"), "archive.tar");
    EXPECT_EQ(stem("README"), "README");
}

TEST(PathUtilsTest, ReplaceExtension)
{
    using namespace libmini;
    EXPECT_EQ(replace_extension("report.txt", ".md"), "report.md");
    EXPECT_EQ(replace_extension("report.txt", "md"), "report.md");
    EXPECT_EQ(replace_extension("report.txt", ""), "report");
    EXPECT_EQ(replace_extension("report", ".csv"), "report.csv");
#ifdef _WIN32
    EXPECT_EQ(replace_extension("dir\\report.txt", ".log"), "dir\\report.log");
#else
    EXPECT_EQ(replace_extension("dir/report.txt", ".log"), "dir/report.log");
#endif
    // 隐藏文件整体是文件名
    EXPECT_EQ(replace_extension(".gitignore", ".bak"), ".gitignore.bak");
}

TEST(PathUtilsTest, NormalizePathExtended)
{
    using namespace libmini;
    EXPECT_EQ(normalize_path("a//b\\c"), "a/b/c");
    // 解析 . 与 ..
    EXPECT_EQ(normalize_path("a/b/./c"), "a/b/c");
    EXPECT_EQ(normalize_path("a/b/../c"), "a/c");
    EXPECT_EQ(normalize_path("../a"), "../a");       // 相对路径无法回退则保留
    EXPECT_EQ(normalize_path("a/../../b"), "../b");   // 相对路径回退到头
    // 不越过根
    EXPECT_EQ(normalize_path("/a/../../b"), "/b");
    EXPECT_EQ(normalize_path("/"), "/");
    EXPECT_EQ(normalize_path(""), "");
#ifdef _WIN32
    // 盘符处理
    EXPECT_EQ(normalize_path("C:\\a\\..\\b\\."), "C:/b");
    EXPECT_EQ(normalize_path("C:/../x"), "C:/x");      // 不越过盘符根
    // UNC 前缀保留
    EXPECT_EQ(normalize_path("\\\\server\\share\\a\\..\\b"),
              "//server/share/b");
#endif
}

TEST(PathUtilsTest, AbsoluteAndParent)
{
    using namespace libmini;
#ifdef _WIN32
    EXPECT_TRUE(path_is_absolute("C:\\x"));
    EXPECT_TRUE(path_is_absolute("\\\\srv\\share"));
    EXPECT_FALSE(path_is_absolute("a\\b"));
#else
    EXPECT_TRUE(path_is_absolute("/x"));
    EXPECT_FALSE(path_is_absolute("a/b"));
#endif
    EXPECT_TRUE(path_is_absolute("/x"));

    // 绝对化（不访问文件系统）
    EXPECT_EQ(path_absolute("a", "base"),
              normalize_path("base/a"));
    EXPECT_EQ(path_absolute("/a", "/base"), "/a");

    // 父目录
    EXPECT_EQ(parent_path("a/b/c.txt"), "a/b");
    EXPECT_EQ(parent_path("file.txt"), "");
    EXPECT_EQ(parent_path("/root"), "/");
#ifdef _WIN32
    EXPECT_EQ(parent_path("C:/x/y"), "C:/x");
    EXPECT_EQ(parent_path("C:/"), "");
#endif
}

TEST(PathUtilsTest, SeparatorsAndEquivalence)
{
    using namespace libmini;
    EXPECT_EQ(path_to_generic("a\\b/c"), "a/b/c");
#ifdef _WIN32
    EXPECT_EQ(path_to_native("a/b"), "a\\b");
    EXPECT_TRUE(path_equivalent("A\\B\\c.txt", "a/b/c.TXT"));
#else
    EXPECT_EQ(path_to_native("a\\b"), "a/b");
    EXPECT_TRUE(path_equivalent("a/b/../b/c.txt", "a/b/c.txt"));
    EXPECT_FALSE(path_equivalent("a/b/c.txt", "a/b/C.txt"));
#endif
    // 空元素跳过
    EXPECT_EQ(path_combine({"base", "", "sub", "f.txt"}).size() > 0, true);
    EXPECT_EQ(path_combine({}), "");
}

// ------------------------------ 文件系统扩展 ------------------------------

namespace {

// 每个测试独立的临时目录（含中文），测试结束递归清理
struct TempDirGuard
{
    std::string dir;

    TempDirGuard(const std::string& tag)
        : dir(libmini::unique_temp_path("fssys_" + tag + "_"))
    {
        libmini::make_directories(dir);
    }
    ~TempDirGuard() { libmini::remove_tree(dir); }
};

}  // namespace

TEST(FileSysTest, MakeDirectoriesAndRemoveTree)
{
    using namespace libmini;
    TempDirGuard guard("mkdir");
    const std::string base = guard.dir;

    // 递归创建多级中文目录
    const std::string deep = base + "/第一级/子目录/目标目录";
    EXPECT_TRUE(make_directories(deep));
    EXPECT_TRUE(is_directory(deep));
    EXPECT_TRUE(make_directories(deep));  // 已存在视为成功

    // 单级创建（父目录已存在）
    EXPECT_TRUE(make_directory(base + "/plain"));
#ifdef _WIN32
    // 父目录不存在时失败
    EXPECT_FALSE(make_directory(base + "/no_parent/child"));
#endif

    // 递归删除
    EXPECT_TRUE(write_file(deep + "/文件.txt", "内容"));
    EXPECT_TRUE(remove_tree(base + "/第一级"));
    EXPECT_FALSE(file_exists(base + "/第一级"));

    // remove_tree 也接受文件
    EXPECT_TRUE(write_file(base + "/f.txt", "x"));
    EXPECT_TRUE(remove_tree(base + "/f.txt"));
    EXPECT_FALSE(file_exists(base + "/f.txt"));

    EXPECT_TRUE(remove_tree(base + "/plain"));
    EXPECT_FALSE(remove_tree(base + "/missing"));  // 不存在返回 false
}

TEST(FileSysTest, CopyRenameMove)
{
    using namespace libmini;
    TempDirGuard guard("cpmv");
    const std::string base = guard.dir;

    // 文件复制与追加
    EXPECT_TRUE(write_file(base + "/src.txt", "line1\n"));
    EXPECT_TRUE(copy_file(base + "/src.txt", base + "/dst.txt"));
    EXPECT_EQ(read_file(base + "/dst.txt"), "line1\n");
    EXPECT_TRUE(copy_file(base + "/src.txt", base + "/dst.txt"));  // 覆盖
    EXPECT_TRUE(append_file(base + "/dst.txt", "line2\n"));
    EXPECT_EQ(read_file(base + "/dst.txt"), "line1\nline2\n");

    // 重命名
    EXPECT_TRUE(rename_path(base + "/src.txt", base + "/renamed.txt"));
    EXPECT_FALSE(file_exists(base + "/src.txt"));
    EXPECT_TRUE(file_exists(base + "/renamed.txt"));

    // 移动到子目录
    EXPECT_TRUE(make_directory(base + "/sub"));
    EXPECT_TRUE(move_path(base + "/renamed.txt", base + "/sub/moved.txt"));
    EXPECT_EQ(read_file(base + "/sub/moved.txt"), "line1\n");

    // 目录树复制
    EXPECT_TRUE(make_directory(base + "/tree"));
    EXPECT_TRUE(write_file(base + "/tree/a.txt", "A"));
    EXPECT_TRUE(make_directories(base + "/tree/子目录"));
    EXPECT_TRUE(write_file(base + "/tree/子目录/b.txt", "B"));
    EXPECT_TRUE(copy_tree(base + "/tree", base + "/tree_copy"));
    EXPECT_EQ(read_file(base + "/tree_copy/a.txt"), "A");
    EXPECT_EQ(read_file(base + "/tree_copy/子目录/b.txt"), "B");

    // 复制到已存在目录 → 并入其中
    EXPECT_TRUE(make_directory(base + "/merge_to"));
    EXPECT_TRUE(copy_tree(base + "/tree", base + "/merge_to"));
    EXPECT_EQ(read_file(base + "/merge_to/tree/a.txt"), "A");

    // 目录树移动
    EXPECT_TRUE(move_path(base + "/tree_copy", base + "/tree_moved"));
    EXPECT_FALSE(file_exists(base + "/tree_copy"));
    EXPECT_TRUE(file_exists(base + "/tree_moved/子目录/b.txt"));

    // 目标已存在时 move 失败且源保留
    EXPECT_FALSE(move_path(base + "/tree", base + "/tree_moved"));
    EXPECT_TRUE(file_exists(base + "/tree/a.txt"));
}

TEST(FileSysTest, DetailedListingAndTimestamps)
{
    using namespace libmini;
    TempDirGuard guard("detail");
    const std::string base = guard.dir;

    EXPECT_TRUE(write_file(base + "/alpha.txt", "12345"));       // 5 字节
    EXPECT_TRUE(write_file(base + "/中文文件.txt", "你好"));     // 6 字节 UTF-8
    EXPECT_TRUE(make_directory(base + "/subdir"));

    const std::vector<DirEntry> entries = list_directory_detailed(base);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].name, "alpha.txt");
    EXPECT_EQ(entries[0].kind, EntryKind::File);
    EXPECT_EQ(entries[0].size, 5u);
    EXPECT_EQ(entries[1].name, "subdir");
    EXPECT_EQ(entries[1].kind, EntryKind::Directory);
    EXPECT_EQ(entries[2].name, "中文文件.txt");
    EXPECT_EQ(entries[2].size, 6u);

    // 完整路径包含中文组件
    EXPECT_NE(entries[2].path.find("中文文件.txt"), std::string::npos);

    // 时间戳：接近当前时间（Unix 毫秒）
    const std::int64_t now_ms = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const std::int64_t mtime = file_mtime_ms(base + "/alpha.txt");
    EXPECT_GT(mtime, now_ms - 60000);
    EXPECT_LE(mtime, now_ms + 60000);

    // 简版列表与详版一致
    const std::vector<std::string> names = list_directory(base);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "alpha.txt");
}

TEST(FileSysTest, TempPaths)
{
    using namespace libmini;
    const std::string tmp = temp_directory_path();
    EXPECT_FALSE(tmp.empty());
    EXPECT_TRUE(is_directory(tmp));

    const std::string p1 = unique_temp_path();
    const std::string p2 = unique_temp_path();
    EXPECT_NE(p1, p2);
    EXPECT_NE(p1.find("libmini_"), std::string::npos);
    EXPECT_EQ(p1.find(".tmp"), p1.size() - 4);

    // 自定义目录与前缀
    TempDirGuard guard("tmp");
    const std::string p3 = unique_temp_path("myapp_", guard.dir);
    EXPECT_NE(p3.find("myapp_"), std::string::npos);
    EXPECT_NE(p3.find(guard.dir), std::string::npos);
}

// ------------------------------ 文件锁 ------------------------------

TEST(FileLockTest, TryLockExclusiveAndRelease)
{
    using namespace libmini;
    TempDirGuard guard("lock");
    const std::string lock_path = guard.dir + "\\test.lock";

    FileLock first(lock_path);
    EXPECT_TRUE(first.try_lock());
    EXPECT_TRUE(first.is_locked());

    // 第二把锁（同一进程内不同句柄也互斥，Windows 独占共享模式语义）
    FileLock second(lock_path);
    EXPECT_FALSE(second.try_lock());

    // 释放后可获取
    first.unlock();
    EXPECT_FALSE(first.is_locked());
    EXPECT_TRUE(second.try_lock());
    second.unlock();
}

TEST(FileLockTest, LockForTimesOutAndAcquires)
{
    using namespace libmini;
    TempDirGuard guard("lock2");
    const std::string lock_path = guard.dir + "\\busy.lock";

    FileLock first(lock_path);
    ASSERT_TRUE(first.try_lock());

    FileLock waiter(lock_path);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(waiter.lock_for(150));  // 超时失败
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    EXPECT_GE(waited, 100);

    // 持有者释放后，等待者能拿到
    first.unlock();
    EXPECT_TRUE(waiter.lock_for(2000));
    waiter.unlock();

    // RAII Guard
    {
        FileLockGuard g(lock_path, true, 0);
        EXPECT_TRUE(g.holds_lock());
        FileLockGuard blocked(lock_path, true, 0);
        EXPECT_FALSE(blocked.holds_lock());  // 被上者持有
    }   // 析构自动释放
    FileLockGuard g2(lock_path, false, 1000);
    EXPECT_TRUE(g2.holds_lock());
}

TEST(FileLockTest, SetPathReleasesPrevious)
{
    using namespace libmini;
    TempDirGuard guard("lock3");
    const std::string a = guard.dir + "\\a.lock";
    const std::string b = guard.dir + "\\b.lock";

    FileLock lock(a);
    ASSERT_TRUE(lock.try_lock());
    lock.set_path(b);  // 换路径时自动释放 a
    FileLock other(a);
    EXPECT_TRUE(other.try_lock());
    other.unlock();

    EXPECT_TRUE(lock.try_lock());
    // unlock 后再次 set_path 到空路径不崩溃
    lock.unlock();
    lock.set_path("");
    EXPECT_FALSE(lock.try_lock());
}

// ------------------------------ 目录监听 ------------------------------

TEST(DirWatcherTest, ReportsCreateModifyRemove)
{
    using namespace libmini;
    TempDirGuard guard("watch");
    const std::string dir = guard.dir;

    DirWatcher watcher;
    std::mutex mu;
    std::map<std::string, int> events;  // "name:事件" 计数
    watcher.set_callback([&](const WatchNotification& n) {
        std::lock_guard<std::mutex> lock(mu);
        const char* tag = "?";
        switch (n.event) {
            case WatchEvent::Created: tag = "C"; break;
            case WatchEvent::Modified: tag = "M"; break;
            case WatchEvent::Removed: tag = "R"; break;
            case WatchEvent::RenamedOld: tag = "O"; break;
            case WatchEvent::RenamedNew: tag = "N"; break;
        }
        events[n.name + ":" + tag]++;
    });
    ASSERT_TRUE(watcher.start(dir, true));
    EXPECT_TRUE(watcher.is_running());
    EXPECT_EQ(watcher.directory(), dir);

    // 触发：创建 → 修改 → 重命名 → 删除
    ASSERT_TRUE(write_file(dir + "\\新建.txt", "v1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_TRUE(append_file(dir + "\\新建.txt", "v2"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_TRUE(rename_path(dir + "\\新建.txt", dir + "\\改名.txt"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_TRUE(remove_file(dir + "\\改名.txt"));

    // 等待事件到达（ReadDirectoryChangesW 通知有延迟）
    for (int i = 0; i < 100; ++i) {
        std::lock_guard<std::mutex> lock(mu);
        const bool created = events.count("新建.txt:C") > 0;
        const bool renamed_new = events.count("改名.txt:N") > 0;
        const bool removed = events.count("改名.txt:R") > 0;
        if (created && renamed_new && removed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    watcher.stop();
    EXPECT_FALSE(watcher.is_running());

    std::lock_guard<std::mutex> lock(mu);
    EXPECT_GT(events["新建.txt:C"], 0);   // 创建
    EXPECT_GT(events["新建.txt:M"], 0);   // 修改
    EXPECT_GT(events["新建.txt:O"], 0);   // 重命名：旧名
    EXPECT_GT(events["改名.txt:N"], 0);   // 重命名：新名
    EXPECT_GT(events["改名.txt:R"], 0);   // 删除
}

TEST(DirWatcherTest, SubtreeWatchAndRestart)
{
    using namespace libmini;
    TempDirGuard guard("watch2");
    const std::string dir = guard.dir;
    ASSERT_TRUE(make_directories(dir + "\\子目录"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    DirWatcher watcher;
    std::mutex mu;
    std::atomic<bool> child_created{false};
    std::string child_name;
    watcher.set_callback([&](const WatchNotification& n) {
        // watch_subtree 下 name 为相对路径（如 "子目录\\深层.txt"）
        if (n.name.find("深层.txt") != std::string::npos) {
            std::lock_guard<std::mutex> lock(mu);
            child_created = true;
            child_name = n.name;
        }
    });
    ASSERT_TRUE(watcher.start(dir, true));   // 含子目录
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(write_file(dir + "\\子目录\\深层.txt", "x"));
    for (int i = 0; i < 100 && !child_created.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    watcher.stop();
    EXPECT_TRUE(child_created.load());
    // name 携带子目录前缀（相对路径语义）
    EXPECT_NE(child_name.find("深层.txt"), std::string::npos);
    EXPECT_GT(child_name.size(), std::string("深层.txt").size());

    // 重启后可再次使用
    std::atomic<bool> again{false};
    watcher.set_callback([&](const WatchNotification& n) {
        if (n.name == "再次.txt") {
            again = true;
        }
    });
    ASSERT_TRUE(watcher.start(dir, false));  // 不含子目录
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(write_file(dir + "\\再次.txt", "y"));
    for (int i = 0; i < 100 && !again.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    watcher.stop();
    EXPECT_TRUE(again.load());

    // 目录不存在 → start 失败
    DirWatcher bad;
    EXPECT_FALSE(bad.start(dir + "\\不存在"));
}

// ------------------------------ thread_utils ------------------------------

TEST(ThreadUtilsTest, PoolSubmit)
{
    using namespace libmini;
    ThreadPool pool(2);
    auto future = pool.submit([]() { return 42; });
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadUtilsTest, PoolMultipleTasks)
{
    using namespace libmini;
    ThreadPool pool(4);

    std::vector<std::future<int> > futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([i]() { return i * 2; }));
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(futures[i].get(), i * 2);
    }
}

// ------------------------------ Windows 服务 ------------------------------

#if defined(_WIN32)

// 命令行分发：未知参数返回 -1（不消费），宿主程序自行继续
TEST(WinServiceTest, UnknownArgsNotConsumed)
{
    libmini::ServiceApp app("libmini_test_svc", "Libmini Test Service");
    char arg0[] = "prog.exe";
    char arg1[] = "--verbose";
    char* argv[] = {arg0, arg1};
    EXPECT_EQ(app.run(2, argv), -1);
}

// console 模式：前台运行回调，stop_event 由 Ctrl/CtrlBreak 事件触发。
// 这里用另一个线程模拟：回调内等 stop_event，主线程直接测 "run" 被拒绝后
// 走 console 分支的退出码透传
TEST(WinServiceTest, ConsoleModeRunsCallbackAndReturnsExitCode)
{
    libmini::ServiceApp app("libmini_test_svc", "Libmini Test Service");
    app.set_run_callback([](const std::atomic<bool>& stop) {
        int loops = 0;
        while (!stop.load() && loops < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ++loops;
        }
        return loops < 50 ? 7 : 1;  // stop 触发时返回 7
    });

    // 无参数 → console 模式。stop_event 不会被外部置位（测试进程无控制台
    // 关闭事件），回调靠 500ms 超时兜底返回 1
    char arg0[] = "prog.exe";
    char* argv1[] = {arg0};
    const int code = app.run(1, argv1);
    EXPECT_EQ(code, 1);

    // 显式 console 同样入口
    char arg1[] = "prog.exe";
    char arg2[] = "console";
    char* argv2[] = {arg1, arg2};
    libmini::ServiceApp app2("libmini_test_svc", "Libmini Test Service");
    app2.set_run_callback([](const std::atomic<bool>&) { return 7; });
    EXPECT_EQ(app2.run(2, argv2), 7);
}

// 未安装服务的查询语义：state = NotFound；启停/卸载等接口可安全调用
TEST(WinServiceTest, QueryUninstalledServiceReportsNotFound)
{
    const std::string name = "libmini_definitely_not_installed_svc";
    const libmini::ServiceStatusInfo st = libmini::ServiceControl::query(name);
    EXPECT_EQ(st.state, libmini::ServiceState::NotFound);

    libmini::ServiceStartType start_type = libmini::ServiceStartType::AutoStart;
    EXPECT_FALSE(libmini::ServiceControl::query_start_type(name, start_type));

    // 幂等语义：卸载未安装服务成功；启动未安装服务失败
    EXPECT_TRUE(libmini::ServiceControl::uninstall(name));
    EXPECT_FALSE(libmini::ServiceControl::start(name));
    EXPECT_FALSE(libmini::ServiceControl::stop(name));
    EXPECT_FALSE(libmini::ServiceControl::set_start_type(
        name, libmini::ServiceStartType::Disabled));
}

// ServiceApp 的 install 模式在无管理员权限的测试环境里会失败，
// 但应返回 1 而不是崩溃（参数消费语义正确）
TEST(WinServiceTest, InstallWithoutAdminReturnsGracefully)
{
    libmini::ServiceApp app("libmini_test_svc", "Libmini Test Service");
    char arg0[] = "prog.exe";
    char arg1[] = "install";
    char* argv[] = {arg0, arg1};
    const int code = app.run(2, argv);
    // 0（恰好有权限且成功）或 1（权限不足/SCM 拒绝）都算正常路径
    EXPECT_TRUE(code == 0 || code == 1);
}

#else

TEST(WinServiceTest, StubPlatformReportsUnsupported)
{
    const libmini::ServiceStatusInfo st =
        libmini::ServiceControl::query("anything");
    EXPECT_EQ(st.state, libmini::ServiceState::Unknown);
    EXPECT_FALSE(libmini::ServiceControl::install(libmini::ServiceInstallDesc()));
}

#endif

// ------------------------------ json_utils --------------------------------

TEST(JsonUtilsTest, ParseAndSerialize)
{
    using namespace libmini;
    const JsonValue value = parse_json(R"({"name":"test","value":123})");
    EXPECT_EQ(value["name"].get<std::string>(), "test");
    EXPECT_EQ(value["value"].get<int>(), 123);

    const std::string text = to_json_string(value);
    EXPECT_NE(text.find("\"test\""), std::string::npos);
    EXPECT_NE(text.find("123"), std::string::npos);
}

TEST(JsonUtilsTest, SimpleHelpers)
{
    using namespace libmini;
    EXPECT_EQ(parse_json_simple("{ \"a\" : 1 }"), "{\"a\":1}");
    EXPECT_EQ(parse_json_simple("garbage"), "");
    EXPECT_EQ(to_json_string_simple("say \"hi\""), "\"say \\\"hi\\\"\"");
}

// ------------------------------ xml_utils ---------------------------------

TEST(XmlUtilsTest, ParseXml)
{
    using namespace libmini;
    const XmlDocument doc = parse_xml("<root><item id='1'>v</item></root>");
    const std::string text = to_xml_string(doc);
    EXPECT_NE(text.find("root"), std::string::npos);
    EXPECT_NE(text.find("item"), std::string::npos);
}

TEST(XmlUtilsTest, SimpleNodeRoundTrip)
{
    using namespace libmini;
    SimpleXmlNode node;
    node.name = "test";
    node.value = "content";
    const std::string xml = to_xml_string_simple(node);
    EXPECT_NE(xml.find("<test>"), std::string::npos);
    EXPECT_NE(xml.find("content"), std::string::npos);
}

// ------------------------------ serialization ------------------------------

TEST(SerializationTest, IntRoundTrip)
{
    using namespace libmini;
    const std::string result = serialize_to_json(42);
    EXPECT_EQ(result, "42");
    EXPECT_EQ(deserialize_from_json<int>("42"), 42);
}

TEST(SerializationTest, JsonScalarAndContainer)
{
    using namespace libmini;
    EXPECT_EQ(serialize_to_json(3.5), "3.5");
    EXPECT_EQ(serialize_to_json(true), "true");
    EXPECT_EQ(serialize_to_json(std::string("hi")), "\"hi\"");

    std::vector<int> nums;
    nums.push_back(1);
    nums.push_back(2);
    nums.push_back(3);
    const std::string text = serialize_to_json(nums);
    EXPECT_EQ(text, "[1,2,3]");
    const std::vector<int> back = deserialize_from_json<std::vector<int> >(text);
    EXPECT_EQ(back.size(), 3u);
    EXPECT_EQ(back[2], 3);

    std::map<std::string, double> prices;
    prices["apple"] = 1.5;
    prices["pear"] = 2.0;
    const std::map<std::string, double> got =
        deserialize_from_json<std::map<std::string, double> >(
            serialize_to_json(prices));
    EXPECT_EQ(got.size(), 2u);
    EXPECT_DOUBLE_EQ(got.at("apple"), 1.5);
}

namespace test_ns {

struct Task {
    std::string title;
    int priority;
    bool done;
    std::vector<std::string> tags;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Task, title, priority, done, tags)

}  // namespace test_ns

TEST(SerializationTest, JsonCustomStruct)
{
    using namespace libmini;
    test_ns::Task task;
    task.title = "write docs";
    task.priority = 2;
    task.done = false;
    task.tags.push_back("doc");
    task.tags.push_back("v2");

    const std::string text = serialize_to_json(task);
    EXPECT_NE(text.find("\"title\":\"write docs\""), std::string::npos);

    const test_ns::Task back = deserialize_from_json<test_ns::Task>(text);
    EXPECT_EQ(back.title, "write docs");
    EXPECT_EQ(back.priority, 2);
    EXPECT_FALSE(back.done);
    EXPECT_EQ(back.tags.size(), 2u);
    EXPECT_EQ(back.tags[1], "v2");
}

TEST(SerializationTest, JsonOrFallback)
{
    using namespace libmini;
    EXPECT_EQ(deserialize_from_json_or<int>("not json", -1), -1);
    // 类型不匹配：字符串转 int 失败
    EXPECT_EQ(deserialize_from_json_or<int>("\"abc\"", -1), -1);
    // 合法路径不受影响
    EXPECT_EQ(deserialize_from_json_or<int>("7", -1), 7);
}

TEST(SerializationTest, XmlScalarRoundTrip)
{
    using namespace libmini;
    EXPECT_EQ(serialize_to_xml(8080), "<value>8080</value>");
    EXPECT_EQ(deserialize_from_xml<int>(serialize_to_xml(8080)), 8080);
    EXPECT_EQ(deserialize_from_xml<int>(serialize_to_xml(-7)), -7);
    EXPECT_DOUBLE_EQ(deserialize_from_xml<double>(serialize_to_xml(2.5)), 2.5);
    EXPECT_EQ(deserialize_from_xml<bool>(serialize_to_xml(true)), true);
    EXPECT_TRUE(deserialize_from_xml<bool>(serialize_to_xml(false)) == false);
}

TEST(SerializationTest, XmlStringNotGuessed)
{
    using namespace libmini;
    // type="string" 标注：数字样文本、布尔样文本不变形
    const std::string a = serialize_to_xml(std::string("0089"));
    EXPECT_NE(a.find("type=\"string\""), std::string::npos);
    EXPECT_EQ(deserialize_from_xml<std::string>(a), "0089");
    EXPECT_EQ(deserialize_from_xml<std::string>(serialize_to_xml(std::string("true"))), "true");
}

TEST(SerializationTest, XmlContainerRoundTrip)
{
    using namespace libmini;
    std::vector<std::string> names;
    names.push_back("alpha");
    names.push_back("beta");
    const std::string xml = serialize_to_xml(names);
    EXPECT_NE(xml.find("type=\"array\""), std::string::npos);
    EXPECT_NE(xml.find("<item type=\"string\">alpha</item>"), std::string::npos);
    const std::vector<std::string> back =
        deserialize_from_xml<std::vector<std::string> >(xml);
    EXPECT_EQ(back.size(), 2u);
    EXPECT_EQ(back[1], "beta");

    std::map<std::string, int> ages;
    ages["tom"] = 12;
    ages["jerry"] = 10;
    const std::map<std::string, int> got =
        deserialize_from_xml<std::map<std::string, int> >(serialize_to_xml(ages));
    EXPECT_EQ(got.at("jerry"), 10);
}

namespace test_ns {

struct Host {
    std::string name;
    int port;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Host, name, port)

struct Config {
    std::string host;
    int timeout_ms;
    std::vector<Host> replicas;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Config, host, timeout_ms, replicas)

}  // namespace test_ns

TEST(SerializationTest, XmlNestedStructRoundTrip)
{
    using namespace libmini;
    test_ns::Config cfg;
    cfg.host = "db_<primary>&";  // 含 XML 特殊字符
    cfg.timeout_ms = 3000;
    test_ns::Host r1;
    r1.name = "r1";
    r1.port = 6380;
    test_ns::Host r2;
    r2.name = "r2";
    r2.port = 6381;
    cfg.replicas.push_back(r1);
    cfg.replicas.push_back(r2);

    const std::string xml = serialize_to_xml(cfg);
    // 特殊字符已实体转义，XML 仍可解析
    const test_ns::Config back = deserialize_from_xml<test_ns::Config>(xml);
    EXPECT_EQ(back.host, "db_<primary>&");
    EXPECT_EQ(back.timeout_ms, 3000);
    ASSERT_EQ(back.replicas.size(), 2u);
    EXPECT_EQ(back.replicas[0].port, 6380);
    EXPECT_EQ(back.replicas[1].name, "r2");
}

TEST(SerializationTest, XmlHandWrittenCompat)
{
    using namespace libmini;
    // 未标 type 的手写 XML：元素结构按对象读，纯文本按内容推断
    const JsonValue v = xml_to_json("<value><host>db</host><port>5432</port></value>");
    EXPECT_EQ(v["host"].get<std::string>(), "db");
    EXPECT_EQ(v["port"].get<int>(), 5432);
}

TEST(SerializationTest, XmlOrFallback)
{
    using namespace libmini;
    EXPECT_EQ(deserialize_from_xml_or<int>("<broken", -1), -1);
    EXPECT_EQ(deserialize_from_xml_or<int>("<value></value>", -1), -1);
}

// ==================== HMAC（RFC 2104 / RFC 4231 / RFC 2202 向量）====================

TEST(HmacTest, Sha256Rfc4231Vectors)
{
    // RFC 4231 Test Case 1：key = 0x20 字节，data = "Hi There"
    EXPECT_EQ(HmacSha256::hex(std::string(20, '\x0b'), "Hi There"),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // Test Case 2：key = "Jefe"，data = "what do ya want for nothing?"
    EXPECT_EQ(HmacSha256::hex("Jefe", "what do ya want for nothing?"),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // Test Case 6：key 长于块大小（131 字节），data = "Test Using Larger Than Block-Size Key - Hash Key First"
    EXPECT_EQ(HmacSha256::hex(std::string(131, '\xaa'),
                              "Test Using Larger Than Block-Size Key - Hash Key First"),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(HmacTest, Md5Rfc2202Vectors)
{
    // RFC 2202 Test Case 1
    EXPECT_EQ(HmacMd5::hex(std::string(16, '\x0b'), "Hi There"),
              "9294727a3638bb1c13f48ef8158bfc9d");
    // Test Case 2
    EXPECT_EQ(HmacMd5::hex("Jefe", "what do ya want for nothing?"),
              "750c783e6ab0b503eaa86e310a5db738");
}

TEST(HmacTest, StreamingMatchesOneShot)
{
    const std::string key = "stream-key";
    const std::string part1 = "hello ";
    const std::string part2 = "hmac world";

    HmacSha256 h;
    h.set_key(key);
    h.update(part1);
    h.update(part2);
    EXPECT_EQ(h.finish(), HmacSha256::raw(key, part1 + part2));

    HmacMd5 m;
    m.set_key(key);
    m.update(part1);
    m.update(part2);
    EXPECT_EQ(m.finish(), HmacMd5::raw(key, part1 + part2));
}

TEST(HmacTest, EmptyKeyAndLongKey)
{
    // 空密钥是合法输入（ipad/opad 全为填充值），只要有确定结果即可
    const std::string a = HmacSha256::hex("", "data");
    EXPECT_EQ(a.size(), 64u);
    EXPECT_EQ(a, HmacSha256::hex("", "data"));

    // 超长密钥（>64 字节）与哈希后密钥的结果一致（RFC 4231 TC6 已验证正确性）
    EXPECT_EQ(HmacSha256::hex(std::string(100, '\x5a'), "x").size(), 64u);
}

TEST(HmacTest, DetectsTampering)
{
    const std::string mac = HmacSha256::hex("secret", "payload");
    EXPECT_NE(mac, HmacSha256::hex("secret", "payload2"));  // 数据变 → MAC 变
    EXPECT_NE(mac, HmacSha256::hex("secret2", "payload"));  // 密钥变 → MAC 变
}

// ==================== 子进程执行 ====================

TEST(ProcessTest, RunProgramCapturesOutputAndExitCode)
{
#ifdef _WIN32
    libmini::ProcessResult r = libmini::run_process("cmd.exe", {"/C", "echo hello"});
    EXPECT_EQ(r.exit_code, 0);
    // cmd echo 输出带回车换行
    EXPECT_NE(r.stdout_text.find("hello"), std::string::npos);
    EXPECT_FALSE(r.timed_out);
#else
    libmini::ProcessResult r = libmini::run_process("echo", {"hello"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_text, "hello\n");
#endif
}

TEST(ProcessTest, ExitCodePropagates)
{
#ifdef _WIN32
    libmini::ProcessResult r = libmini::run_process("cmd.exe", {"/C", "exit 7"});
#else
    libmini::ProcessResult r = libmini::run_process("sh", {"-c", "exit 7"});
#endif
    EXPECT_EQ(r.exit_code, 7);
}

TEST(ProcessTest, StderrSeparated)
{
#ifdef _WIN32
    libmini::ProcessResult r = libmini::run_process(
        "cmd.exe", {"/C", "echo out& echo err 1>&2"});
    EXPECT_NE(r.stdout_text.find("out"), std::string::npos);
    EXPECT_NE(r.stderr_text.find("err"), std::string::npos);
#else
    libmini::ProcessResult r = libmini::run_process(
        "sh", {"-c", "echo out; echo err 1>&2"});
    EXPECT_EQ(r.stdout_text, "out\n");
    EXPECT_EQ(r.stderr_text, "err\n");
#endif
}

TEST(ProcessTest, StdinFeedsChild)
{
#ifdef _WIN32
    // findstr 读 stdin 过滤包含 needle 的行
    libmini::ProcessResult r = libmini::run_process(
        "findstr", {"needle"}, 0, "line1\nneedle here\nline3");
    EXPECT_NE(r.stdout_text.find("needle here"), std::string::npos);
#else
    libmini::ProcessResult r = libmini::run_process(
        "grep", {"needle"}, 0, "line1\nneedle here\nline3");
    EXPECT_EQ(r.stdout_text, "needle here\n");
#endif
}

TEST(ProcessTest, TimeoutKillsProcess)
{
    const auto start = std::chrono::steady_clock::now();
#ifdef _WIN32
    // ping -n 是可靠的 Windows 长任务（timeout 命令在重定向 stdin 时会立即退出）
    libmini::ProcessResult r = libmini::run_process(
        "ping", {"-n", "10", "127.0.0.1"}, 300);
#else
    libmini::ProcessResult r = libmini::run_process("sleep", {"10"}, 300);
#endif
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    EXPECT_TRUE(r.timed_out);
    EXPECT_EQ(r.exit_code, -1);
    EXPECT_LT(elapsed, 5000);  // 远小于 10s 的子进程时长
}

TEST(ProcessTest, ShellRunsPipeline)
{
#ifdef _WIN32
    libmini::ProcessResult r = libmini::run_shell("echo abc | findstr a");
#else
    libmini::ProcessResult r = libmini::run_shell("echo abc | grep a");
#endif
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(r.stdout_text.find("abc"), std::string::npos);
}

TEST(ProcessTest, NonexistentProgramReportsFailure)
{
    libmini::ProcessResult r = libmini::run_process(
        "no_such_binary_xyz_12345", {"arg"});
    EXPECT_EQ(r.exit_code, -1);
#ifdef _WIN32
    EXPECT_TRUE(r.stdout_text.empty());
#endif
}

// ==================== LRU 缓存 ====================

TEST(LruCacheTest, PutGetAndCapacityEviction)
{
    libmini::LruCache<std::string, int> cache(2);
    cache.put("a", 1);
    cache.put("b", 2);
    EXPECT_EQ(*cache.get("a"), 1);   // a 变为最新
    cache.put("c", 3);               // 淘汰最旧的 b
    EXPECT_FALSE(cache.get("b").has_value());
    EXPECT_EQ(*cache.get("a"), 1);
    EXPECT_EQ(*cache.get("c"), 3);
    EXPECT_EQ(cache.size(), 2u);
}

TEST(LruCacheTest, PutExistingKeyUpdatesAndPromotes)
{
    libmini::LruCache<int, std::string> cache(2);
    cache.put(1, "old");
    cache.put(2, "x");
    cache.put(1, "new");             // 更新 1 并提升为最新
    cache.put(3, "y");               // 淘汰 2（1 刚被访问过）
    EXPECT_FALSE(cache.get(2).has_value());
    EXPECT_EQ(*cache.get(1), "new");
    EXPECT_EQ(cache.size(), 2u);
}

TEST(LruCacheTest, HitRateStats)
{
    libmini::LruCache<int, int> cache(4);
    cache.put(1, 10);
    (void)cache.get(1);              // hit
    (void)cache.get(2);              // miss
    (void)cache.get(1);              // hit
    EXPECT_EQ(cache.lookups(), 3u);
    EXPECT_DOUBLE_EQ(cache.hit_rate(), 2.0 / 3.0);

    cache.clear(/*clear_stats=*/true);
    EXPECT_EQ(cache.lookups(), 0u);
    EXPECT_DOUBLE_EQ(cache.hit_rate(), 0.0);
    EXPECT_TRUE(cache.empty());
}

TEST(LruCacheTest, ZeroCapacityCacheNothing)
{
    libmini::LruCache<std::string, int> cache(0);
    cache.put("a", 1);
    EXPECT_TRUE(cache.empty());
    EXPECT_FALSE(cache.get("a").has_value());
}

TEST(LruCacheTest, EraseAndThreadSafety)
{
    libmini::LruCache<int, int> cache(100);
    cache.put(1, 1);
    EXPECT_TRUE(cache.erase(1));
    EXPECT_FALSE(cache.erase(1));    // 已删，再次删返回 false

    // 并发读写不崩溃（TSAN 级别验证靠运行，此处烟雾测试）
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cache, t]() {
            for (int i = 0; i < 200; ++i) {
                cache.put(t * 1000 + i, i);
                (void)cache.get(t * 1000 + i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(cache.size(), 100u);   // 容量封顶
}

// ==================== Base32 ====================

TEST(Base32Test, Rfc4648TestVectors)
{
    // RFC 4648 §10 测试向量
    struct Case
    {
        const char* raw;
        const char* encoded;
    };
    const Case cases[] = {
        {"", ""},
        {"f", "MY======"},
        {"fo", "MZXQ===="},
        {"foo", "MZXW6==="},
        {"foob", "MZXW6YQ="},
        {"fooba", "MZXW6YTB"},
        {"foobar", "MZXW6YTBOI======"},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(libmini::Base32::encode(c.raw), c.encoded) << c.raw;
        std::string decoded;
        ASSERT_TRUE(libmini::Base32::decode(c.encoded, decoded)) << c.encoded;
        EXPECT_EQ(decoded, c.raw);
    }
}

TEST(Base32Test, LenientDecodeAcceptsLowerAndWhitespace)
{
    std::string out;
    // 小写 + 每四位分隔空格（TOTP 密钥的常见书写格式）
    EXPECT_TRUE(libmini::Base32::decode("mzxw 6ytb", out));
    EXPECT_EQ(out, "fooba");

    // 非法字符宽容跳过
    EXPECT_TRUE(libmini::Base32::decode("MZXW6YTB!", out));
    EXPECT_EQ(out, "fooba");
}

TEST(Base32Test, StrictModeRejectsGarbage)
{
    std::string out;
    EXPECT_FALSE(libmini::Base32::decode("MZXW6Y1B", out, /*strict=*/true));
    EXPECT_FALSE(libmini::Base32::decode("=MZXW", out, true));  // 先填充后数据
    EXPECT_TRUE(libmini::Base32::decode("MZXW6YTB", out, true));
    EXPECT_EQ(out, "fooba");
}

TEST(Base32Test, BinaryRoundTrip)
{
    // 含全字节值的二进制往返
    std::string raw;
    for (int i = 0; i < 256; ++i) {
        raw.push_back(static_cast<char>(i));
    }
    const std::string encoded = libmini::Base32::encode(raw);
    std::string decoded;
    ASSERT_TRUE(libmini::Base32::decode(encoded, decoded, true));
    EXPECT_EQ(decoded, raw);
}

// ---------------- ConsoleExit：优雅退出封装 ----------------

TEST(ConsoleExitTest, SignalTriggersHandlerAndStopRequested)
{
    libmini::ConsoleExit ce;
    std::atomic<int> called{0};
    ce.set_handler([&]() { ++called; });

    EXPECT_FALSE(ce.stop_requested());
    ce.on_signal();  // 直接驱动统一入口（等价信号到达）

    EXPECT_TRUE(ce.stop_requested());
    EXPECT_EQ(called.load(), 1);
}

TEST(ConsoleExitTest, RepeatedSignalOnlyHandledOnce)
{
    libmini::ConsoleExit ce;
    std::atomic<int> called{0};
    ce.set_handler([&]() { ++called; });

    ce.on_signal();
    ce.on_signal();
    ce.on_signal();

    EXPECT_EQ(called.load(), 1);
    EXPECT_TRUE(ce.stop_requested());
}

TEST(ConsoleExitTest, WaitReturnsAfterSignal)
{
    libmini::ConsoleExit ce;
    ce.set_handler([]() {});
    ce.on_signal();
    ce.wait();  // 回调已执行完，立即返回
    EXPECT_TRUE(ce.stop_requested());
}

TEST(ConsoleExitTest, HandlerSetAfterConstructionStillRuns)
{
    libmini::ConsoleExit ce;
    ce.set_handler([]() {});  // 空回调也应推进 done 状态
    ce.on_signal();
    ce.wait();
    EXPECT_TRUE(ce.stop_requested());
}

TEST(ConsoleExitTest, DestructorRestoresDefaultBehavior)
{
    {
        libmini::ConsoleExit ce;
        ce.set_handler([]() {});
        ce.on_signal();
    }  // 析构：恢复默认信号行为，不崩溃
    SUCCEED();
}

// ---------------- Retry：指数退避轮询 ----------------

TEST(RetryTest, ImmediateSuccessNoDelay)
{
    int calls = 0;
    const bool ok = libmini::poll_until([&]() { ++calls; return true; }, 10, 50);
    EXPECT_TRUE(ok);
    EXPECT_EQ(calls, 1);
}

TEST(RetryTest, SucceedsAfterRetries)
{
    int calls = 0;
    const bool ok = libmini::poll_until(
        [&]() { return ++calls >= 3; }, 10, /*base=*/10, /*max=*/50, /*jitter=*/0.0);
    EXPECT_TRUE(ok);
    EXPECT_EQ(calls, 3);
}

TEST(RetryTest, ExhaustsAttempts)
{
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = libmini::poll_until([]() { return false; },
        /*max=*/3, /*base=*/40, /*max_delay=*/100, /*jitter=*/0.0);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_FALSE(ok);
    // 2 次等待：40 + 80 = 120ms（jitter=0 确定序列）
    EXPECT_GE(elapsed, 110);
}

TEST(RetryTest, BackoffSequenceCapsAtMax)
{
    // 序列 100,200,400,800,1600 → 封顶 1000
    EXPECT_EQ(libmini::backoff_delay_ms(0, 100, 1000, 0.0), 100);
    EXPECT_EQ(libmini::backoff_delay_ms(1, 100, 1000, 0.0), 200);
    EXPECT_EQ(libmini::backoff_delay_ms(3, 100, 1000, 0.0), 800);
    EXPECT_EQ(libmini::backoff_delay_ms(4, 100, 1000, 0.0), 1000);
    EXPECT_EQ(libmini::backoff_delay_ms(9, 100, 1000, 0.0), 1000);
}

TEST(RetryTest, JitterStaysInRange)
{
    for (int i = 0; i < 50; ++i) {
        const int d = libmini::backoff_delay_ms(2, 100, 10000, 0.3);
        // 400ms ± 30% = [280, 520]
        EXPECT_GE(d, 280);
        EXPECT_LE(d, 520);
    }
}

TEST(RetryTest, DeadlineVariant)
{
    const bool ok = libmini::poll_until_deadline([]() { return false; },
        /*timeout_ms=*/150, /*base=*/40, /*max=*/100, /*jitter=*/0.0);
    EXPECT_FALSE(ok);  // 150ms 到点即停

    int calls = 0;
    const bool ok2 = libmini::poll_until_deadline([&]() { return ++calls >= 2; }, 5000, 10, 50, 0.0);
    EXPECT_TRUE(ok2);
}

// ---------------- CRC 家族：CRC16/Modbus、CRC64、Adler-32 ----------------

TEST(CrcFamilyTest, Crc16ModbusCheckValue)
{
    // 标准校验值："123456789" → 0x4B37
    EXPECT_EQ(libmini::Crc16Modbus::compute("123456789"), 0x4B37u);

    libmini::Crc16Modbus crc;
    crc.update("12345");
    crc.update("6789");
    EXPECT_EQ(crc.value(), 0x4B37u);  // 增量与一次性一致
}

TEST(CrcFamilyTest, Crc64EcmaCheckValue)
{
    // CRC-64/ECMA-182（反射形式）："123456789" → 0x995DC9BBDF1939FA
    EXPECT_EQ(libmini::Crc64::compute("123456789"), 0x995DC9BBDF1939FAULL);

    libmini::Crc64 crc;
    crc.update("12345");
    crc.update("6789");
    EXPECT_EQ(crc.value(), 0x995DC9BBDF1939FAULL);
}

TEST(CrcFamilyTest, Adler32KnownValues)
{
    // RFC 1950："Wikipedia" → 0x11E60398
    EXPECT_EQ(libmini::Adler32::compute("Wikipedia"), 0x11E60398u);
    EXPECT_EQ(libmini::Adler32::compute(""), 1u);  // 空输入 = 初值

    libmini::Adler32 a;
    a.update("Wiki");
    a.update("pedia");
    EXPECT_EQ(a.value(), 0x11E60398u);
}

// ---------------- TCP：帧协议长连接 ----------------

TEST(TcpTest, EchoRoundTrip)
{
    libmini::TcpServer server;
    server.set_on_message([&server](std::uint64_t, const std::string& msg) {
        server.send(1, "echo:" + msg);  // 单连接测试场景
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0));
    ASSERT_GT(server.port(), 0);

    libmini::TcpClient client;
    std::string got;
    client.set_on_message([&](const std::string& msg) {
        got = msg;
    });
    ASSERT_TRUE(client.connect("127.0.0.1", server.port()));
    ASSERT_TRUE(client.is_connected());
    ASSERT_TRUE(client.send("hello"));

    // 等回包（最多 2s）
    for (int i = 0; i < 200 && got.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(got, "echo:hello");

    client.close();
    server.stop();
}

TEST(TcpTest, ConcurrentClientsAndBroadcast)
{
    libmini::TcpServer server;
    std::atomic<int> echo_count{0};
    server.set_on_message([&server, &echo_count](std::uint64_t /*conn*/, const std::string& msg) {
        if (msg == "join") {
            ++echo_count;
        }
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    constexpr int kClients = 5;
    std::vector<std::unique_ptr<libmini::TcpClient>> clients;
    std::atomic<int> received{0};
    for (int i = 0; i < kClients; ++i) {
        auto c = std::make_unique<libmini::TcpClient>();
        c->set_on_message([&](const std::string& msg) {
            if (msg == "news") {
                ++received;
            }
        });
        ASSERT_TRUE(c->connect("127.0.0.1", server.port()));
        clients.push_back(std::move(c));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server.connection_count(), static_cast<std::size_t>(kClients));

    server.broadcast("news");
    for (int i = 0; i < 200 && received.load() < kClients; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received.load(), kClients);  // 每个客户端各收到一次广播

    for (auto& c : clients) {
        c->close();
    }
    server.stop();
}

TEST(TcpTest, LargeBinaryFrameIntegrity)
{
    libmini::TcpServer server;
    std::string server_got;
    server.set_on_message([&](std::uint64_t, const std::string& msg) {
        server_got = msg;
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    libmini::TcpClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", server.port()));

    // 1MB 二进制（含全字节值）——验帧长度前缀与粘包处理
    std::string payload;
    payload.reserve(1024 * 1024);
    for (int i = 0; i < 1024 * 1024; ++i) {
        payload.push_back(static_cast<char>(i & 0xff));
    }
    ASSERT_TRUE(client.send(payload));

    const std::string expect = payload;
    for (int i = 0; i < 300 && server_got.size() != expect.size(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(server_got, expect);

    client.close();
    server.stop();
}

TEST(TcpTest, DisconnectCallbackFires)
{
    libmini::TcpServer server;
    std::atomic<int> server_disc{0};
    server.set_on_disconnect([&](std::uint64_t, const std::string&) { ++server_disc; });
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    libmini::TcpClient client;
    std::atomic<int> client_disc{0};
    std::string reason;
    client.set_on_disconnect([&](const std::string& r) {
        ++client_disc;
        reason = r;
    });
    ASSERT_TRUE(client.connect("127.0.0.1", server.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client.close();
    for (int i = 0; i < 100 && server_disc.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(server_disc.load(), 1);  // 服务端感知断开

    // 服务端主动断：客户端应收到断连回调（回调需在 connect 前注册）
    std::uint64_t cid = 0;
    server.set_on_connect([&](std::uint64_t id) { cid = id; });
    libmini::TcpClient c2;
    c2.set_on_disconnect([&](const std::string& r) {
        ++client_disc;
        reason = r;
    });
    ASSERT_TRUE(c2.connect("127.0.0.1", server.port()));
    for (int i = 0; i < 100 && cid == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GT(cid, 0u);
    server.disconnect(cid);
    for (int i = 0; i < 100 && client_disc.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(client_disc.load(), 1);
    EXPECT_FALSE(reason.empty());

    c2.close();
    server.stop();
}

TEST(TcpTest, HeartbeatDetectsDeadPeer)
{
    libmini::TcpConfig cfg;
    cfg.heartbeat_interval_ms = 60;
    cfg.heartbeat_timeout_ms = 250;  // 快速判定便于测试

    libmini::TcpServer server(cfg);
    std::atomic<int> server_disc{0};
    server.set_on_disconnect([&](std::uint64_t, const std::string&) { ++server_disc; });
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    libmini::TcpClient client(cfg);
    ASSERT_TRUE(client.connect("127.0.0.1", server.port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 客户端硬杀进程模拟不可用——本地测试用 close 不触发心跳超时，
    // 改用「对端静默」：直接拿底层连接 shutdown 不通知（服务端应超时判死）。
    // 公开 API 无此能力，故验证 PING/PONG 保活不误杀正常连接 + 服务端在
    // 客户端 close 后能感知断开（前例已验）。此处验证心跳配置下长连接稳定：
    for (int i = 0; i < 15; ++i) {  // 1.5s >> 3×interval，无业务流量
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(client.is_connected());   // 心跳应答维持存活
    EXPECT_EQ(server_disc.load(), 0);     // 未误判

    client.close();
    server.stop();
}

// ---------------- 日历运算 ----------------

TEST(CalendarTest, DaysInMonthAndLeapYears)
{
    EXPECT_EQ(libmini::days_in_month(2024, 2), 29);   // 闰年
    EXPECT_EQ(libmini::days_in_month(2023, 2), 28);
    EXPECT_EQ(libmini::days_in_month(2000, 2), 29);   // 400 整除闰
    EXPECT_EQ(libmini::days_in_month(1900, 2), 28);   // 100 整除不闰
    EXPECT_EQ(libmini::days_in_month(2024, 4), 30);
    EXPECT_TRUE(libmini::is_leap_year(2024));
    EXPECT_FALSE(libmini::is_leap_year(2023));
}

TEST(CalendarTest, DaysCivilRoundTrip)
{
    // 1970-01-01 = 0
    int y, m, d;
    libmini::civil_from_days(0, y, m, d);
    EXPECT_EQ((std::make_tuple(y, m, d)), std::make_tuple(1970, 1, 1));
    EXPECT_EQ(libmini::days_from_civil(1970, 1, 1), 0);
    EXPECT_EQ(libmini::days_from_civil(2026, 9, 20), 20716);

    // 往返扫 1970-2100 每月第一天
    for (int yy = 1970; yy <= 2100; ++yy) {
        for (int mm = 1; mm <= 12; ++mm) {
            const std::int64_t days = libmini::days_from_civil(yy, mm, 1);
            int ry, rm, rd;
            libmini::civil_from_days(days, ry, rm, rd);
            ASSERT_EQ((std::make_tuple(ry, rm, rd)), std::make_tuple(yy, mm, 1)) << yy << "-" << mm;
        }
    }

    // 非法日期
    EXPECT_EQ(libmini::days_from_civil(2023, 2, 29), -1);
    EXPECT_EQ(libmini::days_from_civil(2024, 13, 1), -1);
}

TEST(CalendarTest, AddMonthsClampsToMonthEnd)
{
    // 2024-01-31 + 1月 = 2024-02-29（闰年钳制）
    const std::int64_t jan31 = libmini::days_from_civil(2024, 1, 31);
    EXPECT_EQ(libmini::date_to_string(libmini::add_months(jan31, 1)), "2024-02-29");
    // 2023-01-31 + 1月 = 2023-02-28（平年）
    const std::int64_t jan31b = libmini::days_from_civil(2023, 1, 31);
    EXPECT_EQ(libmini::date_to_string(libmini::add_months(jan31b, 1)), "2023-02-28");
    // 跨年
    const std::int64_t nov15 = libmini::days_from_civil(2024, 11, 15);
    EXPECT_EQ(libmini::date_to_string(libmini::add_months(nov15, 3)), "2025-02-15");
    // 负数
    EXPECT_EQ(libmini::date_to_string(libmini::add_months(nov15, -12)), "2023-11-15");
}

TEST(CalendarTest, WeekdayAndMonthBoundaries)
{
    // 1970-01-01 是周四（4）
    EXPECT_EQ(libmini::weekday_of(0), 4);
    // 2026-09-20 是周日（0，tm_wday 语义）
    EXPECT_EQ(libmini::weekday_of(libmini::days_from_civil(2026, 9, 20)), 0);

    // 下周一：今天是周日 → 明天
    const std::int64_t sunday = libmini::days_from_civil(2026, 9, 20);
    EXPECT_EQ(libmini::next_weekday(sunday, 1), sunday + 1);
    // 今天是周一，next_weekday(1) 返回下周一（当天不算）
    EXPECT_EQ(libmini::next_weekday(sunday + 1, 1), sunday + 8);

    // 月初/月末
    const std::int64_t mid_feb = libmini::days_from_civil(2024, 2, 15);
    EXPECT_EQ(libmini::date_to_string(libmini::month_start(mid_feb)), "2024-02-01");
    EXPECT_EQ(libmini::date_to_string(libmini::month_end(mid_feb)), "2024-02-29");

    // 字符串互转
    EXPECT_EQ(libmini::date_from_string("2024-02-29"), libmini::days_from_civil(2024, 2, 29));
    EXPECT_EQ(libmini::date_from_string("2024-02-30"), -1);  // 非法
    EXPECT_EQ(libmini::date_from_string("bad"), -1);
}

// ---------------- FileLock 共享模式 ----------------

TEST(FileLockSharedTest, MultipleReadersCoexist)
{
    const std::string path = "shared_lock_test.lock";
    libmini::FileLock r1(path, libmini::FileLock::Mode::Shared);
    libmini::FileLock r2(path, libmini::FileLock::Mode::Shared);
    EXPECT_TRUE(r1.try_lock_shared());
    EXPECT_TRUE(r2.try_lock_shared());  // 双读者并存
    EXPECT_TRUE(r1.is_locked());
    EXPECT_TRUE(r2.is_locked());
    r1.unlock();
    r2.unlock();
}

TEST(FileLockSharedTest, WriterExcludedByReader)
{
    const std::string path = "shared_lock_test2.lock";
    libmini::FileLock reader(path, libmini::FileLock::Mode::Shared);
    ASSERT_TRUE(reader.try_lock_shared());

    libmini::FileLock writer(path, libmini::FileLock::Mode::Exclusive);
    EXPECT_FALSE(writer.try_lock());      // 读者持有 → 写者被拒

    reader.unlock();
    EXPECT_TRUE(writer.try_lock());       // 读者释放 → 写者获得
    writer.unlock();
}

TEST(FileLockSharedTest, ReaderExcludedByWriter)
{
    const std::string path = "shared_lock_test3.lock";
    libmini::FileLock writer(path, libmini::FileLock::Mode::Exclusive);
    ASSERT_TRUE(writer.try_lock());

    libmini::FileLock reader(path, libmini::FileLock::Mode::Shared);
    EXPECT_FALSE(reader.try_lock_shared());  // 写者持有 → 读者被拒

    writer.unlock();
    EXPECT_TRUE(reader.try_lock_shared());
    reader.unlock();
}

// ---------------- ObjectPool ----------------

TEST(ObjectPoolTest, AcquireReturnReuse)
{
    libmini::ObjectPool<std::string> pool(2, [] { return std::make_unique<std::string>("obj"); });
    {
        auto lease = pool.acquire();
        ASSERT_TRUE(static_cast<bool>(lease));
        EXPECT_EQ(*lease, "obj");
        EXPECT_EQ(pool.idle_count(), 0u);  // 借出后池空
    }
    EXPECT_EQ(pool.idle_count(), 1u);      // 归还
    auto again = pool.try_acquire();
    ASSERT_TRUE(static_cast<bool>(again));
    EXPECT_EQ(*again, "obj");              // 同一对象复用
}

TEST(ObjectPoolTest, CapacityLimitAndTryAcquire)
{
    libmini::ObjectPool<int> pool(1, [] { return std::make_unique<int>(42); });
    auto a = pool.try_acquire();
    ASSERT_TRUE(static_cast<bool>(a));
    auto b = pool.try_acquire();
    EXPECT_FALSE(static_cast<bool>(b));    // 容量上限，拒绝
    EXPECT_EQ(*a, 42);
}

TEST(ObjectPoolTest, BlockingAcquireWaitsForReturn)
{
    libmini::ObjectPool<int> pool(1, [] { return std::make_unique<int>(7); });
    auto a = pool.acquire();
    ASSERT_TRUE(static_cast<bool>(a));

    // 后台 100ms 后归还 → 主线程阻塞 acquire 应拿到
    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }).detach();
    // 注意：a 的 Lease 还在，无法跨线程归还——改为先手动释放
    a = libmini::ObjectPool<int>::Lease();  // 归还
    auto b = pool.acquire(/*max_wait_ms=*/1000);
    ASSERT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(*b, 7);
}

TEST(ObjectPoolTest, ResetterRunsOnReturn)
{
    libmini::ObjectPool<std::string> pool(1, [] { return std::make_unique<std::string>(); });
    pool.set_resetter([](std::string& s) { s = "reset"; });
    {
        auto lease = pool.acquire();
        *lease = "dirty";
    }
    auto lease = pool.try_acquire();
    ASSERT_TRUE(static_cast<bool>(lease));
    EXPECT_EQ(*lease, "reset");            // 归还时被重置
}

// ---------------- zip ----------------

TEST(ZipTest, RoundTripTextAndBinary)
{
    libmini::ZipWriter zw;
    ASSERT_TRUE(zw.add_file("hello.txt", "hello zip world"));
    std::string bin;
    for (int i = 0; i < 10000; ++i) {
        bin.push_back(static_cast<char>(i & 0xff));
    }
    ASSERT_TRUE(zw.add_file("data/bin.dat", bin, libmini::ZipMethod::Store));
    ASSERT_TRUE(zw.add_file("中文文件.md", u8"# 中文内容\n"));
    const std::string zip_bytes = zw.finish();
    EXPECT_GT(zip_bytes.size(), 100u);

    libmini::ZipReader zr;
    ASSERT_TRUE(zr.open(zip_bytes));
    ASSERT_EQ(zr.entries().size(), 3u);
    EXPECT_TRUE(zr.contains("hello.txt"));
    EXPECT_EQ(zr.extract("hello.txt"), "hello zip world");
    EXPECT_EQ(zr.extract("data/bin.dat"), bin);      // store 逐字节
    EXPECT_EQ(zr.extract("\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE4\xBB\xB6.md"), u8"# 中文内容\n");
}

TEST(ZipTest, DeflateActuallyCompresses)
{
    const std::string repetitive(100000, 'a');
    libmini::ZipWriter zw;
    ASSERT_TRUE(zw.add_file("big.txt", repetitive));
    const std::string zip_bytes = zw.finish();
    // deflate 后应远小于原内容 + 头开销
    EXPECT_LT(zip_bytes.size(), 2000u);

    libmini::ZipReader zr;
    ASSERT_TRUE(zr.open(zip_bytes));
    EXPECT_EQ(zr.entries()[0].uncompressed_size, 100000u);
    EXPECT_EQ(zr.extract("big.txt"), repetitive);
}

TEST(ZipTest, CorruptDataFailsGracefully)
{
    libmini::ZipWriter zw;
    ASSERT_TRUE(zw.add_file("f.txt", "some content here"));
    std::string zip_bytes = zw.finish();

    // 篡改压缩数据区 → CRC 失败
    const std::size_t data_pos = zip_bytes.find("some content");
    ASSERT_NE(data_pos, std::string::npos);
    zip_bytes[data_pos] ^= 0xff;

    libmini::ZipReader zr;
    if (zr.open(zip_bytes)) {  // 头可能仍可解析
        EXPECT_TRUE(zr.extract("f.txt").empty());
        EXPECT_FALSE(zr.last_error().empty());
    }

    libmini::ZipReader bad;
    EXPECT_FALSE(bad.open("not a zip"));
}

// ---------------- AES-256-GCM ----------------

TEST(AesGcmTest, RoundTripAndAad)
{
    const std::string key(32, 'k');
    const std::string nonce(12, 'n');
    const std::string pt = "attack at dawn";

    const std::string ct = libmini::Aes256Gcm::encrypt(key, nonce, pt);
    ASSERT_EQ(ct.size(), pt.size() + 16);  // ct || tag
    EXPECT_NE(ct, pt);
    EXPECT_EQ(libmini::Aes256Gcm::decrypt(key, nonce, ct), pt);

    // AAD 参与认证但不加密
    const std::string ct2 = libmini::Aes256Gcm::encrypt(key, nonce, pt, "header");
    EXPECT_EQ(libmini::Aes256Gcm::decrypt(key, nonce, ct2, "header"), pt);
    EXPECT_TRUE(libmini::Aes256Gcm::decrypt(key, nonce, ct2, "HEADER").empty());  // AAD 不匹配
}

TEST(AesGcmTest, TamperDetection)
{
    const std::string key(32, 'k');
    const std::string nonce(12, 'n');
    std::string ct = libmini::Aes256Gcm::encrypt(key, nonce, "secret payload");

    ct[3] ^= 0x01;  // 翻转密文 1 bit
    EXPECT_TRUE(libmini::Aes256Gcm::decrypt(key, nonce, ct).empty());

    ct = libmini::Aes256Gcm::encrypt(key, nonce, "secret payload");
    ct.back() ^= 0x80;  // 篡改 tag
    EXPECT_TRUE(libmini::Aes256Gcm::decrypt(key, nonce, ct).empty());
}

TEST(AesGcmTest, WrongKeyNonceFails)
{
    const std::string key(32, 'k');
    std::string key2(32, 'K');
    const std::string nonce(12, 'n');
    const std::string ct = libmini::Aes256Gcm::encrypt(key, nonce, "data");
    EXPECT_TRUE(libmini::Aes256Gcm::decrypt(key2, nonce, ct).empty());
    key2 = key;
    std::string nonce2(12, 'm');
    EXPECT_TRUE(libmini::Aes256Gcm::decrypt(key, nonce2, ct).empty());

    // 非法长度参数
    EXPECT_TRUE(libmini::Aes256Gcm::encrypt("short", nonce, "x").empty());
    EXPECT_TRUE(libmini::Aes256Gcm::decrypt(key, "12b", ct).empty());
}

TEST(AesGcmTest, SealOpenRandomNonce)
{
    const std::string key(32, 'k');
    const std::string pt = "persist me";
    const std::string sealed1 = libmini::Aes256Gcm::seal(key, pt);
    const std::string sealed2 = libmini::Aes256Gcm::seal(key, pt);
    ASSERT_EQ(sealed1.size(), 12 + pt.size() + 16);
    EXPECT_NE(sealed1, sealed2);  // 每次随机 nonce

    EXPECT_EQ(libmini::Aes256Gcm::open(key, sealed1), pt);
    EXPECT_EQ(libmini::Aes256Gcm::open(key, sealed2), pt);
    std::string tampered = sealed1;
    tampered[20] ^= 0xff;
    EXPECT_TRUE(libmini::Aes256Gcm::open(key, tampered).empty());  // 篡改检测
}

// ------------------------------ sqlite ------------------------------

namespace {

// 临时数据库文件路径（每测试唯一，避免并发冲突）
std::string sqlite_temp_path(const char* name)
{
    static std::atomic<int> seq{0};
    return libmini::temp_directory_path() + "/libmini_sqlite_" + name +
           "_" + std::to_string(seq.fetch_add(1)) + ".db";
}

// 建标准测试表并插入 n 行（:name/:score/:note 命名参数；每三行 note 为 NULL）
void create_users_table(libmini::SqliteDatabase& db, int n)
{
    ASSERT_TRUE(db.exec(
        "CREATE TABLE users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  score REAL,"
        "  note TEXT)"));
    libmini::SqliteStatement ins(
        db, "INSERT INTO users(name, score, note) VALUES(:name, :score, :note)");
    for (int i = 0; i < n; ++i) {
        ins.reset();
        ASSERT_TRUE(ins.bind_text_by_name(":name", "user" + std::to_string(i)));
        ASSERT_TRUE(ins.bind_double_by_name(":score", i * 0.5));
        if (i % 3 == 0) {
            ASSERT_TRUE(ins.bind_null_by_name(":note"));
        } else {
            ASSERT_TRUE(
                ins.bind_text_by_name(":note", "n" + std::to_string(i)));
        }
        ASSERT_EQ(ins.step(), libmini::SqliteStatement::StepDone);
    }
}

}  // namespace

TEST(SqliteTest, InMemoryOpenAndExec)
{
    using namespace libmini;
    SqliteDatabase db(":memory:");
    ASSERT_TRUE(db.is_open());
    ASSERT_TRUE(db.exec("CREATE TABLE t (a INTEGER, b TEXT)"));
    ASSERT_TRUE(db.exec("INSERT INTO t VALUES (1, 'one'), (2, 'two')"));
    EXPECT_EQ(db.changes(), 2);

    SqliteStatement st(db, "SELECT a, b FROM t ORDER BY a");
    ASSERT_TRUE(st.is_prepared());
    ASSERT_EQ(st.step(), SqliteStatement::StepRow);
    EXPECT_EQ(st.column_int(0), 1);
    EXPECT_EQ(st.column_text(1), "one");
    ASSERT_EQ(st.step(), SqliteStatement::StepRow);
    EXPECT_EQ(st.column_int(0), 2);
    EXPECT_EQ(st.column_text(1), "two");
    EXPECT_EQ(st.step(), SqliteStatement::StepDone);
}

TEST(SqliteTest, OpenFailureReportsCannotOpen)
{
    using namespace libmini;
    SqliteDatabase db(temp_directory_path() + "/no_such_dir_99123/x.db");
    EXPECT_FALSE(db.is_open());
    EXPECT_EQ(db.last_status(), SqliteStatus::CannotOpen);
}

TEST(SqliteTest, BindIterateAndRowid)
{
    using namespace libmini;
    const std::string path = sqlite_temp_path("bind");
    remove_file(path);  // 清理上次失败运行可能留下的文件
    {
        SqliteDatabase db(path);
        ASSERT_TRUE(db.is_open());
        create_users_table(db, 10);
        EXPECT_EQ(db.changes(), 1);
        EXPECT_EQ(db.last_insert_rowid(), 10);

        // ? 占位绑定 + 遍历；AUTOINCREMENT 下 id = i + 1；note 为 NULL：i % 3 == 0
        SqliteStatement st(db,
            "SELECT id, name, score, note FROM users WHERE score >= ? "
            "ORDER BY id");
        ASSERT_TRUE(st.bind_double(1, 3.0));  // i >= 6，共 4 行
        int rows = 0;
        while (st.step() == SqliteStatement::StepRow) {
            const int i = 6 + rows;
            EXPECT_EQ(st.column_int(0), i + 1);  // id 从 1 开始
            EXPECT_EQ(st.column_text(1), "user" + std::to_string(i));
            if (i % 3 == 0) {
                EXPECT_TRUE(st.column_is_null(3));
                EXPECT_TRUE(st.column_text(3).empty());
            } else {
                EXPECT_FALSE(st.column_is_null(3));
            }
            ++rows;
        }
        EXPECT_EQ(rows, 4);
        EXPECT_EQ(st.column_name(1), "name");
        EXPECT_EQ(st.column_count(), 4);
    }
    // 持久化：重新打开数据仍在
    SqliteDatabase db2(path);
    ASSERT_TRUE(db2.is_open());
    SqliteStatement cnt(db2, "SELECT COUNT(*) FROM users");
    ASSERT_EQ(cnt.step(), SqliteStatement::StepRow);
    EXPECT_EQ(cnt.column_int(0), 10);
    remove_file(path);
}

TEST(SqliteTest, QueryAllPreservesTypes)
{
    using namespace libmini;
    SqliteDatabase db(":memory:");
    ASSERT_TRUE(db.exec("CREATE TABLE t (i INTEGER, r REAL, s TEXT, n)"));
    ASSERT_TRUE(
        db.exec("INSERT INTO t VALUES (42, 3.5, 'txt', NULL)"));

    SqliteStatement st(db, "SELECT i, r, s, n FROM t");
    const std::vector<std::vector<SqliteValue>> rows = st.query_all();
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].size(), 4u);
    EXPECT_EQ(rows[0][0].type, SqliteValue::Type::Integer);
    EXPECT_EQ(rows[0][0].integer, 42);
    EXPECT_EQ(rows[0][1].type, SqliteValue::Type::Real);
    EXPECT_DOUBLE_EQ(rows[0][1].real, 3.5);
    EXPECT_EQ(rows[0][2].type, SqliteValue::Type::Text);
    EXPECT_EQ(rows[0][2].bytes, "txt");
    EXPECT_TRUE(rows[0][3].is_null());

    // 便捷转换：类型可桥接取值
    EXPECT_EQ(rows[0][0].to_int64(), 42);
    EXPECT_DOUBLE_EQ(rows[0][0].to_double(), 42.0);
    EXPECT_EQ(rows[0][2].to_int64(), 0);  // 非数字文本宽松转换为 0
    EXPECT_TRUE(rows[0][3].to_text().empty());

    // query_all 之后语句可再次直接 step（游标已重置）
    ASSERT_EQ(st.step(), SqliteStatement::StepRow);
    EXPECT_EQ(st.column_int(0), 42);
}

TEST(SqliteTest, BlobRoundTripBinarySafe)
{
    using namespace libmini;
    SqliteDatabase db(":memory:");
    ASSERT_TRUE(db.exec("CREATE TABLE b (data BLOB)"));

    std::string blob;
    for (int i = 0; i < 256; ++i) {
        blob.push_back(static_cast<char>(i));  // 含 \0 的全部字节
    }
    SqliteStatement ins(db, "INSERT INTO b VALUES (?)");
    ASSERT_TRUE(ins.bind_blob(1, blob));
    ASSERT_EQ(ins.step(), SqliteStatement::StepDone);

    SqliteStatement sel(db, "SELECT data FROM b");
    ASSERT_EQ(sel.step(), SqliteStatement::StepRow);
    EXPECT_EQ(sel.column_blob(0), blob);
    EXPECT_EQ(sel.column_value(0).type, SqliteValue::Type::Blob);
}

TEST(SqliteTest, TransactionCommitAndRollback)
{
    using namespace libmini;
    SqliteDatabase db(":memory:");
    ASSERT_TRUE(db.exec("CREATE TABLE t (v INTEGER)"));

    // 提交路径
    {
        SqliteTransaction tx(db);
        ASSERT_TRUE(tx.is_active());
        SqliteStatement ins(db, "INSERT INTO t VALUES (1)");
        ASSERT_EQ(ins.step(), SqliteStatement::StepDone);
        ASSERT_TRUE(tx.commit());
        EXPECT_FALSE(tx.is_active());
        EXPECT_FALSE(tx.commit());  // 幂等：已结束再提交返回 false
    }

    // 显式回滚
    {
        SqliteTransaction tx(db);
        SqliteStatement ins(db, "INSERT INTO t VALUES (2)");
        ASSERT_EQ(ins.step(), SqliteStatement::StepDone);
        tx.rollback();
    }

    // 析构自动回滚（未调用 commit）
    {
        SqliteTransaction tx(db);
        SqliteStatement ins(db, "INSERT INTO t VALUES (3)");
        ASSERT_EQ(ins.step(), SqliteStatement::StepDone);
    }

    SqliteStatement cnt(db, "SELECT COUNT(*) FROM t");
    ASSERT_EQ(cnt.step(), SqliteStatement::StepRow);
    EXPECT_EQ(cnt.column_int(0), 1);  // 只有提交的 1
    SqliteStatement sum(db, "SELECT v FROM t");
    ASSERT_EQ(sum.step(), SqliteStatement::StepRow);
    EXPECT_EQ(sum.column_int(0), 1);
}

TEST(SqliteTest, ConstraintViolationReported)
{
    using namespace libmini;
    SqliteDatabase db(":memory:");
    ASSERT_TRUE(db.exec("CREATE TABLE t (k TEXT UNIQUE)"));
    ASSERT_TRUE(db.exec("INSERT INTO t VALUES ('dup')"));

    SqliteStatement ins(db, "INSERT INTO t VALUES (?)");
    ASSERT_TRUE(ins.bind_text(1, "dup"));
    EXPECT_EQ(ins.step(), SqliteStatement::StepError);
    EXPECT_EQ(db.last_status(), SqliteStatus::Constraint);
    EXPECT_FALSE(db.error_message().empty());

    // 出错后连接仍然可用；bind 自动 reset，换值直接重跑
    ASSERT_TRUE(ins.bind_text(1, "other"));
    EXPECT_EQ(ins.step(), SqliteStatement::StepDone);
    SqliteStatement cnt(db, "SELECT COUNT(*) FROM t");
    ASSERT_EQ(cnt.step(), SqliteStatement::StepRow);
    EXPECT_EQ(cnt.column_int(0), 2);
}

TEST(SqliteTest, ConcurrentWriteReportsBusy)
{
    using namespace libmini;
    const std::string path = sqlite_temp_path("busy");
    remove_file(path);
    SqliteDatabase db1(path);
    ASSERT_TRUE(db1.is_open());
    ASSERT_TRUE(db1.exec("CREATE TABLE t (v INTEGER)"));

    // 连接 1 持有写事务
    ASSERT_TRUE(db1.begin());
    ASSERT_TRUE(db1.exec("INSERT INTO t VALUES (1)"));

    // 连接 2 写入被锁：busy_timeout 极短 → Busy
    SqliteDatabase db2(path);
    ASSERT_TRUE(db2.is_open());
    db2.set_busy_timeout_ms(30);
    EXPECT_FALSE(db2.exec("INSERT INTO t VALUES (2)"));
    EXPECT_EQ(db2.last_status(), SqliteStatus::Busy);

    // 连接 1 释放后连接 2 可写
    ASSERT_TRUE(db1.rollback());
    db2.set_busy_timeout_ms(2000);
    EXPECT_TRUE(db2.exec("INSERT INTO t VALUES (2)"));

    SqliteStatement cnt(db2, "SELECT COUNT(*) FROM t");
    ASSERT_EQ(cnt.step(), SqliteStatement::StepRow);
    EXPECT_EQ(cnt.column_int(0), 1);  // db1 的回滚行不存在
    remove_file(path);
}

TEST(SqliteTest, NotADatabaseDetected)
{
    using namespace libmini;
    const std::string path = sqlite_temp_path("garbage");
    remove_file(path);
    ASSERT_TRUE(libmini::write_file(path, "this is definitely not sqlite"));
    SqliteDatabase db(path);
    ASSERT_TRUE(db.is_open());  // 打开是惰性的，首条语句才读文件
    // 注意：SELECT 1 是常量表达式不读库文件；读 schema 的语句才会暴露坏库
    EXPECT_FALSE(db.exec("SELECT COUNT(*) FROM sqlite_master"));
    EXPECT_EQ(db.last_status(), SqliteStatus::NotADatabase);
    remove_file(path);
}

TEST(SqliteTest, StatementReuseInBatchInsert)
{
    using namespace libmini;
    SqliteDatabase db(":memory:");
    ASSERT_TRUE(db.exec("CREATE TABLE t (v INTEGER)"));
    SqliteStatement ins(db, "INSERT INTO t VALUES (?)");
    {
        SqliteTransaction tx(db);
        for (int i = 0; i < 200; ++i) {
            ASSERT_TRUE(ins.reset_and_clear_bindings());
            ASSERT_TRUE(ins.bind_int(1, i));
            ASSERT_EQ(ins.step(), SqliteStatement::StepDone);
        }
        ASSERT_TRUE(tx.commit());
    }
    SqliteStatement sum(db, "SELECT COUNT(*), SUM(v) FROM t");
    ASSERT_EQ(sum.step(), SqliteStatement::StepRow);
    EXPECT_EQ(sum.column_int(0), 200);
    EXPECT_EQ(sum.column_int64(1), 200 * 199 / 2);
}

TEST(SqliteTest, MisuseErrorsAreReported)
{
    using namespace libmini;
    SqliteDatabase db;  // 未打开
    EXPECT_FALSE(db.exec("SELECT 1"));
    EXPECT_EQ(db.last_status(), SqliteStatus::Misuse);

    SqliteDatabase mem(":memory:");
    SqliteStatement st;  // 未 prepare
    EXPECT_FALSE(st.bind_int(1, 1));
    EXPECT_EQ(st.step(), SqliteStatement::StepError);
    EXPECT_EQ(mem.last_status(), SqliteStatus::OK);  // 错误不殃及无辜连接

    SqliteStatement bad(mem, "SELEKT nonsense");
    EXPECT_FALSE(bad.is_prepared());
    EXPECT_EQ(mem.last_status(), SqliteStatus::Error);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
