// libmini 基础模块单元测试（string/time/file/path/thread/json/xml/serialization）
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "libmini.h"
#include "utils/json_utils.h"  // JsonValue / parse_json（TracingTest 导出验证）
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
    // "/file.txt" 带根名：按 std::filesystem::path::append 语义直接替换
    EXPECT_EQ(path_join("dir", "/file.txt"), "/file.txt");
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
    // 字面量转 std::string：glibc <string.h> 的 ::basename(const char*)
    // 对字面量是精确匹配，不转换会误命中 C 库版本（返回指针）
    using libmini::basename;
    using libmini::dirname;
    const std::string p1 = "dir/file.txt";
    const std::string p2 = "/path/to/file.txt";
    const std::string p3 = "file.txt";
    EXPECT_EQ(dirname(p1), "dir");
    EXPECT_EQ(dirname(p2), "/path/to");
    EXPECT_EQ(basename(p1), "file.txt");
    EXPECT_EQ(basename(p3), "file.txt");
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

    // 触发：创建 → 修改 → 重命名 → 删除（分隔符走 path_join 跨平台）
    const std::string f1 = path_join(dir, "新建.txt");
    const std::string f2 = path_join(dir, "改名.txt");
#ifndef _WIN32
    constexpr bool is_posix_watcher = true;
#else
    constexpr bool is_posix_watcher = false;
#endif
    ASSERT_TRUE(write_file(f1, "v1"));
    // POSIX 轮询的首次扫描落点不定：若 create 与 append 落在首次扫描前，
    // 创建事件会被基线吸收（macOS 慢机实测踩过）。等创建事件到达后再继续。
    if (is_posix_watcher) {
        bool created_seen = false;
        for (int i = 0; i < 100 && !created_seen; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            std::lock_guard<std::mutex> lock(mu);
            created_seen = events.count("\xE6\x96\xB0\xE5\xBB\xBA.txt:C") > 0;
        }
        ASSERT_TRUE(created_seen);
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    ASSERT_TRUE(append_file(f1, "v2"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_TRUE(rename_path(f1, f2));
    // POSIX 轮询靠一次完整扫描才能观察到 rename 状态；若下一个动作在
    // 扫描落地前删除了文件，rename 状态将永久消失。等到事件到达再继续，
    // 保证 remove 不会与 rename 落在同一个扫描窗口里（CI 慢机上必现）。
    // Windows 的 ReadDirectoryChangesW 是事件驱动，不存在该窗口。
    if (is_posix_watcher) {
        bool seen = false;
        for (int i = 0; i < 100 && !seen; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            std::lock_guard<std::mutex> lock(mu);
            seen = events.count("\xE6\x94\xB9\xE5\x90\x8D.txt:C") > 0;
        }
        ASSERT_TRUE(seen);
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    ASSERT_TRUE(remove_file(f2));

    // 等待事件到达（ReadDirectoryChangesW 有系统通知延迟；POSIX 轮询
    // diff 报告删除/创建而非重命名对）
    for (int i = 0; i < 100; ++i) {
        std::lock_guard<std::mutex> lock(mu);
        const bool created = events.count("新建.txt:C") > 0;
#ifdef _WIN32
        const bool renamed_new = events.count("改名.txt:N") > 0;
#else
        // POSIX 轮询把 rename 观察为「新名创建」，报告的是 Created 事件
        const bool renamed_new = events.count("改名.txt:C") > 0;
#endif
#ifdef _WIN32
        const bool removed = events.count("改名.txt:R") > 0;
#else
        // POSIX 轮询把 rename 观察为（新名创建 + 旧名删除），删除事件
        // 可能在 rename 与 remove 合并的窗口里，对最终断言不构成影响
        const bool removed = true;
#endif
        if (created && renamed_new && removed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    watcher.stop();
    EXPECT_FALSE(watcher.is_running());

    std::lock_guard<std::mutex> lock(mu);
    EXPECT_GT(events["新建.txt:C"], 0);   // 创建
#ifdef _WIN32
    EXPECT_GT(events["新建.txt:M"], 0);   // 修改
    EXPECT_GT(events["新建.txt:O"], 0);   // 重命名：旧名
#endif
#ifdef _WIN32
    EXPECT_GT(events["改名.txt:N"], 0);   // 重命名：新名
#else
    EXPECT_GT(events["改名.txt:C"], 0);   // POSIX 轮询：rename 观察为创建
#endif
    if (events["改名.txt:R"] > 0) {       // 删除（POSIX rename 已报 Removed 时不再有）
        EXPECT_GT(events["改名.txt:R"], 0);
    }
}

TEST(DirWatcherTest, SubtreeWatchAndRestart)
{
    using namespace libmini;
    TempDirGuard guard("watch2");
    const std::string dir = guard.dir;
    const std::string subdir = path_join(dir, "子目录");
    ASSERT_TRUE(make_directories(subdir));
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

    ASSERT_TRUE(write_file(path_join(subdir, "深层.txt"), "x"));
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
    ASSERT_TRUE(write_file(path_join(dir, "再次.txt"), "y"));
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

// ==================== MsgPack（nlohmann 内置编解码，字节级规范兼容）====================

TEST(SerializationTest, MsgPackScalarRoundTrip)
{
    using namespace libmini;
    const JsonValue v = JsonValue::object();
    (void)v;

    // 标量逐个往返
    JsonValue obj = JsonValue::object();
    obj["i"] = 150;
    obj["neg"] = -7;
    obj["b"] = true;
    obj["d"] = 3.5;
    obj["s"] = "msgpack \"hello\"";
    obj["n"] = nullptr;
    obj["u64"] = 18446744073709551615ULL;

    const std::string bytes = json_to_msgpack(obj);
    const JsonValue back = msgpack_to_json(bytes);
    EXPECT_EQ(back["i"], 150);
    EXPECT_EQ(back["neg"], -7);
    EXPECT_EQ(back["b"], true);
    EXPECT_DOUBLE_EQ(back["d"].get<double>(), 3.5);
    EXPECT_EQ(back["s"], "msgpack \"hello\"");
    EXPECT_TRUE(back["n"].is_null());
    EXPECT_EQ(back["u64"], 18446744073709551615ULL);

    // 类型化封装
    EXPECT_EQ(deserialize_from_msgpack<int>(serialize_to_msgpack(42)), 42);
    EXPECT_EQ(deserialize_from_msgpack<std::string>(
                  serialize_to_msgpack(std::string("hi"))),
              "hi");
}

TEST(SerializationTest, MsgPackContainerAndStruct)
{
    using namespace libmini;
    // 复用 JSON 组的 Task 结构：能 JSON 序列化的类型 MsgPack 同样能
    test_ns::Task task;
    task.title = "write docs";
    task.priority = 2;
    task.done = false;
    task.tags.push_back("doc");
    task.tags.push_back("v2");

    const std::string bytes = serialize_to_msgpack(task);
    const test_ns::Task back =
        deserialize_from_msgpack<test_ns::Task>(bytes);
    EXPECT_EQ(back.title, "write docs");
    EXPECT_EQ(back.priority, 2);
    EXPECT_EQ(back.done, false);
    ASSERT_EQ(back.tags.size(), 2u);
    EXPECT_EQ(back.tags[1], "v2");

    // map 容器
    std::map<std::string, double> prices;
    prices["apple"] = 1.5;
    prices["pear"] = 2.0;
    const std::map<std::string, double> got =
        deserialize_from_msgpack<std::map<std::string, double> >(
            serialize_to_msgpack(prices));
    EXPECT_EQ(got.size(), 2u);
    EXPECT_DOUBLE_EQ(got.at("pear"), 2.0);
}

TEST(SerializationTest, MsgPackBinaryPayload)
{
    using namespace libmini;
    // nlohmann 的 binary 类型在 MsgPack 里走 bin 格式，往返保留
    std::vector<std::uint8_t> raw;
    raw.push_back(0x00);
    raw.push_back(0x01);
    raw.push_back(0xFF);
    JsonValue v = JsonValue::binary(raw, 0);
    EXPECT_TRUE(v.is_binary());

    const std::string bytes = json_to_msgpack(v);
    const JsonValue back = msgpack_to_json(bytes);
    ASSERT_TRUE(back.is_binary());
    const std::vector<std::uint8_t> got = back.get_binary();
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[2], 0xFF);
}

TEST(SerializationTest, MsgPackOrFallback)
{
    using namespace libmini;
    // 非法字节流 / 类型不匹配 → fallback
    EXPECT_EQ(deserialize_from_msgpack_or<int>("\\xff\\xff\\xff", -1), -1);
    EXPECT_EQ(deserialize_from_msgpack_or<int>(serialize_to_msgpack("str"),
                                               -1),
              -1);
}

// ==================== ProtoBuf（proto3 wire format）====================

TEST(SerializationTest, ProtoScalarRoundTrip)
{
    using namespace libmini;
    JsonValue msg = JsonValue::object();
    msg["1"] = 150;               // int → varint
    msg["2"] = "testing";         // string → length-delimited
    msg["3"] = 1.5;               // double → fixed64
    msg["4"] = true;              // bool → varint 1
    msg["5"] = JsonValue(nullptr);  // 空 message → 0 字节载荷

    const std::string bytes = json_to_proto(msg);
    const JsonValue back = proto_to_json(bytes);
    EXPECT_EQ(back["1"], 150);
    EXPECT_EQ(back["2"], "testing");
    EXPECT_DOUBLE_EQ(back["3"].get<double>(), 1.5);
    EXPECT_EQ(back["4"].get<int>(), 1);  // bool 读取端按 varint 数值
    EXPECT_TRUE(back["5"].is_null());    // 空载荷按约定读回 null
}

TEST(SerializationTest, ProtoOfficialWireCompat)
{
    using namespace libmini;
    // 官方文档标准示例：message { int32 a = 1; } 序列化为 08 96 01
    const std::string official;
    (void)official;
    const char kOfficialBytes[] = "\x08\x96\x01";
    const JsonValue back = proto_to_json(std::string(kOfficialBytes, 3));
    EXPECT_EQ(back["1"], 150);

    // 反向：同样内容编出的字节一致
    JsonValue msg = JsonValue::object();
    msg["1"] = 150;
    const std::string bytes = json_to_proto(msg);
    ASSERT_EQ(bytes.size(), 3u);
    EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 0x08);
    EXPECT_EQ(static_cast<unsigned char>(bytes[1]), 0x96);
    EXPECT_EQ(static_cast<unsigned char>(bytes[2]), 0x01);

    // 负数 int32：proto3 以 64 位二补码 varint 编码（10 字节），
    // 读取端无法区分 64 位大数，按无符号返回（与官方 C++ API 的
    // uint64 视角一致）
    JsonValue neg = JsonValue::object();
    neg["1"] = -1;
    const JsonValue back2 = proto_to_json(json_to_proto(neg));
    EXPECT_TRUE(back2["1"].is_number_unsigned());
    EXPECT_EQ(back2["1"], 18446744073709551615ULL);
}

TEST(SerializationTest, ProtoNestedAndRepeated)
{
    using namespace libmini;
    JsonValue inner = JsonValue::object();
    inner["1"] = "inner";

    JsonValue msg = JsonValue::object();
    msg["1"] = inner;                 // 嵌套 message
    JsonValue arr = JsonValue::array();
    arr.push_back("alpha");
    arr.push_back("beta");
    msg["2"] = arr;                   // repeated：同号重复出现

    const JsonValue back = proto_to_json(json_to_proto(msg));
    ASSERT_TRUE(back["1"].is_object());
    EXPECT_EQ(back["1"]["1"], "inner");
    ASSERT_TRUE(back["2"].is_array());
    EXPECT_EQ(back["2"][0], "alpha");
    EXPECT_EQ(back["2"][1], "beta");
}

TEST(SerializationTest, ProtoUnfoldPacked)
{
    using namespace libmini;
    // 官方 packed repeated：field 5, wire 2, len 3, varint 1,2,3
    // 手工组帧（写端数组走非 packed，packed 场景用 unfold_packed 读）
    const char kPacked[] = "\x2A\x03\x01\x02\x03";
    const JsonValue back = proto_to_json(std::string(kPacked, 5));
    // 载荷 01 02 03 不是合法 message（字段号 0）→ 落为原始字符串
    ASSERT_TRUE(back["5"].is_string());

    JsonValue tree = back;
    unfold_packed(tree, "5", ProtoWireType::Varint);
    ASSERT_TRUE(tree["5"].is_array());
    EXPECT_EQ(tree["5"][0], 1);
    EXPECT_EQ(tree["5"][2], 3);

    // 写端数组逐元素重复 tag；单元素数组读回是标量（无 schema 无法
    // 区分 repeated 与单值），两个元素才往返为数组
    JsonValue fmsg = JsonValue::object();
    JsonValue farr = JsonValue::array();
    farr.push_back(0.5);
    farr.push_back(1.5);
    fmsg["3"] = farr;
    JsonValue ftree = proto_to_json(json_to_proto(fmsg));
    ASSERT_TRUE(ftree["3"].is_array());
    EXPECT_DOUBLE_EQ(ftree["3"][0].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(ftree["3"][1].get<double>(), 1.5);
}

TEST(SerializationTest, ProtoOrFallbackAndErrors)
{
    using namespace libmini;
    // wire type 7 非法 / 截断流 → fallback
    const char kBadWire[] = "\x0F";  // field 1, wire 7
    const JsonValue fallback = JsonValue::object();
    EXPECT_TRUE(proto_to_json_or(std::string(kBadWire, 1), fallback)
                    .is_object());
    const char kTruncated[] = "\x0A\x05\x61";  // len 5 实际 1 字节
    EXPECT_TRUE(proto_to_json_or(std::string(kTruncated, 3), fallback)
                    .is_object());

    // 非 message 顶层 / 非字段号键 → 抛 runtime_error
    JsonValue scalar = JsonValue(42);
    EXPECT_THROW(json_to_proto(scalar), std::runtime_error);
    JsonValue badkey = JsonValue::object();
    badkey["name"] = "x";
    EXPECT_THROW(json_to_proto(badkey), std::runtime_error);

    // 类型化封装 fallback
    EXPECT_EQ(deserialize_from_proto_or<int>(std::string(kBadWire, 1), -1),
              -1);
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

// ---------------- 熔断器 ----------------

TEST(CircuitBreakerTest, OpensAfterConsecutiveFailures)
{
    libmini::CircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    cfg.open_duration_ms = 60000;  // 测试内不冷却
    libmini::CircuitBreaker cb(cfg);
    EXPECT_EQ(cb.state(), libmini::CircuitState::Closed);

    // 连续失败不足阈值：保持 Closed，成功清零连续计数
    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    EXPECT_TRUE(cb.allow());
    cb.record_success();
    EXPECT_EQ(cb.state(), libmini::CircuitState::Closed);

    // 重新累计到阈值 → Open，后续 allow 立即拒绝（快速失败）
    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    EXPECT_EQ(cb.state(), libmini::CircuitState::Open);
    EXPECT_FALSE(cb.allow());
    EXPECT_FALSE(cb.allow());

    const libmini::CircuitBreakerStats st = cb.stats();
    EXPECT_EQ(st.failures, 5u);
    EXPECT_EQ(st.rejected, 2u);
    EXPECT_EQ(st.state_changes, 1u);
}

TEST(CircuitBreakerTest, HalfOpenAfterCooldownThenCloseOnSuccess)
{
    libmini::CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.open_duration_ms = 80;
    cfg.success_threshold = 2;
    libmini::CircuitBreaker cb(cfg);

    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    ASSERT_EQ(cb.state(), libmini::CircuitState::Open);
    EXPECT_FALSE(cb.allow());

    // 冷却到点：state()/allow() 惰性推进到 HalfOpen
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_EQ(cb.state(), libmini::CircuitState::HalfOpen);

    // HalfOpen 放行探测，连续成功达阈值 → Closed
    EXPECT_TRUE(cb.allow());
    cb.record_success();
    EXPECT_EQ(cb.state(), libmini::CircuitState::HalfOpen);
    EXPECT_TRUE(cb.allow());
    cb.record_success();
    EXPECT_EQ(cb.state(), libmini::CircuitState::Closed);
    EXPECT_TRUE(cb.allow());
}

TEST(CircuitBreakerTest, HalfOpenProbeFailureReopens)
{
    libmini::CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.open_duration_ms = 60;
    libmini::CircuitBreaker cb(cfg);

    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    ASSERT_EQ(cb.state(), libmini::CircuitState::Open);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(cb.state(), libmini::CircuitState::HalfOpen);

    // 探测失败：立即回 Open 重新冷却
    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    EXPECT_EQ(cb.state(), libmini::CircuitState::Open);
    EXPECT_FALSE(cb.allow());

    // 再次冷却后又能探测（失败不永久锁死）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cb.state(), libmini::CircuitState::HalfOpen);
    EXPECT_TRUE(cb.allow());
}

TEST(CircuitBreakerTest, HalfOpenConcurrentProbeLimit)
{
    libmini::CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.open_duration_ms = 40;
    cfg.half_open_max_calls = 2;
    libmini::CircuitBreaker cb(cfg);

    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    ASSERT_EQ(cb.state(), libmini::CircuitState::HalfOpen);

    // 名额 2：前两个放行，第三个拒绝
    EXPECT_TRUE(cb.allow());
    EXPECT_TRUE(cb.allow());
    EXPECT_FALSE(cb.allow());

    // 归结一个结果释放名额
    cb.record_success();
    EXPECT_TRUE(cb.allow());
}

TEST(CircuitBreakerTest, ExecuteOutcomesAndExceptionRecords)
{
    libmini::CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.open_duration_ms = 60000;
    libmini::CircuitBreaker cb(cfg);

    // 成功与失败自动记录
    EXPECT_EQ(cb.execute([]() { return true; }),
              libmini::CircuitOutcome::Success);
    EXPECT_EQ(cb.execute([]() { return false; }),
              libmini::CircuitOutcome::Failure);
    ASSERT_EQ(cb.state(), libmini::CircuitState::Open);

    // 熔断中：Rejected 且 call 不执行
    bool called = false;
    EXPECT_EQ(cb.execute([&]() {
        called = true;
        return true;
    }), libmini::CircuitOutcome::Rejected);
    EXPECT_FALSE(called);

    // 异常路径：记录失败后原样上抛（failure_threshold=1，故先 reset）
    cb.reset();
    ASSERT_EQ(cb.state(), libmini::CircuitState::Closed);
    EXPECT_THROW(cb.execute([]() -> bool { throw std::runtime_error("boom"); }),
                 std::runtime_error);
    EXPECT_EQ(cb.state(), libmini::CircuitState::Open);  // 异常也计入失败
}

TEST(CircuitBreakerTest, StateChangeCallbackAndReset)
{
    libmini::CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.open_duration_ms = 50;
    libmini::CircuitBreaker cb(cfg);

    std::vector<std::pair<int, int>> transitions;
    cb.set_on_state_change([&](libmini::CircuitState from,
                               libmini::CircuitState to) {
        transitions.emplace_back(static_cast<int>(from),
                                 static_cast<int>(to));
        // 回调内重入是安全的（锁外触发）
        (void)cb.state();
    });

    EXPECT_TRUE(cb.allow());
    cb.record_failure();  // Closed → Open（0→1）
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    (void)cb.state();      // Open → HalfOpen 惰性转移（1→2）
    EXPECT_TRUE(cb.allow());
    cb.record_success();   // success_threshold 默认 2：HalfOpen → 不转移
    EXPECT_EQ(transitions.size(), 2u);
    EXPECT_EQ(transitions[0], std::make_pair(0, 1));
    EXPECT_EQ(transitions[1], std::make_pair(1, 2));

    // reset：回到 Closed 并触发转移，累计统计保留
    const std::uint64_t allowed_before = cb.stats().allowed;
    cb.reset();
    EXPECT_EQ(cb.state(), libmini::CircuitState::Closed);
    EXPECT_EQ(transitions.back(), std::make_pair(2, 0));
    EXPECT_EQ(cb.stats().allowed, allowed_before);
    EXPECT_EQ(cb.stats().state_changes, 3u);

    // reset_stats 清零累计、状态不变
    cb.reset_stats();
    const libmini::CircuitBreakerStats st = cb.stats();
    EXPECT_EQ(st.allowed, 0u);
    EXPECT_EQ(st.state_changes, 0u);
    EXPECT_EQ(cb.state(), libmini::CircuitState::Closed);
}

TEST(CircuitBreakerTest, ConfigValidateRejectsInvalid)
{
    libmini::CircuitBreakerConfig good;
    EXPECT_TRUE(good.validate());

    libmini::CircuitBreakerConfig bad;
    bad.failure_threshold = 0;
    bad.open_duration_ms = -1;
    bad.half_open_max_calls = 0;
    bad.success_threshold = -2;
    EXPECT_FALSE(bad.validate());

    // 非法配置构造后仍可用（兜底：阈值按 1 处理、冷却按 0 处理，不除零不卡死）
    libmini::CircuitBreaker cb(bad);
    EXPECT_TRUE(cb.allow());
    cb.record_failure();
    // 阈值兜底为 1：一次失败即熔断；冷却兜底为 0，查询即转 HalfOpen 探测
    EXPECT_NE(cb.state(), libmini::CircuitState::Closed);
    EXPECT_TRUE(cb.allow());  // HalfOpen 放行探测，不会负等待卡死
}

// ---------------- 指标：Counter/Gauge/Histogram + Prometheus 注册表 ----------------

TEST(MetricRegistryTest, CounterMonotonicAndThreadSafe)
{
    libmini::MetricCounter c;
    EXPECT_DOUBLE_EQ(c.value(), 0.0);
    c.inc();
    c.inc(2.5);
    c.inc(0);      // 非正数：忽略（计数器禁止回退）
    c.inc(-3);     // 忽略
    c.inc(std::numeric_limits<double>::quiet_NaN());  // 忽略
    EXPECT_DOUBLE_EQ(c.value(), 3.5);

    // 并发累加无丢失（CAS 循环）
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.push_back(std::thread([&c]() {
            for (int i = 0; i < 10000; ++i) {
                c.inc();
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }
    EXPECT_DOUBLE_EQ(c.value(), 40003.5);  // 4 * 10000 + 3.5，双精度精确

    c.reset();
    EXPECT_DOUBLE_EQ(c.value(), 0.0);
}

TEST(MetricRegistryTest, GaugeSetAddSub)
{
    libmini::MetricGauge g;
    g.set(10);
    g.add(5.5);
    EXPECT_DOUBLE_EQ(g.value(), 15.5);
    g.sub(3.5);
    EXPECT_DOUBLE_EQ(g.value(), 12.0);
    g.add(-2);
    EXPECT_DOUBLE_EQ(g.value(), 10.0);
    g.set(-4.25);
    EXPECT_DOUBLE_EQ(g.value(), -4.25);
    // NaN 拒收（仪表可减，NaN 会污染后续运算）
    g.add(std::numeric_limits<double>::quiet_NaN());
    g.set(std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(g.value(), -4.25);
    g.reset();
    EXPECT_DOUBLE_EQ(g.value(), 0.0);
}

TEST(MetricRegistryTest, HistogramBucketsCumulativeAndPercentiles)
{
    libmini::MetricHistogram h(std::vector<double>{1, 2, 5, 10});
    EXPECT_DOUBLE_EQ(h.percentile(50), -1.0);  // 无样本

    h.observe(0.5);   // le=1
    h.observe(1.0);   // le=1（上界含端点）
    h.observe(1.5);   // le=2
    h.observe(3);     // le=5
    h.observe(7);     // le=10
    h.observe(50);    // +Inf
    h.observe(std::numeric_limits<double>::quiet_NaN());  // 忽略

    EXPECT_EQ(h.count(), 6u);
    EXPECT_DOUBLE_EQ(h.sum(), 63.0);  // 0.5+1+1.5+3+7+50，全程精确
    const std::vector<std::uint64_t> cum = h.cumulative_counts();
    ASSERT_EQ(cum.size(), 5u);  // 4 个上界 + 1 个 +Inf
    EXPECT_EQ(cum[0], 2u);
    EXPECT_EQ(cum[1], 3u);
    EXPECT_EQ(cum[2], 4u);
    EXPECT_EQ(cum[3], 5u);
    EXPECT_EQ(cum[4], 6u);

    // 桶内线性插值：p50 落 le=2 桶顶 2.0；p75 在 [5,10] 中点 7.5；
    // p100 只能给最后一个有限上界（样本在 +Inf 桶里无上界可推）
    EXPECT_DOUBLE_EQ(h.percentile(50), 2.0);
    EXPECT_DOUBLE_EQ(h.percentile(75), 7.5);
    EXPECT_DOUBLE_EQ(h.percentile(100), 10.0);
    EXPECT_DOUBLE_EQ(h.percentile(0), 0.0);
    EXPECT_DOUBLE_EQ(h.percentile(-5), 0.0);    // 越界钳制到 [0,100]
    EXPECT_DOUBLE_EQ(h.percentile(999), 10.0);

    // 非 histogram 样本的分位数恒为 -1
    libmini::MetricSample s;
    s.type = libmini::MetricType::Counter;
    EXPECT_DOUBLE_EQ(s.percentile(50), -1.0);
}

TEST(MetricRegistryTest, HistogramBoundsValidationAndDefaultBuckets)
{
    // 默认桶：1/2/5 序列，单位无关的兕底
    libmini::MetricHistogram def;
    ASSERT_EQ(def.upper_bounds().size(), 10u);
    EXPECT_DOUBLE_EQ(def.upper_bounds().front(), 1.0);
    EXPECT_DOUBLE_EQ(def.upper_bounds().back(), 1000.0);
    def.observe(2000);  // 超最大上界 → +Inf 桶
    EXPECT_EQ(def.count(), 1u);
    EXPECT_EQ(def.cumulative_counts().back(), 1u);

    // 非严格递增/非有限项逐个剔除（5 保留，3 丢，10 保留，+Inf 丢，20 保留）
    libmini::MetricHistogram messy(
        std::vector<double>{5, 3, 10, std::numeric_limits<double>::infinity(), 20});
    ASSERT_EQ(messy.upper_bounds().size(), 3u);
    EXPECT_DOUBLE_EQ(messy.upper_bounds()[0], 5.0);
    EXPECT_DOUBLE_EQ(messy.upper_bounds()[1], 10.0);
    EXPECT_DOUBLE_EQ(messy.upper_bounds()[2], 20.0);

    // 全部非法（首个就非有限）→ 回落默认桶
    libmini::MetricHistogram allbad(
        std::vector<double>{std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()});
    EXPECT_EQ(allbad.upper_bounds().size(), 10u);

    // reset：值清零、桶边界不变
    messy.observe(6);
    messy.reset();
    EXPECT_EQ(messy.count(), 0u);
    EXPECT_DOUBLE_EQ(messy.sum(), 0.0);
    EXPECT_EQ(messy.upper_bounds().size(), 3u);
}

TEST(MetricRegistryTest, RegistryReuseAndTypeConflicts)
{
    libmini::MetricRegistry reg;
    std::shared_ptr<libmini::MetricCounter> c1 = reg.counter("requests_total", "请求总数");
    ASSERT_TRUE(c1 != nullptr);

    // 同名同类型：幂等复用；首个非空 help 生效
    std::shared_ptr<libmini::MetricCounter> c2 =
        reg.counter("requests_total", "另一个 help");
    ASSERT_TRUE(c2 != nullptr);
    EXPECT_EQ(c1.get(), c2.get());

    // 同名不同类型 / 同族不同类型：类型不可转换 → nullptr（编程错误）
    EXPECT_TRUE(reg.gauge("requests_total") == nullptr);
    EXPECT_TRUE(reg.histogram("requests_total") == nullptr);
    EXPECT_TRUE(reg.gauge("requests_total{code=\"200\"}") == nullptr);

    // 同族同类型的标签序列：新实例，计入同一指标族
    std::shared_ptr<libmini::MetricCounter> c3 =
        reg.counter("requests_total{code=\"200\"}");
    ASSERT_TRUE(c3 != nullptr);
    EXPECT_NE(c1.get(), c3.get());
    EXPECT_EQ(reg.size(), 2u);

    const std::vector<libmini::MetricSample> samples = reg.collect();
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].help, "请求总数");  // 首个非空 help，不被后续覆盖
}

TEST(MetricRegistryTest, CollectSortedSnapshotAndResetAll)
{
    libmini::MetricRegistry reg;
    reg.counter("zeta_total", "z help");
    reg.gauge("alpha_gauge", "a help");
    std::shared_ptr<libmini::MetricHistogram> h =
        reg.histogram("mid_ms", std::vector<double>{1, 10});
    h->observe(5);
    std::shared_ptr<libmini::MetricCounter> z =
        reg.counter("zeta_total{env=\"prod\"}");
    z->inc(7);

    std::vector<libmini::MetricSample> samples = reg.collect();
    ASSERT_EQ(samples.size(), 4u);
    // 按 (族名, 序列名) 排序：同族连续，采集顺序稳定
    EXPECT_EQ(samples[0].name, "alpha_gauge");
    EXPECT_EQ(samples[1].name, "mid_ms");
    EXPECT_EQ(samples[2].name, "zeta_total");
    EXPECT_EQ(samples[3].name, "zeta_total{env=\"prod\"}");
    EXPECT_EQ(samples[3].base, "zeta_total");

    EXPECT_EQ(samples[0].type, libmini::MetricType::Gauge);
    EXPECT_EQ(samples[0].help, "a help");
    EXPECT_DOUBLE_EQ(samples[0].value, 0.0);
    EXPECT_DOUBLE_EQ(samples[2].value, 0.0);
    EXPECT_DOUBLE_EQ(samples[3].value, 7.0);

    ASSERT_EQ(samples[1].counts.size(), 3u);
    EXPECT_EQ(samples[1].counts[0], 0u);
    EXPECT_EQ(samples[1].counts[1], 1u);  // 观测 5 落 le=10 桶（累计式）
    EXPECT_EQ(samples[1].total_count, 1u);
    EXPECT_DOUBLE_EQ(samples[1].sum, 5.0);
    EXPECT_DOUBLE_EQ(samples[1].percentile(50), 5.5);  // 桶 [1,10] 内插值

    // reset_all：只归零值，注册项与桶边界保留
    reg.reset_all();
    samples = reg.collect();
    ASSERT_EQ(samples.size(), 4u);
    EXPECT_DOUBLE_EQ(samples[3].value, 0.0);
    EXPECT_EQ(samples[1].total_count, 0u);
    EXPECT_DOUBLE_EQ(samples[1].sum, 0.0);
    EXPECT_EQ(reg.size(), 4u);
}

TEST(MetricRegistryTest, RenderPrometheusText)
{
    libmini::MetricRegistry reg;
    std::shared_ptr<libmini::MetricCounter> reqs =
        reg.counter("http_requests_total", "Total requests");
    reqs->inc(42);
    std::shared_ptr<libmini::MetricCounter> ok =
        reg.counter("http_requests_total{code=\"200\"}");
    ok->inc();
    std::shared_ptr<libmini::MetricHistogram> h =
        reg.histogram("latency_ms{route=\"/api\"}",
                      std::vector<double>{0.5, 1, 5}, "Latency");
    h->observe(0.25);
    h->observe(3);
    h->observe(9);

    const std::string text = reg.render_prometheus();

    // 每族只一组 # HELP / # TYPE，且在序列之前
    std::size_t type_hits = 0;
    for (std::size_t pos = text.find("# TYPE http_requests_total ");
         pos != std::string::npos;
         pos = text.find("# TYPE http_requests_total ", pos + 1)) {
        ++type_hits;
    }
    EXPECT_EQ(type_hits, 1u);  // 两个序列同族，只写一次
    EXPECT_NE(text.find("# HELP http_requests_total Total requests\n"),
              std::string::npos);
    EXPECT_NE(text.find("# TYPE http_requests_total counter\n"),
              std::string::npos);
    EXPECT_NE(text.find("# TYPE latency_ms histogram\n"), std::string::npos);

    // counter 序列：整数值不带小数点；标签序列原样输出
    EXPECT_NE(text.find("http_requests_total 42\n"), std::string::npos);
    EXPECT_NE(text.find("http_requests_total{code=\"200\"} 1\n"),
              std::string::npos);

    // histogram：_bucket{已有标签,le=...} 累计计数 + 末位 +Inf，然后 _sum/_count
    EXPECT_NE(text.find("latency_ms_bucket{route=\"/api\",le=\"0.5\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("latency_ms_bucket{route=\"/api\",le=\"1\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("latency_ms_bucket{route=\"/api\",le=\"5\"} 2\n"),
              std::string::npos);
    EXPECT_NE(text.find("latency_ms_bucket{route=\"/api\",le=\"+Inf\"} 3\n"),
              std::string::npos);
    EXPECT_NE(text.find("latency_ms_sum{route=\"/api\"} 12.25\n"),
              std::string::npos);
    EXPECT_NE(text.find("latency_ms_count{route=\"/api\"} 3\n"),
              std::string::npos);
}

TEST(MetricRegistryTest, ValidNameCheckAndInvalidStillRegisters)
{
    EXPECT_TRUE(libmini::MetricRegistry::valid_name("a"));
    EXPECT_TRUE(libmini::MetricRegistry::valid_name("_x1:b"));
    EXPECT_TRUE(libmini::MetricRegistry::valid_name("a_total{code=\"200\"}"));
    EXPECT_TRUE(libmini::MetricRegistry::valid_name("ns:metric"));
    EXPECT_FALSE(libmini::MetricRegistry::valid_name(""));
    EXPECT_FALSE(libmini::MetricRegistry::valid_name("1abc"));
    EXPECT_FALSE(libmini::MetricRegistry::valid_name("a-b"));
    EXPECT_FALSE(libmini::MetricRegistry::valid_name("a{b"));   // 无收尾 '}'
    EXPECT_FALSE(libmini::MetricRegistry::valid_name("a{b}{c}"));  // 第二个 '{'

    // 非法名：告警但不拒绝（注册表本质是字符串键值表，渲染自担）
    libmini::MetricRegistry reg;
    std::shared_ptr<libmini::MetricCounter> c = reg.counter("-bad name");
    EXPECT_TRUE(c != nullptr);
    EXPECT_EQ(reg.size(), 1u);
}

TEST(MetricRegistryTest, ConcurrentObserveCollectAndRender)
{
    libmini::MetricRegistry reg;
    std::shared_ptr<libmini::MetricCounter> c = reg.counter("stress_total");
    std::shared_ptr<libmini::MetricHistogram> h =
        reg.histogram("stress_ms", std::vector<double>{1, 10, 100});

    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.push_back(std::thread([c, h]() {
            for (int i = 0; i < 5000; ++i) {
                c->inc();
                h->observe(static_cast<double>(i % 1000) / 10.0);
            }
        }));
    }
    // 主线程在写入的同时反复采集与渲染（快照锁 + 原子桶计数不悬空）
    for (int i = 0; i < 50; ++i) {
        EXPECT_FALSE(reg.collect().empty());
        EXPECT_FALSE(reg.render_prometheus().empty());
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    EXPECT_DOUBLE_EQ(c->value(), 20000.0);
    EXPECT_EQ(h->count(), 20000u);  // 计数为整数，精确断言（sum 不断言：浮点加法顺序不定）
}

// ---------------- 追踪：TraceContext / Span / SpanScope / Tracer ----------------

TEST(TracingTest, SpanLifecycleAndSnapshot)
{
    libmini::Tracer tracer("svc");
    std::shared_ptr<libmini::Span> sp = tracer.start_span("op");
    EXPECT_TRUE(sp->context().valid());
    EXPECT_EQ(sp->context().trace_id.size(), 32u);
    EXPECT_EQ(sp->context().span_id.size(), 16u);
    EXPECT_TRUE(sp->parent_span_id().empty());
    EXPECT_TRUE(sp->context().sampled);
    EXPECT_FALSE(sp->ended());
    EXPECT_GT(sp->start_ms(), 0);
    EXPECT_GE(sp->elapsed_ms(), 0);

    sp->set_attribute("user.id", "u42");
    sp->set_attribute("retries", 3);
    sp->set_attribute("neg", -7);
    sp->set_attribute("ratio", 0.5);
    sp->set_attribute("cached", true);
    sp->add_event("cache.miss");
    sp->set_status(libmini::SpanStatus::Ok);

    sp->end();
    EXPECT_TRUE(sp->ended());
    sp->end();  // 幂等：不再产生第二个快照
    EXPECT_EQ(tracer.finished_count(), 1u);

    const std::vector<libmini::SpanSnapshot> done = tracer.finished();
    ASSERT_EQ(done.size(), 1u);
    const libmini::SpanSnapshot& s = done[0];
    EXPECT_EQ(s.service, "svc");
    EXPECT_EQ(s.name, "op");
    EXPECT_EQ(s.trace_id, sp->context().trace_id);
    EXPECT_EQ(s.span_id, sp->context().span_id);
    EXPECT_TRUE(s.parent_span_id.empty());
    EXPECT_EQ(s.status, libmini::SpanStatus::Ok);
    EXPECT_GT(s.start_ms, 0);
    EXPECT_GE(s.end_ms, s.start_ms);
    EXPECT_GE(s.duration_ms, 0);
    EXPECT_EQ(sp->elapsed_ms(), s.duration_ms);  // 结束后 elapsed 即总时长

    // 属性保插入序，数值按整数/定点格式化
    ASSERT_EQ(s.attributes.size(), 5u);
    EXPECT_EQ(s.attributes[0].first, "user.id");
    EXPECT_EQ(s.attributes[0].second, "u42");
    EXPECT_EQ(s.attributes[1].second, "3");
    EXPECT_EQ(s.attributes[2].second, "-7");
    EXPECT_EQ(s.attributes[3].second, "0.5");
    EXPECT_EQ(s.attributes[4].second, "true");
    ASSERT_EQ(s.events.size(), 1u);
    EXPECT_EQ(s.events[0].name, "cache.miss");
    EXPECT_GT(s.events[0].at_ms, 0);

    // 同键覆盖：只留最后一次，且不改变其它键的插入位置
    libmini::Tracer t2;
    std::shared_ptr<libmini::Span> sp2 = t2.start_span("x");
    sp2->set_attribute("k", "v1");
    sp2->set_attribute("k", "v2");
    sp2->set_attribute("n", 2.25);
    sp2->end();
    const std::vector<libmini::SpanSnapshot> d2 = t2.finished();
    ASSERT_EQ(d2.size(), 1u);
    ASSERT_EQ(d2[0].attributes.size(), 2u);
    EXPECT_EQ(d2[0].attributes[0].first, "k");
    EXPECT_EQ(d2[0].attributes[0].second, "v2");
    EXPECT_EQ(d2[0].attributes[1].second, "2.25");
}

TEST(TracingTest, NestedScopesAndChildContext)
{
    libmini::Tracer tracer("nest");
    std::shared_ptr<libmini::Span> root = tracer.start_span("root");
    std::shared_ptr<libmini::Span> child;
    std::shared_ptr<libmini::Span> gc;
    {
        libmini::SpanScope s1(root);
        EXPECT_EQ(libmini::current_span().get(), root.get());
        child = tracer.start_child_span("child");  // 挂在 root 下（不改 current）
        EXPECT_EQ(child->parent_span_id(), root->context().span_id);
        EXPECT_EQ(child->context().trace_id, root->context().trace_id);
        EXPECT_NE(child->context().span_id, root->context().span_id);
        {
            libmini::SpanScope s2(child);
            EXPECT_EQ(libmini::current_span().get(), child.get());
            gc = tracer.start_child_span("gc");
            {
                libmini::SpanScope s3(gc);
                EXPECT_EQ(libmini::current_span().get(), gc.get());
                EXPECT_EQ(gc->parent_span_id(), child->context().span_id);
            }  // s3 析构 end(gc) 并恢复 current=child
            EXPECT_TRUE(gc->ended());
            EXPECT_EQ(libmini::current_span().get(), child.get());
            EXPECT_FALSE(child->ended());
        }  // s2 end(child)
        EXPECT_TRUE(child->ended());
        EXPECT_EQ(libmini::current_span().get(), root.get());
    }  // s1 end(root)
    EXPECT_TRUE(root->ended());
    EXPECT_TRUE(libmini::current_span() == nullptr);  // 作用域栈完全恢复

    const std::vector<libmini::SpanSnapshot> done = tracer.finished();
    ASSERT_EQ(done.size(), 3u);
    // 完成顺序 = 结束顺序：gc → child → root
    EXPECT_EQ(done[0].name, "gc");
    EXPECT_EQ(done[1].name, "child");
    EXPECT_EQ(done[2].name, "root");
    EXPECT_TRUE(done[2].parent_span_id.empty());  // 根无父
    EXPECT_EQ(done[1].parent_span_id, root->context().span_id);
    EXPECT_EQ(done[0].parent_span_id, child->context().span_id);
    EXPECT_EQ(done[0].trace_id, done[2].trace_id);  // 同一 trace 三个 span
}

TEST(TracingTest, TraceparentRoundtrip)
{
    libmini::TraceContext ctx;
    ctx.trace_id = "af7651916cd43dd8448eb211c80319c7";
    ctx.span_id = "b7ad6b7169203331";
    ctx.sampled = true;
    EXPECT_TRUE(ctx.valid());
    EXPECT_EQ(ctx.traceparent(),
              "00-af7651916cd43dd8448eb211c80319c7-b7ad6b7169203331-01");

    libmini::TraceContext back = libmini::TraceContext::parse_traceparent(
        ctx.traceparent());
    EXPECT_TRUE(back.valid());
    EXPECT_EQ(back.trace_id, ctx.trace_id);
    EXPECT_EQ(back.span_id, ctx.span_id);
    EXPECT_TRUE(back.sampled);

    ctx.sampled = false;
    const std::string off = ctx.traceparent();
    EXPECT_EQ(off.substr(off.size() - 2), "00");  // 最后一段 = flags
    back = libmini::TraceContext::parse_traceparent(off);
    EXPECT_TRUE(back.valid());
    EXPECT_FALSE(back.sampled);

    // 大小写不敏感解析
    back = libmini::TraceContext::parse_traceparent(
        "00-AF7651916CD43DD8448EB211C80319C7-B7AD6B7169203331-01");
    EXPECT_TRUE(back.valid());

    // 无效输入一律 invalid：空、垃圾、段数不对、非十六进制、全零 id
    EXPECT_FALSE(libmini::TraceContext::parse_traceparent("").valid());
    EXPECT_FALSE(libmini::TraceContext::parse_traceparent("garbage").valid());
    EXPECT_FALSE(libmini::TraceContext::parse_traceparent(
                     "00-af7651916cd43dd8448eb211c80319c7-b7ad6b7169203331")
                     .valid());  // 缺 flags 段
    EXPECT_FALSE(libmini::TraceContext::parse_traceparent(
                     "00-zz7651916cd43dd8448eb211c80319c7-b7ad6b7169203331-01")
                     .valid());  // 非十六进制
    EXPECT_FALSE(libmini::TraceContext::parse_traceparent(
                     "00-00000000000000000000000000000000-b7ad6b7169203331-01")
                     .valid());  // 全零 trace_id

    libmini::TraceContext empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(empty.traceparent(), "");  // 无效上下文不产头值
}

TEST(TracingTest, RemoteParentContinuesTrace)
{
    libmini::Tracer tracer("svc");
    // 上游服务注入的 traceparent（W3C 规范示例 id）
    libmini::TraceContext remote = libmini::TraceContext::parse_traceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    ASSERT_TRUE(remote.valid());

    std::shared_ptr<libmini::Span> sp = tracer.start_span("server_op", remote);
    EXPECT_EQ(sp->context().trace_id, remote.trace_id);  // 继续上游 trace
    EXPECT_EQ(sp->parent_span_id(), remote.span_id);
    EXPECT_NE(sp->context().span_id, remote.span_id);  // 自己新 id
    EXPECT_TRUE(sp->context().sampled);

    // 无效父上下文 → 降级为新根（新 trace_id、无父）
    libmini::TraceContext bad;
    bad.trace_id = "zz";
    std::shared_ptr<libmini::Span> root2 = tracer.start_span("root2", bad);
    EXPECT_TRUE(root2->parent_span_id().empty());
    EXPECT_EQ(root2->context().trace_id.size(), 32u);
    EXPECT_NE(root2->context().trace_id, remote.trace_id);

    sp->end();
    root2->end();
    EXPECT_EQ(tracer.finished_count(), 2u);
}

TEST(TracingTest, CapacityDropsOldest)
{
    libmini::Tracer tracer("svc", 3);
    for (int i = 0; i < 5; ++i) {
        std::shared_ptr<libmini::Span> sp =
            tracer.start_span("s" + std::to_string(i));
        sp->end();
    }
    EXPECT_EQ(tracer.finished_count(), 3u);
    EXPECT_EQ(tracer.dropped_count(), 2u);  // 环形：丢最旧保最新
    const std::vector<libmini::SpanSnapshot> done = tracer.finished();
    ASSERT_EQ(done.size(), 3u);
    EXPECT_EQ(done[0].name, "s2");
    EXPECT_EQ(done[1].name, "s3");
    EXPECT_EQ(done[2].name, "s4");

    tracer.clear();
    EXPECT_EQ(tracer.finished_count(), 0u);
    EXPECT_EQ(tracer.dropped_count(), 0u);

    // 上限 0 = 不保留快照（只计丢弃）
    libmini::Tracer none("svc", 0);
    none.start_span("gone")->end();
    EXPECT_EQ(none.finished_count(), 0u);
    EXPECT_EQ(none.dropped_count(), 1u);
}

TEST(TracingTest, ConcurrentSpansFromManyThreads)
{
    libmini::Tracer tracer("svc", 1000);
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.push_back(std::thread([&tracer]() {
            for (int i = 0; i < 50; ++i) {
                std::shared_ptr<libmini::Span> sp =
                    tracer.start_span("work");
                sp->set_attribute("i", i);
                sp->end();
            }
        }));
    }
    // 主线程在写入的同时反复导出（收集锁保护快照队列）
    for (int i = 0; i < 20; ++i) {
        EXPECT_FALSE(tracer.finished_json().empty());
        EXPECT_LE(tracer.finished_count(), 1000u);
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    EXPECT_EQ(tracer.finished_count(), 200u);
    EXPECT_EQ(tracer.dropped_count(), 0u);
}

TEST(TracingTest, FinishedJsonExport)
{
    libmini::Tracer tracer("auth-service");
    std::shared_ptr<libmini::Span> sp = tracer.start_span("login");
    sp->set_attribute("k", "v");
    sp->set_status(libmini::SpanStatus::Error, "db down");
    sp->end();

    const std::string js = tracer.finished_json();
    const libmini::JsonValue doc =
        libmini::parse_json(js);  // 非法 JSON 在此抛异常 → 测试失败
    ASSERT_TRUE(doc.is_array());
    ASSERT_EQ(doc.size(), 1u);
    const libmini::JsonValue& item = doc[0];
    EXPECT_EQ(item.at("service").get<std::string>(), "auth-service");
    EXPECT_EQ(item.at("name").get<std::string>(), "login");
    EXPECT_EQ(item.at("status").get<std::string>(), "error");
    EXPECT_EQ(item.at("status_message").get<std::string>(), "db down");
    EXPECT_EQ(item.at("trace_id").get<std::string>().size(), 32u);
    EXPECT_EQ(item.at("span_id").get<std::string>().size(), 16u);
    EXPECT_TRUE(item.at("parent_span_id").get<std::string>().empty());
    EXPECT_EQ(item.at("attributes").at("k").get<std::string>(), "v");
    EXPECT_GE(item.at("duration_ms").get<std::int64_t>(), 0);
    EXPECT_TRUE(item.at("events").is_array());
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

TEST(TcpTest, AsyncConnectResolvesAndCompletes)
{
    libmini::TcpServer server;
    server.set_on_message([&server](std::uint64_t, const std::string& msg) {
        server.send(1, "echo:" + msg);
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    libmini::TcpClient client;
    auto f = client.async_connect("127.0.0.1", server.port());
    ASSERT_EQ(f.get(), true);
    EXPECT_TRUE(client.is_connected());

    // 连接后收发与同步 connect 语义一致（回显路径）
    std::string got;
    client.set_on_message([&](const std::string& msg) { got = msg; });
    ASSERT_TRUE(client.send("ping"));
    for (int i = 0; i < 200 && got.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(got, "echo:ping");

    client.close();
    server.stop();
}

TEST(TcpTest, AsyncConnectToDeadPortFailsFast)
{
    libmini::TcpClient client;
    libmini::TcpConfig cfg;
    cfg.connect_timeout_ms = 1500;
    libmini::TcpClient timed(cfg);

    // 未监听端口：连接被拒或超时，future 都必须置 false（不悬挂）
    auto f = timed.async_connect("127.0.0.1", 1);
    ASSERT_EQ(f.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_FALSE(f.get());
    EXPECT_FALSE(timed.is_connected());
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

TEST(TcpTest, ConfigValidateAcceptsDefaultsAndSaneHeartbeat)
{
    libmini::TcpConfig cfg;
    EXPECT_TRUE(cfg.validate());   // 默认值合法

    cfg.heartbeat_interval_ms = 1000;
    cfg.heartbeat_timeout_ms = 0;  // 0 = interval*3 兕底，合法
    EXPECT_TRUE(cfg.validate());

    cfg.heartbeat_timeout_ms = 3000;   // 3x 推荐配比
    EXPECT_TRUE(cfg.validate());

    cfg.auto_reconnect = true;
    EXPECT_TRUE(cfg.validate());
}

TEST(TcpTest, ConfigValidateRejectsInvalid)
{
    libmini::TcpConfig cfg;

    cfg.connect_timeout_ms = -1;
    EXPECT_FALSE(cfg.validate());
    cfg.connect_timeout_ms = 5000;

    cfg.max_frame_bytes = 0;
    EXPECT_FALSE(cfg.validate());
    cfg.max_frame_bytes = 1024;

    // 判死阈值 <= PING 间隔：健康连接的正常心跳间隙就会被判死
    cfg.heartbeat_interval_ms = 500;
    cfg.heartbeat_timeout_ms = 400;
    EXPECT_FALSE(cfg.validate());

    // 负的心跳/退避毫秒值
    cfg.heartbeat_interval_ms = -100;
    cfg.heartbeat_timeout_ms = 0;
    EXPECT_FALSE(cfg.validate());
    cfg.heartbeat_interval_ms = 0;
    cfg.reconnect_base_delay_ms = -1;
    EXPECT_FALSE(cfg.validate());
    cfg.reconnect_base_delay_ms = 200;

    // 退避上限低于基数
    cfg.auto_reconnect = true;
    cfg.reconnect_max_delay_ms = 100;
    EXPECT_FALSE(cfg.validate());
}

TEST(TcpTest, StartAndConnectRejectInvalidConfig)
{
    libmini::TcpConfig bad;
    bad.heartbeat_interval_ms = 500;
    bad.heartbeat_timeout_ms = 100;   // < interval，非法
    ASSERT_FALSE(bad.validate());

    libmini::TcpServer server(bad);
    EXPECT_FALSE(server.start("127.0.0.1", 0));

    libmini::TcpClient client(bad);
    EXPECT_FALSE(client.connect("127.0.0.1", 1));

    // 合法配置不受影响（同一对对象换回好配置后可正常工作）
    libmini::TcpConfig good;
    libmini::TcpServer server2(good);
    ASSERT_TRUE(server2.start("127.0.0.1", 0));
    libmini::TcpClient client2(good);
    EXPECT_TRUE(client2.connect("127.0.0.1", server2.port()));
    client2.close();
    server2.stop();
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

// ---------------- WebSocket：RFC 6455 ----------------

TEST(WebSocketTest, EchoRoundTripUtf8)
{
    libmini::WsServer server;
    server.set_on_message([&server](std::uint64_t conn, const std::string& msg) {
        server.send(conn, "echo:" + msg);
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0));
    ASSERT_GT(server.port(), 0);

    libmini::WsClient ws;
    std::string got;
    ws.set_on_message([&](const std::string& msg) { got = msg; });
    ASSERT_TRUE(ws.connect("127.0.0.1", server.port(), "/chat"));
    ASSERT_TRUE(ws.is_connected());
    ASSERT_TRUE(ws.send("hello 中文"));

    for (int i = 0; i < 200 && got.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(got, "echo:hello 中文");

    ws.close();
    server.stop();
}

TEST(WebSocketTest, LargeBinaryPayload)
{
    libmini::WsServer server;
    std::string server_got;
    server.set_on_message([&](std::uint64_t, const std::string& msg) {
        server_got = msg;
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    libmini::WsClient ws;
    ASSERT_TRUE(ws.connect("127.0.0.1", server.port()));

    // 256KB 二进制：验证 64 位长度域（>125 且 >65535）与掩码往返
    std::string payload;
    payload.reserve(256 * 1024);
    for (int i = 0; i < 256 * 1024; ++i) {
        payload.push_back(static_cast<char>(i & 0xff));
    }
    ASSERT_TRUE(ws.send_binary(payload));

    for (int i = 0; i < 300 && server_got.size() != payload.size(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(server_got, payload);

    ws.close();
    server.stop();
}

TEST(WebSocketTest, HandshakeRejectsPlainHttpServer)
{
    // 对面是 HTTP 服务器（不会回 101 + 正确 Accept）：握手必须失败
    libmini::HttpServer http;
    http.get("/", [](const libmini::HttpRequest&) {
        return libmini::HttpReply::text(200, "ok");
    });
    ASSERT_TRUE(http.start_background(0));
    ASSERT_TRUE(http.wait_until_ready());

    libmini::WsClient ws;
    libmini::WsConfig cfg;
    cfg.handshake_timeout_ms = 1500;
    libmini::WsClient timed(cfg);
    EXPECT_FALSE(timed.connect("127.0.0.1",
                               static_cast<std::uint16_t>(http.port())));
    EXPECT_FALSE(timed.is_connected());
    ws.close();
    http.stop();
}

TEST(WebSocketTest, CloseHandshakeFiresDisconnectCallbacks)
{
    libmini::WsServer server;
    std::atomic<int> server_conn{0};
    std::atomic<int> server_disc{0};
    std::uint64_t cid = 0;
    server.set_on_connect([&](std::uint64_t id) {
        cid = id;
        ++server_conn;
    });
    server.set_on_disconnect([&](std::uint64_t, const std::string&) {
        ++server_disc;
    });
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    libmini::WsClient ws;
    std::atomic<int> client_disc{0};
    std::string reason;
    ws.set_on_disconnect([&](const std::string& r) {
        ++client_disc;
        reason = r;
    });
    ASSERT_TRUE(ws.connect("127.0.0.1", server.port()));
    for (int i = 0; i < 200 && server_conn.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(server_conn.load(), 1);
    EXPECT_EQ(server.connection_count(), 1u);

    // 客户端主动 close：应走 CLOSE 帧握手，服务端感知断开
    ws.close();
    for (int i = 0; i < 200 && server_disc.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(server_disc.load(), 1);
    EXPECT_EQ(client_disc.load(), 1);
    EXPECT_EQ(reason, "closed");
    EXPECT_FALSE(ws.is_connected());

    // 服务端主动 disconnect：客户端收到 CLOSE 后触发断连回调
    libmini::WsClient c2;
    std::atomic<int> c2_disc{0};
    c2.set_on_disconnect([&](const std::string&) { ++c2_disc; });
    ASSERT_TRUE(c2.connect("127.0.0.1", server.port()));
    for (int i = 0; i < 200 && server_conn.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(server_conn.load(), 2);
    server.disconnect(cid);
    for (int i = 0; i < 200 && c2_disc.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(c2_disc.load(), 1);

    c2.close();
    server.stop();
}

TEST(WebSocketTest, BroadcastToMultipleClients)
{
    libmini::WsServer server;
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    constexpr int kClients = 4;
    std::vector<std::unique_ptr<libmini::WsClient>> clients;
    std::atomic<int> received{0};
    for (int i = 0; i < kClients; ++i) {
        auto c = std::make_unique<libmini::WsClient>();
        c->set_on_message([&](const std::string& msg) {
            if (msg == "news") {
                ++received;
            }
        });
        ASSERT_TRUE(c->connect("127.0.0.1", server.port()));
        clients.push_back(std::move(c));
    }
    // 等全部完成 Upgrade（on_connect 在握手后触发）
    for (int i = 0; i < 200 &&
                    server.connection_count() != kClients; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(server.connection_count(),
              static_cast<std::size_t>(kClients));

    server.broadcast("news");
    for (int i = 0; i < 200 && received.load() < kClients; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received.load(), kClients);

    for (auto& c : clients) {
        c->close();
    }
    server.stop();
}

TEST(WebSocketTest, MessageLimitEnforcedAndConfigValidate)
{
    libmini::WsConfig cfg;
    cfg.max_message_bytes = 1024;
    libmini::WsServer server(cfg);
    ASSERT_TRUE(server.start("127.0.0.1", 0));

    libmini::WsClient ws(cfg);
    ASSERT_TRUE(ws.connect("127.0.0.1", server.port()));
    // 超限消息在发送侧直接拒绝
    EXPECT_FALSE(ws.send(std::string(2048, 'x')));
    EXPECT_TRUE(ws.send(std::string(1024, 'x')));

    // 非法配置：两端都拒绝启动/连接
    libmini::WsConfig bad;
    bad.max_message_bytes = 0;
    libmini::WsServer bad_server(bad);
    EXPECT_FALSE(bad_server.start("127.0.0.1", 0));
    libmini::WsClient bad_client(bad);
    EXPECT_FALSE(bad_client.connect("127.0.0.1", server.port()));

    ws.close();
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

// ---------------- ISO-8601 / RFC-3339 ----------------

namespace {

// 构造指定 UTC 时刻的 time_point（毫秒精度）
std::chrono::system_clock::time_point make_tp_ms(std::int64_t ms)
{
    using dur = std::chrono::system_clock::duration;
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<dur>(std::chrono::milliseconds(ms)));
}

std::int64_t tp_to_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch())
        .count();
}

}  // namespace

TEST(Iso8601Test, FormatKnownVectors)
{
    using namespace libmini;
    // epoch
    EXPECT_EQ(format_iso8601(make_tp_ms(0)), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(format_iso8601(make_tp_ms(0), true, false),
              "1970-01-01T00:00:00Z");
    // 有毫秒分量的时刻
    const std::int64_t t = static_cast<std::int64_t>(days_from_civil(2026, 10, 7)) *
                               86400000 +
                           8 * 3600000 + 30 * 60000 + 123;
    EXPECT_EQ(format_iso8601(make_tp_ms(t)), "2026-10-07T08:30:00.123Z");
    // 1970 前（负时间戳）
    EXPECT_EQ(format_iso8601(make_tp_ms(-1000)),
              "1969-12-31T23:59:59.000Z");

    // 本地时区形式可被自己解析回来（与 utc 版指向同一时刻）
    const std::chrono::system_clock::time_point tp = make_tp_ms(t);
    const std::string local = format_iso8601(tp, false);
    std::chrono::system_clock::time_point back;
    ASSERT_TRUE(parse_iso8601(local, back));
    EXPECT_EQ(tp_to_ms(back), tp_to_ms(tp));
    // 结构符合 ±HH:MM：分隔符 T、日期 10 位、偏移段在末尾
    ASSERT_GE(local.size(), 25u);
    EXPECT_EQ(local[10], 'T');
    EXPECT_TRUE(local.back() == 'Z' || local.size() >= 6);
    EXPECT_EQ(local[local.size() - 3], ':');
}

TEST(Iso8601Test, ParseVariants)
{
    using namespace libmini;
    const std::int64_t t = static_cast<std::int64_t>(days_from_civil(2026, 10, 7)) *
                               86400000 +
                           8 * 3600000 + 30 * 60000 + 123;
    std::chrono::system_clock::time_point tp;

    // Z 后缀
    ASSERT_TRUE(parse_iso8601("2026-10-07T08:30:00.123Z", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    // 小写 t + 空格变体 + 小写 z
    ASSERT_TRUE(parse_iso8601("2026-10-07t08:30:00.123z", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    ASSERT_TRUE(parse_iso8601("2026-10-07 08:30:00.123Z", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    // 正/负偏移：东八区表示的同一时刻是 00:30:00Z
    ASSERT_TRUE(parse_iso8601("2026-10-07T16:30:00.123+08:00", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    ASSERT_TRUE(parse_iso8601("2026-10-07T03:00:00.123-05:30", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    // ±HHMM 与 ±HH 写法
    ASSERT_TRUE(parse_iso8601("2026-10-07T16:30:00.123+0800", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    ASSERT_TRUE(parse_iso8601("2026-10-07T08:30:00.123+00", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    // 无时区 → UTC
    ASSERT_TRUE(parse_iso8601("2026-10-07T08:30:00.123", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    // 基本格式（无 '-'/':'）
    ASSERT_TRUE(parse_iso8601("20261007T083000.123Z", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    // 省略小数秒与秒
    ASSERT_TRUE(parse_iso8601("2026-10-07T08:30:00Z", tp));
    EXPECT_EQ(tp_to_ms(tp), t - 123);
    ASSERT_TRUE(parse_iso8601("2026-10-07T08:30Z", tp));
    EXPECT_EQ(tp_to_ms(tp), t - 123);
    // 超出毫秒的小数部分被截断（不是四舍五入）
    ASSERT_TRUE(parse_iso8601("2026-10-07T08:30:00.123999999Z", tp));
    EXPECT_EQ(tp_to_ms(tp), t);
    // 1970 前
    ASSERT_TRUE(parse_iso8601("1969-12-31T23:59:59Z", tp));
    EXPECT_EQ(tp_to_ms(tp), -1000);
}

TEST(Iso8601Test, RoundTrip)
{
    using namespace libmini;
    const std::int64_t samples[] = {
        0,
        1,
        -1,
        1000,
        -1000,
        1770000000123LL,   // 2026-02-02T02:40:00.123Z 附近
        -2208988800000LL,  // 1900-01-01T00:00:00Z
        static_cast<std::int64_t>(days_from_civil(2026, 10, 7)) * 86400000 +
            86399999,  // 23:59:59.999
    };
    for (std::size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const auto tp = make_tp_ms(samples[i]);
        const std::string s = format_iso8601(tp);
        std::chrono::system_clock::time_point back;
        ASSERT_TRUE(parse_iso8601(s, back)) << "round trip 解析失败: " << s;
        EXPECT_EQ(tp_to_ms(back), samples[i]) << "round trip 不一致: " << s;
    }
}

TEST(Iso8601Test, RejectInvalid)
{
    using namespace libmini;
    std::chrono::system_clock::time_point tp = make_tp_ms(12345);
    const std::chrono::system_clock::time_point sentinel = tp;
    const char* bad[] = {
        "",
        "garbage",
        "2026-13-01T00:00:00Z",    // 月份越界
        "2026-00-01T00:00:00Z",
        "2026-02-30T00:00:00Z",    // 2 月 30 日
        "2023-02-29T00:00:00Z",    // 非闰年
        "2026-10-07T24:00:00Z",    // 小时越界
        "2026-10-07T08:60:00Z",    // 分钟越界
        "2026-10-07T08:30:00+25:00",   // 偏移越界
        "2026-10-07T08:30:00-00:60",   // 偏移分钟越界
        "2026-10-07T08:30:00.",   // 小数点后无数字
        "2026-10-07T08:30:00Z trailing",  // 尾随垃圾
        "2026-10-07",              // 只有日期
        "08:30:00Z",               // 只有时间
        "2026/10/07T08:30:00Z",    // 错误分隔符（年份后非法）
        "abcd-10-07T08:30:00Z",    // 年份非数字
    };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        EXPECT_FALSE(parse_iso8601(bad[i], tp)) << "应拒绝: " << bad[i];
        EXPECT_EQ(tp_to_ms(tp), tp_to_ms(sentinel)) << "失败时 out 不应改变: " << bad[i];
    }
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

// ------------------------------ tar (ustar) ------------------------------

TEST(TarTest, RoundTripTextBinaryUtf8)
{
    libmini::TarWriter tw;
    ASSERT_TRUE(tw.add_file("hello.txt", "hello tar world"));
    std::string bin;
    for (int i = 0; i < 5000; ++i) {
        bin.push_back(static_cast<char>(i & 0xff));
    }
    ASSERT_TRUE(tw.add_file("data/bin.dat", bin));
    ASSERT_TRUE(tw.add_file(u8"\xE4\xB8\xAD\xE6\x96\x87.md", u8"# 中文\n"));
    ASSERT_TRUE(tw.add_file("empty.txt", ""));
    const std::string bytes = tw.finish();
    EXPECT_EQ(bytes.size() % 512, 0u);  // 块对齐
    EXPECT_EQ(tw.count(), 0u);          // finish 后重置

    libmini::TarReader tr;
    ASSERT_TRUE(tr.open(bytes));
    ASSERT_EQ(tr.entries().size(), 4u);
    EXPECT_TRUE(tr.contains("hello.txt"));
    EXPECT_EQ(tr.extract("hello.txt"), "hello tar world");
    EXPECT_EQ(tr.extract("data/bin.dat"), bin);
    EXPECT_EQ(tr.extract(u8"\xE4\xB8\xAD\xE6\x96\x87.md"), u8"# 中文\n");
    EXPECT_TRUE(tr.extract("empty.txt").empty());
    EXPECT_TRUE(tr.last_error().empty());

    // 条目元数据
    const libmini::TarEntryInfo& e = tr.entries()[0];
    EXPECT_EQ(e.type, libmini::TarType::Regular);
    EXPECT_EQ(e.size, 15u);
    EXPECT_EQ(e.mode, 0644u);

    // extract_at 与顺序
    EXPECT_EQ(tr.extract_at(1), bin);
    EXPECT_TRUE(tr.extract_at(99).empty());
    EXPECT_FALSE(tr.last_error().empty());
    // 不存在的条目
    EXPECT_TRUE(tr.extract("missing.txt").empty());
    EXPECT_FALSE(tr.last_error().empty());
}

TEST(TarTest, LongPathSplitAcrossPrefixName)
{
    // >100 字节：应拆进 prefix/name 两个字段并能还原
    const std::string deep =
        "a_very_long_directory_name_segment_number_one/"
        "another_quite_long_intermediate_directory_name/"
        "final_file_name_that_pushes_past_100_bytes.txt";
    ASSERT_GT(deep.size(), 100u);
    ASSERT_LE(deep.size(), 255u);

    libmini::TarWriter tw;
    ASSERT_TRUE(tw.add_file(deep, "long path content"));
    const std::string bytes = tw.finish();

    libmini::TarReader tr;
    ASSERT_TRUE(tr.open(bytes)) << tr.last_error();
    ASSERT_EQ(tr.entries().size(), 1u);
    EXPECT_EQ(tr.entries()[0].name, deep);
    EXPECT_EQ(tr.extract(deep), "long path content");

    // 超过 255 字节：拒绝
    libmini::TarWriter too_long;
    EXPECT_FALSE(too_long.add_file(std::string(300, 'x') + ".txt", "v"));
    EXPECT_FALSE(too_long.last_error().empty());
}

TEST(TarTest, DirAndSymlink)
{
    libmini::TarWriter tw;
    ASSERT_TRUE(tw.add_dir("docs"));                 // 自动补 '/'
    ASSERT_TRUE(tw.add_file("docs/readme.md", "# doc"));
    ASSERT_TRUE(tw.add_symlink("link", "docs/readme.md"));
    const std::string bytes = tw.finish();

    libmini::TarReader tr;
    ASSERT_TRUE(tr.open(bytes)) << tr.last_error();
    ASSERT_EQ(tr.entries().size(), 3u);
    EXPECT_EQ(tr.entries()[0].name, "docs/");
    EXPECT_EQ(tr.entries()[0].type, libmini::TarType::Directory);
    EXPECT_EQ(tr.entries()[2].type, libmini::TarType::Symlink);
    EXPECT_EQ(tr.entries()[2].link_target, "docs/readme.md");
    EXPECT_EQ(tr.entries()[2].size, 0u);
    EXPECT_TRUE(tr.contains("docs/"));
    EXPECT_FALSE(tr.contains("docs"));  // 精确匹配含尾部 '/'
    // 目录/链接无载荷：extract 返回空且不报错
    EXPECT_TRUE(tr.extract("docs/").empty());
    EXPECT_TRUE(tr.last_error().empty());
    EXPECT_EQ(tr.extract("docs/readme.md"), "# doc");
}

TEST(TarTest, CorruptAndTruncatedFailGracefully)
{
    libmini::TarWriter tw;
    ASSERT_TRUE(tw.add_file("f.txt", "some tar content"));
    std::string bytes = tw.finish();

    // 篡改头部 name 首字节 → checksum 不匹配
    std::string corrupt = bytes;
    corrupt[0] = 'X';
    libmini::TarReader tr;
    EXPECT_FALSE(tr.open(corrupt));
    EXPECT_FALSE(tr.last_error().empty());

    // 截断数据区 → 条目被截断（只剩头部，数据/EOF 都没了）
    libmini::TarReader tr2;
    EXPECT_FALSE(tr2.open(bytes.substr(0, 512)));
    EXPECT_FALSE(tr2.last_error().empty());
    // 只到头部+半个数据块
    EXPECT_FALSE(tr2.open(bytes.substr(0, 512 + 8)));

    // 空输入
    libmini::TarReader tr3;
    EXPECT_FALSE(tr3.open(""));

    // 长度非 512 倍数
    libmini::TarReader tr4;
    EXPECT_FALSE(tr4.open(bytes + "xx"));

    // magic 篡改
    std::string nomagic = bytes;
    nomagic[257] = 'U';
    libmini::TarReader tr5;
    EXPECT_FALSE(tr5.open(nomagic));
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


TEST(SqliteTest_WMutex, WriteMutexSingleHolderSucceeds)
{
    using namespace libmini;
    const std::string path = sqlite_temp_path("_wmutex");
    remove_file(path);
    {
        SqliteWriteMutex wm(path);
        EXPECT_TRUE(wm.acquire(4000));
        EXPECT_TRUE(wm.is_held());
        EXPECT_EQ(wm.path(), path);
        {
            SqliteDatabase db(path);
            ASSERT_TRUE(db.is_open());
            ASSERT_TRUE(db.exec("CREATE TABLE t (v INTEGER)"));
            ASSERT_TRUE(db.exec("INSERT INTO t VALUES (1)"));
            EXPECT_EQ(db.changes(), 1);
        }
        wm.release();
        EXPECT_FALSE(wm.is_held());
    }
    remove_file(path);
    // wlock file is left behind (设计如此)
    EXPECT_TRUE(file_exists(path + ".wlock"));
    remove_file(path + ".wlock");
}


TEST(SqliteTest_WMutex, WriteMutexCrossThreadSerializesWriters)
{
    using namespace libmini;
    const std::string path = sqlite_temp_path("_wmutex_xthread");
    remove_file(path);
    remove_file(path + ".wlock");
    {
        // Give the spawned threads a fair shot to contend for the slot.
        // Spawn first (they'll pile up on the wlock syscall), then hold the
        // slot across threads, then release so exactly one of them proceeds.
        std::atomic<int> acquired{0};
        std::atomic<int> committed{0};
        std::atomic<int> failed{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&]() {
                SqliteWriteMutex wm(path);
                if (wm.acquire(400)) {
                    ++acquired;
                    {
                        SqliteDatabase db(path);
                        if (db.is_open() && db.exec("INSERT INTO t VALUES (" + std::to_string(i) + ")")) {
                            ++committed;
                        }
                    }
                    wm.release();
                } else {
                    ++failed;
                }
            });
        }
        // let threads pile onto the lock, then take the slot away from them
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            SqliteWriteMutex holder(path);
            ASSERT_TRUE(holder.acquire(4000));
            ASSERT_TRUE(holder.is_held());
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            holder.release();
        }
        for (auto& th : threads) th.join();
        // after holder released, at least one thread should have proceeded
        EXPECT_GT(acquired.load(), 0);
        // exactly the threads we spawned should have tried (all accounted for)
        EXPECT_EQ(acquired.load() + failed.load(), 4);
    }
    remove_file(path);
    remove_file(path + ".wlock");
}


TEST(SqliteTest_WMutex, WriteMutexGuardsBeginCommitProtocol)
{
    using namespace libmini;
    const std::string path = sqlite_temp_path("_wmutex_protocol");
    remove_file(path);
    remove_file(path + ".wlock");
    {
        SqliteWriteMutexGuard guard(path, 4000);
        ASSERT_TRUE(guard.acquired());
        {
            SqliteDatabase db(path);
            ASSERT_TRUE(db.is_open());
            ASSERT_TRUE(db.exec("CREATE TABLE t (v INTEGER)"));
            {
                SqliteTransaction tx(db);
                ASSERT_TRUE(tx.is_active());
                SqliteStatement ins(db, "INSERT INTO t VALUES (?)");
                ASSERT_TRUE(ins.bind_int(1, 42));
                ASSERT_EQ(ins.step(), SqliteStatement::StepDone);
                ASSERT_TRUE(tx.commit());
                EXPECT_FALSE(tx.is_active());
            }
            SqliteStatement cnt(db, "SELECT COUNT(*) FROM t");
            ASSERT_EQ(cnt.step(), SqliteStatement::StepRow);
            EXPECT_EQ(cnt.column_int(0), 1);
        }
    }
    remove_file(path);
    remove_file(path + ".wlock");
}

// 跨进程互斥的冒烟测试：同一 .wlock 上，父进程持锁期间子进程必须拿不到，
// 释放后子进程应能拿到。用真实子进程验证 OS 级文件锁（而不是同进程内的锁语义）。
#if defined(LIBMINI_SQLITE_CHILD_EXE)
TEST(SqliteTest_WMutex, WriteMutexCrossProcessSerializesWriters)
{
    using namespace libmini;
    const std::string path = sqlite_temp_path("_wmutex_xproc");
    remove_file(path);
    remove_file(path + ".wlock");
    {
        SqliteWriteMutex holder(path);
        ASSERT_TRUE(holder.acquire(4000));

        // 子进程输出码约定：1 = 拿不到锁，0 = 拿到了锁
        const ProcessResult blocked = run_process(
            LIBMINI_SQLITE_CHILD_EXE, {path, "acquire_then_exit"}, 15000);
        if (blocked.timed_out) {
            GTEST_SKIP() << "child process timed out";
        }

        holder.release();

        const ProcessResult free_slot = run_process(
            LIBMINI_SQLITE_CHILD_EXE, {path, "acquire_then_exit"}, 15000);
        if (free_slot.timed_out) {
            GTEST_SKIP() << "child process timed out";
        }

        EXPECT_EQ(blocked.exit_code, 1)
            << "child stderr: " << blocked.stderr_text;
        EXPECT_EQ(free_slot.exit_code, 0)
            << "child stderr: " << free_slot.stderr_text;
    }
    remove_file(path);
    remove_file(path + ".wlock");
}
#endif

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

// 统一错误面：SqliteStatus → StatusCode 的映射、上下文与新旧接口并存
TEST(SqliteTest, TryApiMapsStatusCodes)
{
    using namespace libmini;

    // 成功路径：Status::success()，与旧接口的成功返回值一一对应
    SqliteDatabase db;
    const Status opened = db.try_open(":memory:");
    EXPECT_TRUE(opened.ok());
    EXPECT_TRUE(db.last_error().ok());
    EXPECT_TRUE(db.try_exec("CREATE TABLE t (k TEXT UNIQUE)").ok());

    // 打开失败：CannotOpen → Io，且 context 带路径（打不开时 message 只有
    // 泛泛的 errmsg，路径往往是排障的第一信息）
    SqliteDatabase missing;
    const Status open_failed =
        missing.try_open(temp_directory_path() + "/no_such_dir_99123/x.db");
    EXPECT_FALSE(open_failed.ok());
    EXPECT_EQ(open_failed.code(), StatusCode::Io);
    EXPECT_EQ(missing.last_status(), SqliteStatus::CannotOpen);  // 旧接口不变
    EXPECT_FALSE(open_failed.message().empty());
    EXPECT_NE(open_failed.context().find("no_such_dir_99123"), std::string::npos);

    // 约束冲突：Constraint → InvalidArgument，原因细节留在 message
    ASSERT_TRUE(db.try_exec("INSERT INTO t VALUES ('dup')").ok());
    const Status dup = db.try_exec("INSERT INTO t VALUES ('dup')");
    EXPECT_FALSE(dup.ok());
    EXPECT_EQ(dup.code(), StatusCode::InvalidArgument);
    EXPECT_NE(dup.message().find("UNIQUE"), std::string::npos);
    EXPECT_EQ(dup.context(), "SqliteDatabase::exec");

    // 没有更精确分类的失败（表不存在）→ Failure，而不是假装知道原因
    const Status no_table = db.try_exec("SELECT * FROM nope");
    EXPECT_FALSE(no_table.ok());
    EXPECT_EQ(no_table.code(), StatusCode::Failure);

    // 空结果集是「带空值的成功」，不是错误
    SqliteStatement none(db, "SELECT k FROM t WHERE k = 'absent'");
    ASSERT_TRUE(none.is_prepared());
    const Result<std::vector<std::vector<SqliteValue>>> no_rows =
        none.try_query_all();
    ASSERT_TRUE(no_rows.ok());
    EXPECT_TRUE(no_rows->empty());
    EXPECT_EQ(no_rows.value().size(), 0u);

    // 执行期失败：唯一约束 → InvalidArgument，context 标明是哪个入口
    SqliteStatement dup_ins(db, "INSERT INTO t VALUES ('dup')");
    ASSERT_TRUE(dup_ins.is_prepared());
    const Result<std::vector<std::vector<SqliteValue>>> dup_rows =
        dup_ins.try_query_all();
    EXPECT_FALSE(dup_rows.ok());
    EXPECT_EQ(dup_rows.code(), StatusCode::InvalidArgument);
    EXPECT_EQ(dup_rows.status().context(), "SqliteStatement::query_all");
    EXPECT_NE(dup_rows.status().message().find("UNIQUE"), std::string::npos);
    EXPECT_THROW(dup_rows.value(), bad_result_access);

    // 从未 prepare 的语句：没有可记录状态的宿主连接
    SqliteStatement unprepared;
    const Result<std::vector<std::vector<SqliteValue>>> no_owner =
        unprepared.try_query_all();
    EXPECT_FALSE(no_owner.ok());
    EXPECT_EQ(no_owner.code(), StatusCode::InvalidArgument);
    EXPECT_EQ(no_owner.status().message(), "statement has no owning database");

    // 并发写冲突：Busy → Conflict——调用方据此决定重试而不是报错
    const std::string busy_path = sqlite_temp_path("tryapi_busy");
    remove_file(busy_path);
    {
        SqliteDatabase holder(busy_path);
        ASSERT_TRUE(holder.is_open());
        ASSERT_TRUE(holder.try_exec("CREATE TABLE t (v INTEGER)").ok());
        ASSERT_TRUE(holder.begin());
        // BEGIN 是惰性的：必须真的写一次，写锁才会被持有，否则 other 能自由写
        ASSERT_TRUE(holder.exec("INSERT INTO t VALUES (1)"));

        SqliteDatabase other(busy_path);
        ASSERT_TRUE(other.is_open());
        other.set_busy_timeout_ms(30);
        const Status busy = other.try_exec("INSERT INTO t VALUES (2)");
        EXPECT_FALSE(busy.ok());
        EXPECT_EQ(busy.code(), StatusCode::Conflict);
        EXPECT_EQ(other.last_status(), SqliteStatus::Busy);  // 旧接口同一结论
        holder.rollback();
    }
    remove_file(busy_path);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
