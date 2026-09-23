// libmini 各模块功能演示
//
// 本程序自身就是 libmini 的真实用例：入口参数用 libmini::Args 解析，
// 每节耗时用 Stopwatch 统计。
//
// 用法：
//   demo_libmini                     运行全部演示
//   demo_libmini --only string,crc   只运行指定节
//   demo_libmini --list              列出全部演示节
//   demo_libmini -h                  帮助
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>

#include <spdlog/spdlog.h>  // LogFacade::logger() 返回指针，调用方法需完整类型
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "libmini.h"

using namespace libmini;

namespace {

// ------------------------- 演示框架 -------------------------

struct DemoSection {
    const char* name;
    void (*run)();
};

std::vector<DemoSection>& sections()
{
    static std::vector<DemoSection> s;
    return s;
}

int register_section(const char* name, void (*run)())
{
    sections().push_back(DemoSection{name, run});
    return 0;
}

#define LIBMINI_DEMO(name)                                               \
    static void demo_##name();                                           \
    static const int k_reg_##name = register_section(#name, demo_##name); \
    static void demo_##name()

void sleep_for_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 定长小数格式化（避免污染 cout 的全局格式状态）
std::string fmt_num(double v, int prec)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

// 已升序排序样本的百分位值；permille 为千分比（500 = P50，990 = P99）
double percentile_of(const std::vector<double>& sorted, int permille)
{
    if (sorted.empty()) {
        return 0.0;
    }
    std::size_t idx = (sorted.size() * static_cast<std::size_t>(permille) + 999)
                      / 1000;
    if (idx == 0) {
        idx = 1;
    }
    if (idx > sorted.size()) {
        idx = sorted.size();
    }
    return sorted[idx - 1];
}

// ------------------------- 各模块演示 -------------------------

LIBMINI_DEMO(string)
{
    std::cout << "split_string  : " << join(split_string("a::b::c", "::"), " | ")
              << "\n";
    std::cout << "replace_all   : " << replace_all("2026-09-18", "-", "/")
              << "\n";
    std::cout << "iequals       : "
              << (iequals("Hello", "hELLO") ? "true" : "false") << "\n";
    std::cout << "lexical_cast  : 42 + 1 = " << lexical_cast<int>("42") + 1
              << "\n";
    std::cout << "cast_or(默认) : " << lexical_cast_or<int>("not-a-number", -1)
              << "\n";
}

LIBMINI_DEMO(random_uuid)
{
    std::cout << "random_int    : " << random_int(1, 6) << " (骰子)\n";
    std::cout << "random_string : " << random_string(12) << "\n";
    std::cout << "uuid          : " << Uuid::generate().to_string() << "\n";
    Uuid parsed;
    if (Uuid::parse("f47ac10b-58cc-4372-a567-0e02b2c3d479", parsed)) {
        std::cout << "uuid parse    : " << parsed.to_hex_string() << "\n";
    }
}

LIBMINI_DEMO(time_stopwatch)
{
    Stopwatch sw;
    sleep_for_ms(120);
    std::cout << "sleep 120ms 实测  : " << sw.elapsed_string() << "\n";
    sw.pause();
    sleep_for_ms(100);   // 暂停期间不计时
    sw.resume();
    sleep_for_ms(30);
    std::cout << "暂停 100ms 后累计 : " << sw.elapsed_string() << " (约 150ms)\n";
    std::cout << "当前时间戳        : " << current_timestamp_ms() << "\n";
    std::cout << "格式化时间        : "
              << format_time(std::chrono::system_clock::now()) << "\n";
}

LIBMINI_DEMO(file_path)
{
    const std::string fname = "demo_演示文件.txt";
    write_file(fname, "hello 你好");
    std::cout << "write/read    : " << read_file(fname) << "\n";
    std::cout << "file_size     : " << file_size(fname) << " bytes\n";

    const std::string full = path_join("logs", "app.log");
    std::cout << "path_join     : " << full << " -> dir=" << dirname(full)
              << ", base=" << basename(full) << ", ext=" << extension(full)
              << "\n";

    // 路径处理：规范化解析 ./..、替换扩展名、分隔符统一
    std::cout << "normalize     : " << normalize_path("a/b/../c/./d") << "\n";
    std::cout << "replace_ext   : " << replace_extension("report.txt", ".md")
              << "\n";
    std::cout << "stem/parent   : " << stem("archive.tar.gz") << " / "
              << parent_path("a/b/c.txt") << "\n";

    // 目录操作：递归创建 → 详单（类型/大小/时间）→ 树复制 → 树删除
    const std::string root = unique_temp_path("demo_fs_");
    make_directories(root + "/子目录/深层");
    write_file(path_join(root, "子目录", "数据.txt"), "demo");
    const std::vector<DirEntry> entries = list_directory_detailed(root + "/子目录");
    for (size_t i = 0; i < entries.size(); ++i) {
        std::cout << "entry         : " << entries[i].name
                  << "  "
                  << (entries[i].kind == EntryKind::Directory ? "<dir>" : "<file>")
                  << "  " << entries[i].size << "B\n";
    }
    copy_tree(root + "/子目录", root + "/副本");
    std::cout << "copy_tree     : "
              << (file_exists(path_join(root, "副本", "数据.txt"))
                      ? "副本/数据.txt 已复制"
                      : "复制失败")
              << "\n";
    remove_tree(root);
    std::cout << "remove_tree   : "
              << (file_exists(root) ? "失败" : "目录树已删除") << "\n";

    remove_file(fname);
    std::cout << "remove_file   : "
              << (file_exists(fname) ? "失败" : "已删除") << "\n";
}

LIBMINI_DEMO(encoding)
{
    const std::string raw = "libmini 编码演示";
    std::string back;
    Base64::decode(Base64::encode(raw), back);
    std::cout << "base64        : " << Base64::encode(raw) << " -> " << back
              << "\n";
    std::cout << "hex           : " << Hex::encode("\xDE\xAD\xBE\xEF") << "\n";
    std::cout << "url encode    : " << UrlEncode::encode("a b&c=值") << "\n";
    std::cout << "url decode    : " << UrlEncode::decode("a+b%26c%3D%E5%80%BC")
              << "\n";
}

LIBMINI_DEMO(digest_crc)
{
    std::cout << "md5(\"libmini\") : " << Md5::hex("libmini") << "\n";
    std::cout << "sha256        : " << Sha256::hex("libmini").substr(0, 32)
              << "...\n";

    Crc32 crc;
    crc.update("hello ");
    crc.update("world");
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", crc.value());
    std::cout << "crc32(增量)   : 0x" << buf << "\n";
}

LIBMINI_DEMO(json_xml)
{
    const JsonValue v = parse_json(R"({"name":"libmini","stars":5})");
    std::cout << "json parse    : name=" << v["name"].get<std::string>()
              << ", stars=" << v["stars"].get<int>() << "\n";
    std::cout << "json build    : "
              << to_json_string(
                     JsonValue{{"ok", true}, {"ts", current_timestamp_ms()}})
              << "\n";

    const std::string xml = "<config><timeout seconds='30'/></config>";
    const XmlDocument doc = parse_xml(xml);
    std::cout << "xml parse     : timeout.seconds="
              << doc.child("config").child("timeout").attribute("seconds").as_int()
              << "\n";
    std::cout << "xml serialize : " << to_xml_string(doc) << "\n";
}

// 序列化演示用的结构体：一行宏即可获得 JSON 读写字段
struct DemoHost {
    std::string name;
    int port;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DemoHost, name, port)

LIBMINI_DEMO(serialization)
{
    DemoHost host;
    host.name = "primary";
    host.port = 6379;

    const std::string json = serialize_to_json(host);
    std::cout << "to_json       : " << json << "\n";

    const DemoHost back = deserialize_from_json<DemoHost>(json);
    std::cout << "from_json     : " << back.name << ":" << back.port << "\n";

    // 坏数据不抛异常，走 fallback
    std::cout << "json fallback : "
              << deserialize_from_json_or<DemoHost>("{broken", host).name
              << "\n";

    const std::string xml = serialize_to_xml(host);
    std::cout << "to_xml        : " << xml << "\n";
    std::cout << "from_xml      : "
              << deserialize_from_xml<DemoHost>(xml).port << "\n";
}

LIBMINI_DEMO(ini_env)
{
    IniConfig cfg;
    cfg.parse("[server]\nhost = 127.0.0.1\nport = 9090\nverbose = yes\n");
    std::cout << "ini           : " << cfg.get("server", "host") << ":"
              << cfg.get_int("server", "port") << " verbose="
              << (cfg.get_bool("server", "verbose") ? "on" : "off") << "\n";

    env_set("LIBMINI_DEMO_HOME", "/opt/libmini");
    std::cout << "env expand    : " << env_expand("%LIBMINI_DEMO_HOME%/logs")
              << "\n";
    env_remove("LIBMINI_DEMO_HOME");
}

LIBMINI_DEMO(gzip)
{
    const std::string payload(2000, 'x');
    const optional<std::string> z = gzip_compress(payload, 9);
    if (z.has_value()) {
        std::cout << "gzip          : " << payload.size() << " -> "
                  << z->size() << " bytes\n";
        const optional<std::string> back = gzip_decompress(*z);
        std::cout << "gunzip 校验   : "
                  << (back.has_value() && *back == payload ? "一致" : "失败")
                  << "\n";
    } else {
        std::cout << "gzip          : 压缩失败\n";
    }
}

LIBMINI_DEMO(thread_pool)
{
    ThreadPool pool(4);
    std::vector<std::future<int> > futures;
    for (int i = 1; i <= 8; ++i) {
        futures.push_back(pool.submit([i] { return i * i; }));
    }
    std::cout << "submit(i*i)   : ";
    for (size_t i = 0; i < futures.size(); ++i) {
        std::cout << futures[i].get() << (i + 1 < futures.size() ? " " : "\n");
    }
}

LIBMINI_DEMO(file_watch)
{
    // 跨进程文件锁：第二个实例拿不到锁
    const std::string lock_path = unique_temp_path("demo_lock_");
    {
        FileLockGuard guard(lock_path, true, 0);
        const bool concurrent = [&] {
            FileLockGuard other(lock_path, true, 0);
            return other.holds_lock();
        }();
        std::cout << "file_lock     : 持有="
                  << (guard.holds_lock() ? "是" : "否")
                  << "，并发获取=" << (concurrent ? "成功（异常）" : "被拒（正确）")
                  << "\n";
        // guard 析构自动释放
    }
    {
        FileLockGuard re(lock_path, true, 0);
        std::cout << "file_lock     : 释放后再次获取="
                  << (re.holds_lock() ? "成功" : "失败") << "\n";
    }

    // 目录监听：创建 → 修改 → 删除，回调记录事件
    const std::string dir = unique_temp_path("demo_watch_");
    make_directories(dir);
    DirWatcher watcher;
    std::vector<std::string> seen;
    std::mutex mu;
    watcher.set_callback([&](const WatchNotification& n) {
        const char* tag = n.event == WatchEvent::Created  ? "创建"
                          : n.event == WatchEvent::Modified ? "修改"
                          : n.event == WatchEvent::Removed   ? "删除"
                                                             : "重命名";
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back(std::string(tag) + " " + n.name);
    });
    if (!watcher.start(dir, false)) {
        std::cout << "dir_watch     : 平台不支持或目录无效\n";
        remove_tree(dir);
        return;
    }
    std::cout << "dir_watch     : 开始监听 " << basename(dir) << "\n";

    write_file(dir + "/事件.txt", "第 1 行");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    append_file(dir + "/事件.txt", "第 2 行");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    remove_file(dir + "/事件.txt");

    for (int i = 0; i < 50 && seen.size() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    watcher.stop();
    std::lock_guard<std::mutex> lock(mu);
    for (size_t i = 0; i < seen.size(); ++i) {
        std::cout << "  事件[" << i << "]     : " << seen[i] << "\n";
    }
    remove_tree(dir);
    std::cout << "dir_watch     : 监听到 " << seen.size()
              << " 次变化，资源已清理\n";
}

LIBMINI_DEMO(config_hot_reload)
{
    // DirWatcher + IniConfig + 退避轮询：配置文件热更新全链路演示。
    // 场景：后台线程监听配置目录，变更防抖后自动重载并应用。

    // 1. 写初始配置
    const std::string cfg_path = "hot_reload_demo.ini";
    write_file(cfg_path, "[server]\nport = 8000\nname = demo\n");

    // 2. 启动监听（子树无所谓，单文件直接监听所在目录）
    DirWatcher watcher;
    std::atomic<int> change_events{0};
    watcher.set_callback([&](const WatchNotification& n) {
        if (n.name == cfg_path &&
            (n.event == WatchEvent::Modified || n.event == WatchEvent::Created)) {
            ++change_events;
        }
    });
    const bool watching = watcher.start(".", /*watch_subtree=*/false);
    std::cout << "watcher       : " << (watching ? "已监听" : "启动失败(跳过)")
              << "\n";

    // 3. 首次加载
    IniConfig cfg;
    cfg.parse(read_file(cfg_path));
    int applied_port = cfg.get_int("server", "port");
    std::cout << "initial port  : " << applied_port << "\n";

    // 4. 修改配置 → 等监听回调 → 防抖 100ms 后重载
    if (watching) {
        sleep_for_ms(150);  // 让监听就绪
        write_file(cfg_path, "[server]\nport = 9001\nname = demo-v2\n");

        // 等待变更事件（最多 2s；防抖 = 两次读取间隔 > 100ms 且内容已变）
        bool reloaded = false;
        for (int i = 0; i < 200 && !reloaded; ++i) {
            sleep_for_ms(10);
            if (change_events.load() > 0) {
                // 模拟防抖：事件到达后稍等再读，避免读到半写状态
                sleep_for_ms(100);
                IniConfig fresh;
                fresh.parse(read_file(cfg_path));
                const int new_port = fresh.get_int("server", "port");
                if (new_port != applied_port) {
                    applied_port = new_port;
                    reloaded = true;
                }
            }
        }
        std::cout << "hot reload    : 事件 " << change_events.load()
                  << " 次, port " << 8000 << " -> " << applied_port
                  << (reloaded ? " (已应用)" : " (超时)") << "\n";
        watcher.stop();
    } else {
        std::cout << "hot reload    : 跳过（监听不可用）\n";
    }

    // 5. 加密敏感配置段（AES-256-GCM，落盘格式 nonce||ct||tag）
    const std::string key(32, 'd');  // 实际项目从凭据存储获取
    const std::string sealed = Aes256Gcm::seal(key, "db_password=s3cret");
    write_file("hot_reload_demo.bin", sealed);
    const std::string opened =
        Aes256Gcm::open(key, read_file("hot_reload_demo.bin"));
    std::cout << "aes sealed cfg: " << (opened == "db_password=s3cret" ? "解密一致" : "失败")
              << "\n";

    remove_file(cfg_path);
    remove_file("hot_reload_demo.bin");
}

LIBMINI_DEMO(async_rate)
{
    AsyncScheduler sched;

    // 延时任务
    std::atomic<bool> fired{false};
    sched.run_after_ms(50, [&fired] { fired = true; });

    // 周期任务：跑 3 次自动停
    std::atomic<int> ticks{0};
    sched.run_every_ms(30,
                       [&ticks]() -> bool { return ticks.fetch_add(1) + 1 < 3; });

    while (!fired.load() || ticks.load() < 3) {
        sleep_for_ms(10);
    }
    std::cout << "run_after_ms  : 已触发\n";
    std::cout << "run_every_ms  : 触发 " << ticks.load() << " 次后自停\n";

    // 限流器：稳态 50/s，桶 2
    RateLimiter limiter(50.0, 2.0);
    int granted = 0;
    Stopwatch sw;
    while (sw.elapsed_ms() < 100) {
        if (limiter.try_acquire()) {
            ++granted;
        }
        sleep_for_ms(5);
    }
    std::cout << "rate limiter  : 100ms 内放行 " << granted
              << " 次（桶 2 + 按速率回补）\n";
}

LIBMINI_DEMO(optional_scope)
{
    // optional：有值/空值两种状态
    optional<int> has_value = 7;
    optional<int> none = nullopt;
    std::cout << "optional      : value_or=" << has_value.value_or(-1)
              << ", 空=" << none.value_or(-1) << "\n";

    // scope_guard：作用域结束自动执行
    int guard_calls = 0;
    {
        auto guard = make_scope_guard([&guard_calls] { ++guard_calls; });
        std::cout << "scope_guard   : 作用域内 guard_calls=" << guard_calls
                  << "\n";
    }
    std::cout << "scope_guard   : 作用域结束自动执行, guard_calls="
              << guard_calls << "\n";
}

LIBMINI_DEMO(args_self)
{
    std::cout << "本程序入口即用 Args 解析：\n";
    std::cout << "  定义  : --only <list>, --list, -h/--help 内置\n";
    std::cout << "  规则  : --key=value / --key value / --flag / -- 终止符\n";
    std::cout << "  试试  : demo_libmini --only string,rpc 或 --list\n";
}

// RPC 回环演示：多客户端并发压测（QPS / 延迟分位 / 过载拒绝率）
LIBMINI_DEMO(rpc)
{
    RpcServer server(0);   // 端口 0 = 自动分配
    std::atomic<int> served{0};
    server.register_method("add", [&](const std::string& params) {
        served.fetch_add(1);
        sleep_for_ms(1);   // 模拟真实处理耗时，制造可观测的延迟与过载
        const JsonValue p = parse_json(params);
        return to_json_string(
            JsonValue{{"sum", p["a"].get<int>() + p["b"].get<int>()}});
    });
    // 处理槽位 = 2：并发客户端超过 2 个时必然出现 429 拒绝
    server.set_max_in_flight(2);
    server.set_overload_message("demo busy");
    server.start_background();
    if (!server.wait_until_ready(5000)) {
        std::cout << "rpc           : 服务端启动失败\n";
        return;
    }
    const int port = server.port();

    // ---- 单调用冒烟 ----
    {
        RpcClient c("127.0.0.1", port);
        const std::string reply = c.call("add", R"({"a":20,"b":22})");
        std::cout << "rpc add       : " << reply << "\n";
    }

    // ---- 并发压测 ----
    // 客户端禁用重试：429 立即返回，延迟样本保持真实耗时，
    // 拒绝率也不会被退避等待稀释
    const int k_threads = 8;
    const int k_calls = 150;              // 每客户端请求数（总计 1200）
    std::vector<double> latencies;
    std::mutex lat_mu;
    std::atomic<int> rejected{0};
    std::atomic<int> failed_other{0};
    Stopwatch wall;

    {
        std::vector<std::thread> workers;
        for (int t = 0; t < k_threads; ++t) {
            (void)t;
            workers.push_back(std::thread([&] {
                RpcClient client("127.0.0.1", port);
                client.set_max_retries(0);   // 429 立即失败，不退避
                client.set_timeout_ms(2000);
                for (int i = 0; i < k_calls; ++i) {
                    Stopwatch sw;
                    const std::string r =
                        client.call("add", R"({"a":1,"b":2})");
                    const double ms = static_cast<double>(sw.elapsed_ms());
                    if (!r.empty()) {
                        std::lock_guard<std::mutex> lock(lat_mu);
                        latencies.push_back(ms);
                    } else if (client.last_error() == RpcError::OVERLOADED) {
                        rejected.fetch_add(1);      // 429 过载拒绝
                    } else {
                        failed_other.fetch_add(1);  // 意外失败，应接近 0
                    }
                }
            }));
        }
        for (size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
    }

    const double wall_s = wall.elapsed_ms() / 1000.0;
    const int total = k_threads * k_calls;
    const int ok = static_cast<int>(latencies.size());
    const int rej = rejected.load();
    const int other = failed_other.load();

    std::sort(latencies.begin(), latencies.end());
    std::cout << "rpc 压测      : " << k_threads << " 线程 x " << k_calls
              << " 次 = " << total << " 请求，处理槽 2，耗时 "
              << fmt_num(wall_s, 2) << "s\n";
    std::cout << "rpc QPS       : 总 " << fmt_num(total / wall_s, 0)
              << " / 成功 " << fmt_num(ok / wall_s, 0)
              << " （成功 " << ok << " / 拒绝 " << rej << " / 其他失败 "
              << other << "）\n";
    std::cout << "rpc 延迟(ms)  : P50="
              << fmt_num(percentile_of(latencies, 500), 1)
              << "  P95=" << fmt_num(percentile_of(latencies, 950), 1)
              << "  P99=" << fmt_num(percentile_of(latencies, 990), 1)
              << "  max="
              << fmt_num(latencies.empty() ? 0.0 : latencies.back(), 1) << "\n";
    std::cout << "rpc 拒绝率    : "
              << fmt_num(total ? 100.0 * rej / total : 0.0, 1) << "%\n";

    const RpcServerStats st = server.stats();
    std::cout << "rpc 服务端    : 处理 " << served.load() << "，拒绝 "
              << st.rejected_total << "，峰值并发 " << st.max_active
              << "，峰值队列 " << st.max_queued << "\n";

    // 服务端延迟直方图（全部请求的耗时分布）与客户端成功样本交叉验证：
    // 两套独立统计的 P50 应在同一量级
    const RpcLatencyStats ls = server.latency_stats();
    std::cout << "rpc 服务端延迟 : n=" << ls.sample_count
              << "  P50=" << fmt_num(ls.p50_ms, 1)
              << "ms  P99=" << fmt_num(ls.p99_ms, 1)
              << "ms  mean=" << fmt_num(ls.mean_ms, 1) << "ms\n";
    std::cout << "rpc 交叉验证   : 客户端 P50="
              << fmt_num(percentile_of(latencies, 500), 1) << "ms vs 服务端 P50="
              << fmt_num(ls.p50_ms, 1) << "ms\n";
    server.stop();

    // ---- 本地传输（Windows 命名管道 / POSIX UDS）：同协议不同管道 ----
    // 过载闸门、统计、直方图与 HTTP 传输完全共用
    const std::string pipe_name = "libmini_demo_rpc.pipe";
    RpcServer local(RpcTransport::LocalPipe, pipe_name);
    local.register_method("add", [](const std::string& params) {
        const JsonValue p = parse_json(params);
        return to_json_string(
            JsonValue{{"sum", p["a"].get<int>() + p["b"].get<int>()}});
    });
    local.register_method("slow", [](const std::string&) {
        sleep_for_ms(400);
        return to_json_string(JsonValue{{"done", true}});
    });
    local.set_max_in_flight(1);  // 触发一次过载，演示 429 语义跨传输一致
    local.start_background();
    if (local.wait_until_ready(5000)) {
        std::cout << "rpc 本地传输   : 端点 " << local.endpoint() << "\n";

        RpcClient pipe_client(RpcTransport::LocalPipe, local.endpoint());
        std::cout << "rpc 管道调用   : "
                  << pipe_client.call("add", R"({"a":40,"b":2})") << "\n";

        // 占住槽位后第二个请求被立即拒绝（RejectImmediate 模式）
        RpcClient busy_client(RpcTransport::LocalPipe, local.endpoint());
        busy_client.set_timeout_ms(3000);
        auto hold = std::async(std::launch::async, [&busy_client]() {
            return busy_client.call("slow", "null");
        });
        for (int i = 0; i < 100 && local.stats().active_requests < 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        RpcClient rejected(RpcTransport::LocalPipe, local.endpoint());
        rejected.set_max_retries(0);
        const std::string r = rejected.call("add", R"({"a":1,"b":1})");
        std::cout << "rpc 管道过载   : "
                  << (r.empty() && rejected.last_error() == RpcError::OVERLOADED
                          ? "429 语义与 HTTP 一致"
                          : "（意外：未被拒绝）")
                  << "\n";
        hold.wait();
        const RpcServerStats lst = local.stats();
        std::cout << "rpc 本地统计   : 完成 " << lst.completed_total
                  << "，拒绝 " << lst.rejected_total << "\n";
    } else {
        std::cout << "rpc 本地传输   : 管道启动失败（跳过）\n";
    }
    local.stop();

    // ---- Tcp 传输（帧协议 over 裸 TCP）：跨机内网场景，比 HTTP 更轻 ----
    RpcServer tcp_srv(RpcTransport::Tcp, "127.0.0.1:0");
    tcp_srv.register_method("add", [](const std::string& params) {
        const JsonValue p = parse_json(params);
        return to_json_string(
            JsonValue{{"sum", p["a"].get<int>() + p["b"].get<int>()}});
    });
    tcp_srv.start_background();
    if (tcp_srv.wait_until_ready(5000)) {
        std::cout << "rpc Tcp 传输   : 端点 " << tcp_srv.endpoint() << "\n";

        RpcClient tcp_client(RpcTransport::Tcp, tcp_srv.endpoint());
        std::cout << "rpc Tcp 调用   : "
                  << tcp_client.call("add", R"({"a":100,"b":23})") << "\n";
        std::cout << "rpc Tcp 统计   : 完成 "
                  << tcp_srv.stats().completed_total << "\n";

        // ---- 连接池压测对比（Tcp 传输）：池关 vs 池开 ----
        // handler 无模拟耗时：让「连接建立/拆除」成为单调用主要开销，
        // 差异全部来自每次调用是否重连
        const int kBenchCalls = 300;
        auto bench_qps = [&](bool use_pool) -> std::pair<double, RpcClientPoolStats> {
            RpcClient c(RpcTransport::Tcp, tcp_srv.endpoint());
            c.set_max_retries(0);
            c.set_timeout_ms(5000);
            if (!use_pool) {
                c.set_connection_pool_max(0);  // 旧行为：一调用一连接
            }
            Stopwatch w;
            for (int i = 0; i < kBenchCalls; ++i) {
                (void)c.call("add", R"({"a":1,"b":1})");
            }
            return {kBenchCalls / (w.elapsed_ms() / 1000.0), c.pool_stats()};
        };
        const std::pair<double, RpcClientPoolStats> off = bench_qps(false);
        const std::pair<double, RpcClientPoolStats> on = bench_qps(true);
        std::cout << "rpc 池压测     : Tcp " << kBenchCalls
                  << " 次顺序调用（无处理耗时）\n";
        std::cout << "rpc 池关 QPS   : " << fmt_num(off.first, 0)
                  << "（每调用新建连接，共 " << kBenchCalls << " 次）\n";
        std::cout << "rpc 池开 QPS   : " << fmt_num(on.first, 0)
                  << "（新建 " << on.second.created_total << "，复用 "
                  << on.second.reused_total << "）→ 提升 "
                  << fmt_num(off.first > 0 ? on.first / off.first : 0.0, 1)
                  << "x\n";
    } else {
        std::cout << "rpc Tcp 传输   : 启动失败（跳过）\n";
    }
    tcp_srv.stop();

    // ---- 传输对比：HTTP vs Tcp ----
    // 专用服务器：handler 无模拟耗时、无过载限制，差异全部来自传输本身；
    // 两种传输同一并发形状（8 线程 × N 次，每线程一个客户端：HTTP 走
    // httplib keep-alive，Tcp 走连接池），回环地址消除网络差异
    {
        const auto plain_add = [](const std::string& params) {
            const JsonValue p = parse_json(params);
            return to_json_string(
                JsonValue{{"sum", p["a"].get<int>() + p["b"].get<int>()}});
        };
        // 模拟真实调用（DB/IO）的处理耗时：这种场景下并发上限 = 连接数
        //（池模式）或在途上限（流水线模式），是流水线的主战场
        const auto slow_add = [](const std::string& params) {
            const JsonValue p = parse_json(params);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return to_json_string(
                JsonValue{{"sum", p["a"].get<int>() + p["b"].get<int>()}});
        };
        // 三种传输各起一台基准服务器：handler 无模拟耗时、无过载限制，
        // 差异全部来自传输本身
        RpcServer http_bench(0);
        http_bench.register_method("add", plain_add);
        http_bench.start_background();
        RpcServer tcp_bench(RpcTransport::Tcp, "127.0.0.1:0");
        tcp_bench.register_method("add", plain_add);
        tcp_bench.register_method("slow_add", slow_add);
        tcp_bench.start_background();
#ifdef _WIN32
        const std::string pipe_ep = "libmini_demo_bench.pipe";
#else
        const std::string pipe_ep =
            temp_directory_path() + "/libmini_demo_bench.sock";
#endif
        RpcServer pipe_bench(RpcTransport::LocalPipe, pipe_ep);
        pipe_bench.register_method("add", plain_add);
        pipe_bench.start_background();

        if (http_bench.wait_until_ready(5000) &&
            tcp_bench.wait_until_ready(5000) &&
            pipe_bench.wait_until_ready(5000)) {
            const int k_threads = 8;
            const int k_calls = 200;  // 每线程请求数，总 1600/传输

            struct BenchResult
            {
                double qps = 0;
                double p50 = 0;
                double p99 = 0;
                int failed = 0;
            };
            // 同一并发形状压一种传输：8 线程 × k_calls，每线程一个客户端
            //（HTTP 走 httplib keep-alive，Tcp/管道走连接池）
            auto bench_transport = [&](RpcTransport transport,
                                       const std::string& endpoint,
                                       int port) -> BenchResult {
                std::vector<double> lat;
                std::mutex lat_mu;
                std::atomic<int> failed{0};
                Stopwatch wall;
                std::vector<std::thread> workers;
                for (int t = 0; t < k_threads; ++t) {
                    (void)t;
                    workers.push_back(std::thread([&] {
                        std::unique_ptr<RpcClient> client;
                        if (transport == RpcTransport::Http) {
                            client.reset(new RpcClient(
                                "127.0.0.1", port));
                        } else {
                            client.reset(
                                new RpcClient(transport, endpoint));
                        }
                        client->set_max_retries(0);
                        client->set_timeout_ms(5000);
                        for (int i = 0; i < k_calls; ++i) {
                            Stopwatch sw;
                            const std::string r =
                                client->call("add", R"({"a":1,"b":2})");
                            const double ms =
                                static_cast<double>(sw.elapsed_ms());
                            if (!r.empty()) {
                                std::lock_guard<std::mutex> lock(lat_mu);
                                lat.push_back(ms);
                            } else {
                                failed.fetch_add(1);
                            }
                        }
                    }));
                }
                for (size_t i = 0; i < workers.size(); ++i) {
                    workers[i].join();
                }
                std::sort(lat.begin(), lat.end());
                BenchResult r;
                r.qps =
                    static_cast<double>(k_threads * k_calls) /
                    (wall.elapsed_ms() / 1000.0);
                r.p50 = percentile_of(lat, 500);
                r.p99 = percentile_of(lat, 990);
                r.failed = failed.load();
                return r;
            };

            std::cout << "rpc 传输对比   : HTTP vs Tcp vs 管道，" << k_threads
                      << " 线程 x " << k_calls
                      << " 次（本机回环，handler 无耗时）\n";
            const BenchResult http_r = bench_transport(
                RpcTransport::Http, std::string(), http_bench.port());
            const BenchResult tcp_r = bench_transport(
                RpcTransport::Tcp, tcp_bench.endpoint(), 0);
            const BenchResult pipe_r = bench_transport(
                RpcTransport::LocalPipe, pipe_bench.endpoint(), 0);

            // 按吞吐降序同表输出，冠军列标 ★（噪声容限 15%）
            struct Row
            {
                const char* label;
                const BenchResult* r;
            };
            std::vector<Row> rows = {
                {"HTTP", &http_r}, {"Tcp", &tcp_r}, {"管道", &pipe_r}};
            std::sort(rows.begin(), rows.end(),
                      [](const Row& a, const Row& b) {
                          return a.r->qps > b.r->qps;
                      });
            const double best = rows[0].r->qps;
            for (const Row& row : rows) {
                const bool champion =
                    row.r->qps >= best * 0.85 &&
                    row.r->qps <= best * 1.15 && row.r == rows[0].r;
                std::cout << "rpc " << row.label << "          : QPS="
                          << fmt_num(row.r->qps, 0)
                          << (champion ? " ★" : "  ")
                          << "  P50=" << fmt_num(row.r->p50, 3)
                          << "ms  P99=" << fmt_num(row.r->p99, 3) << "ms";
                if (row.r->failed != 0) {
                    std::cout << "（失败 " << row.r->failed << "）";
                }
                std::cout << "\n";
            }

            // 结论由数据生成：只有全部成功才输出选型建议
            if (http_r.failed == 0 && tcp_r.failed == 0 &&
                pipe_r.failed == 0 && best > 0) {
                // 量级差（≥2x）才认定本质差异，否则视为同档
                const char* fastest = rows[0].label;
                const char* slowest = rows[2].label;
                const double spread =
                    rows[2].r->qps > 0 ? best / rows[2].r->qps : 0.0;
                if (spread >= 2.0) {
                    std::cout << "rpc IPC 结论    : " << fastest
                              << " 吞吐为 " << slowest << " 的 "
                              << fmt_num(spread, 1)
                              << "x——本机高频 RPC 优先选 " << fastest
                              << "，跨机互通再考虑 HTTP；三传输 API 完全"
                                 "一致，切换只改构造函数\n";
                } else {
                    std::cout << "rpc IPC 结论    : 三传输同档（最大/最小 = "
                              << fmt_num(spread, 2)
                              << "）——本机场景按安全/部署约束选型：管道/UDS"
                                 "不占端口且受文件系统 ACL 保护，Tcp 跨机通用"
                                 "，HTTP 兼容生态\n";
                }
            }

            // ---- 流水线开关对比：同一 Tcp 服务器，连接池 vs 单连接多在途
            // 基线 = 每线程独立客户端（连接池，最多 8 连接）；流水线 =
            // 8 线程共享一个客户端（单连接，在途上限 64），展示「一条
            // 连接承载全部并发」
            {
                // 32 线程共享一个客户端：池模式并发上限 = 连接数（默认 8），
                // 流水线模式 = 在途上限（64）——线程数远超连接数时才看得出
                // 两种并发模型的真实差距
                const int k_pipe_threads = 32;
                const int k_pipe_calls = 100;  // 每线程请求数，总 3200
                std::cout << "rpc 流水线      : Tcp handler 耗时 5ms，"
                          << k_pipe_threads << " 线程共享 1 客户端 x "
                          << k_pipe_calls
                          << " 次（池并发=连接数8，流水线并发=在途上限）\n";
                auto run_shape = [&](bool pipeline_on) -> BenchResult {
                    std::vector<double> lat;
                    std::mutex lat_mu;
                    std::atomic<int> failed{0};
                    Stopwatch wall;
                    std::unique_ptr<RpcClient> shared(
                        new RpcClient(RpcTransport::Tcp,
                                      tcp_bench.endpoint()));
                    shared->set_max_retries(0);
                    shared->set_timeout_ms(5000);
                    if (pipeline_on) {
                        shared->set_pipeline_max_in_flight(64);
                    }
                    std::vector<std::thread> workers;
                    for (int t = 0; t < k_pipe_threads; ++t) {
                        (void)t;
                        workers.push_back(std::thread([&] {
                            for (int i = 0; i < k_pipe_calls; ++i) {
                                Stopwatch sw;
                                const std::string r = shared->call(
                                    "slow_add", R"({"a":1,"b":2})");
                                const double ms =
                                    static_cast<double>(sw.elapsed_ms());
                                if (!r.empty()) {
                                    std::lock_guard<std::mutex> lock(lat_mu);
                                    lat.push_back(ms);
                                } else {
                                    failed.fetch_add(1);
                                }
                            }
                        }));
                    }
                    for (std::size_t i = 0; i < workers.size(); ++i) {
                        workers[i].join();
                    }
                    std::sort(lat.begin(), lat.end());
                    BenchResult r;
                    r.qps =
                        static_cast<double>(k_pipe_threads * k_pipe_calls) /
                        (wall.elapsed_ms() / 1000.0);
                    r.p50 = percentile_of(lat, 500);
                    r.p99 = percentile_of(lat, 990);
                    r.failed = failed.load();
                    return r;
                };
                const BenchResult pool_r = run_shape(false);
                const BenchResult pipe_on_r = run_shape(true);
                std::cout << "rpc 流水线关    : QPS="
                          << fmt_num(pool_r.qps, 0)
                          << "  P50=" << fmt_num(pool_r.p50, 3)
                          << "ms  P99=" << fmt_num(pool_r.p99, 3)
                          << "ms（每并发一条池连接）\n";
                std::cout << "rpc 流水线开    : QPS="
                          << fmt_num(pipe_on_r.qps, 0)
                          << "  P50=" << fmt_num(pipe_on_r.p50, 3)
                          << "ms  P99=" << fmt_num(pipe_on_r.p99, 3)
                          << "ms（单连接，在途上限 64）";
                if (pipe_on_r.failed != 0 || pool_r.failed != 0) {
                    std::cout << "（失败 pipe=" << pipe_on_r.failed
                              << " pool=" << pool_r.failed << "）";
                }
                std::cout << "\n";
                if (pipe_on_r.failed == 0 && pool_r.failed == 0 &&
                    pool_r.qps > 0) {
                    const double speedup = pipe_on_r.qps / pool_r.qps;
                    if (speedup >= 1.3) {
                        std::cout << "rpc 流水线结论  : 开启后吞吐提升 "
                                  << fmt_num(speedup, 1)
                                  << "x——handler 有耗时场景下并发上限从"
                                     "连接数变为在途上限，高频调用建议开启\n";
                    } else {
                        std::cout << "rpc 流水线结论  : 提升有限（"
                                  << fmt_num(speedup, 2)
                                  << "x）——本场景连接池已够用，短连接成本"
                                     "敏感时再考虑开启\n";
                    }
                }
            }

            // ---- call_async vs 同步多线程：同请求总量的三种并发形态对比
            // 同步 = 8 个业务线程各自阻塞 call()（线程数 = 并发上限，
            // 每线程压满一条池连接）；异步 = 业务线程只做提交，并发由
            // 客户端内部执行器（8 线程）承担。回调/future 两种异步形态
            // 再对比提交开销：回调直接投任务，future 多一次 promise 堆
            // 分配与收口成本。handler 有耗时 5ms：各形态服务端侧并发
            // 相当，差异在业务线程占用——异步把「等回包」的线程开销
            // 转移进库，业务线程 8:1。
            {
                const int k_th = 8;                // 同步形态的线程数
                const int k_per = 200;             // 每线程请求数
                const int k_total = k_th * k_per;  // 两形态同总量 1600
                std::cout << "rpc 异步对比   : Tcp handler 耗时 5ms，同步 "
                          << k_th << " 线程 vs 回调/future 各 1 个提交线程，"
                          << "各 " << k_total << " 请求\n";

                struct AsyncBench
                {
                    double qps = 0;
                    double p50 = 0;
                    double p99 = 0;
                    int ok = 0;
                    int failed = 0;
                    double submit_ms = 0;  // 提交阶段耗时（两种异步形态）
                    double collect_ms = 0;  // future 收口耗时（仅 future 形态）
                };
                enum class Form
                {
                    Sync,
                    Callback,
                    Future
                };
                auto run_bench = [&](Form form) -> AsyncBench {
                    std::vector<double> lat;
                    lat.reserve(static_cast<std::size_t>(k_total));
                    std::mutex lat_mu;
                    std::atomic<int> ok{0};
                    std::atomic<int> failed{0};
                    int done_count = 0;          // （异步形态）完成计数
                    std::mutex done_mu;
                    std::condition_variable done_cv;
                    double submit_ms = 0;
                    double collect_ms = 0;  // future 收口总耗时（仅 future 形态）
                    Stopwatch wall;

                    // 结果记账（同步与异步共用；ms<0 表示失败）
                    auto record = [&](double ms) {
                        if (ms >= 0) {
                            std::lock_guard<std::mutex> lk(lat_mu);
                            lat.push_back(ms);
                            ok.fetch_add(1);
                        } else {
                            failed.fetch_add(1);
                        }
                        if (form != Form::Sync) {
                            std::lock_guard<std::mutex> lk(done_mu);
                            if (++done_count == k_total) {
                                done_cv.notify_one();
                            }
                        }
                    };

                    if (form == Form::Sync) {
                        // 同步基线：k_th 个业务线程各自阻塞调用
                        std::vector<std::thread> workers;
                        workers.reserve(static_cast<std::size_t>(k_th));
                        for (int t = 0; t < k_th; ++t) {
                            (void)t;
                            workers.emplace_back([&] {
                                RpcClient c(RpcTransport::Tcp,
                                            tcp_bench.endpoint());
                                c.set_max_retries(0);
                                c.set_timeout_ms(5000);
                                for (int i = 0; i < k_per; ++i) {
                                    Stopwatch sw;
                                    const std::string r = c.call(
                                        "slow_add", R"({"a":1,"b":2})");
                                    record(r.empty() ? -1.0
                                                     : sw.elapsed_ms());
                                }
                            });
                        }
                        for (std::thread& w : workers) {
                            w.join();
                        }
                    } else if (form == Form::Callback) {
                        // 异步形态（回调）：1 个业务线程提交 call_async，回调收结果。
                        // 延迟口径 = 提交到回调完成，与同步 call 的阻塞窗
                        // 一致（每调用独立 Stopwatch 捕入回调）
                        RpcClient c(RpcTransport::Tcp, tcp_bench.endpoint());
                        c.set_max_retries(0);
                        c.set_timeout_ms(5000);
                        Stopwatch submit_sw;  // 提交循环耗时（提交开销口径）
                        for (int i = 0; i < k_total; ++i) {
                            Stopwatch sw;
                            c.call_async(
                                "slow_add", R"({"a":1,"b":2})",
                                [&record, sw](const std::string& r, RpcError,
                                              const std::string&) {
                                    record(r.empty() ? -1.0 : sw.elapsed_ms());
                                });
                        }
                        submit_ms =
                            static_cast<double>(submit_sw.elapsed_ms());
                        Stopwatch wait_sw;  // 等待窗与 future 的收口等价
                        std::unique_lock<std::mutex> lk(done_mu);
                        done_cv.wait_for(lk, std::chrono::seconds(30),
                                         [&] { return done_count == k_total; });
                        collect_ms =
                            static_cast<double>(wait_sw.elapsed_ms());
                    } else {
                        // 异步形态（future）：1 个提交线程批量 call_async，
                        // 逐 future 收口。延迟口径 = 提交到该 future 就绪
                        //（与回调一致）；提交开销 = 批量提交循环耗时
                        //（promise 堆分配 + 共享状态）；收口窗 = 完成等待
                        // + 逐 future 检查（与回调的 CV 等待窗等价）
                        RpcClient c(RpcTransport::Tcp, tcp_bench.endpoint());
                        c.set_max_retries(0);
                        c.set_timeout_ms(5000);
                        struct Pending
                        {
                            Stopwatch sw;
                            std::future<std::string> f;
                        };
                        std::vector<Pending> pending;
                        pending.reserve(static_cast<std::size_t>(k_total));
                        Stopwatch submit_sw;
                        for (int i = 0; i < k_total; ++i) {
                            Pending p;
                            p.f = c.call_async("slow_add",
                                               R"({"a":1,"b":2})");
                            pending.push_back(std::move(p));
                        }
                        submit_ms =
                            static_cast<double>(submit_sw.elapsed_ms());
                        Stopwatch collect_sw;
                        while (!pending.empty()) {
                            for (std::size_t i = 0; i < pending.size();) {
                                if (pending[i].f.wait_for(
                                        std::chrono::milliseconds(0))
                                    == std::future_status::ready) {
                                    const std::string r =
                                        pending[i].f.get();
                                    record(r.empty()
                                               ? -1.0
                                               : pending[i].sw.elapsed_ms());
                                    pending[i] = std::move(pending.back());
                                    pending.pop_back();
                                } else {
                                    ++i;
                                }
                            }
                            if (!pending.empty()) {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(1));
                            }
                        }
                        collect_ms =
                            static_cast<double>(collect_sw.elapsed_ms());
                    }

                    const double wall_ms = wall.elapsed_ms();
                    std::sort(lat.begin(), lat.end());
                    AsyncBench r;
                    r.qps = static_cast<double>(ok.load() + failed.load()) /
                            (wall_ms / 1000.0);
                    r.p50 = percentile_of(lat, 500);
                    r.p99 = percentile_of(lat, 990);
                    r.ok = ok.load();
                    r.failed = failed.load();
                    r.submit_ms = submit_ms;
                    r.collect_ms = collect_ms;
                    return r;
                };
                const AsyncBench sync_r = run_bench(Form::Sync);
                const AsyncBench cb_r = run_bench(Form::Callback);
                const AsyncBench fut_r = run_bench(Form::Future);
                auto fail_tag = [](const AsyncBench& r) {
                    return r.failed == 0
                               ? std::string()
                               : "（失败 " + std::to_string(r.failed) + "）";
                };
                std::cout << "rpc 同步形态    : QPS=" << fmt_num(sync_r.qps, 0)
                          << "  P50=" << fmt_num(sync_r.p50, 2)
                          << "ms  P99=" << fmt_num(sync_r.p99, 2)
                          << "ms（8 个业务线程阻塞等回包）" << fail_tag(sync_r)
                          << "\n";
                std::cout << "rpc 异步回调    : QPS=" << fmt_num(cb_r.qps, 0)
                          << "  P50=" << fmt_num(cb_r.p50, 2)
                          << "ms  P99=" << fmt_num(cb_r.p99, 2)
                          << "ms（业务线程 1，等回包在库内执行器）"
                          << fail_tag(cb_r) << "\n";
                std::cout << "rpc 异步future : QPS=" << fmt_num(fut_r.qps, 0)
                          << "  P50=" << fmt_num(fut_r.p50, 2)
                          << "ms  P99=" << fmt_num(fut_r.p99, 2)
                          << "ms（业务线程 1，结果需逐 future 收口）"
                          << fail_tag(fut_r) << "\n";
                std::cout << "rpc 提交开销    : 回调 "
                          << fmt_num(cb_r.submit_ms * 1000.0 / k_total, 1)
                          << "µs/次 vs future "
                          << fmt_num(fut_r.submit_ms * 1000.0 / k_total, 1)
                          << "µs/次（promise 堆分配 + 共享状态）；完成"
                             "等待窗 回调 " << fmt_num(cb_r.collect_ms, 1)
                          << "ms vs future "
                          << fmt_num(fut_r.collect_ms, 1)
                          << "ms（后者含逐 future 轮询收口）\n";
                if (sync_r.ok + sync_r.failed == k_total &&
                    cb_r.ok + cb_r.failed == k_total &&
                    fut_r.ok + fut_r.failed == k_total && sync_r.qps > 0) {
                    const double cb_ratio = cb_r.qps / sync_r.qps;
                    const double fut_ratio = fut_r.qps / sync_r.qps;
                    std::cout << "rpc 异步结论    : 吞吐比 回调 "
                              << fmt_num(cb_ratio, 2) << "x / future "
                              << fmt_num(fut_ratio, 2)
                              << "x——同档吞吐下业务线程占用 8:1";
                    if (cb_r.p50 > sync_r.p50 * 3.0) {
                        std::cout << "；异步 P50（"
                                  << fmt_num(cb_r.p50 / 1000.0, 2)
                                  << "s）含执行器排队与槽位等待——提交线程"
                                     "零阻塞的代价是等待不可见，同步 P50 才是"
                                     "单请求真实耗时";
                    }
                    std::cout << "；需要结果对象/集中收口选 future，轻量"
                                 "通知选回调，直白易调试选同步\n";
                }
            }
        } else {
            std::cout << "rpc 传输对比   : 基准服务器启动失败（跳过）\n";
        }
        http_bench.stop();
        tcp_bench.stop();
        pipe_bench.stop();
    }
}

// SQLite 全链路演示：建表 → 事务批量插入 → 行遍历 → blob → 错误处理
LIBMINI_DEMO(sqlite)
{
    const std::string db_path =
        temp_directory_path() + "/libmini_demo_sqlite.db";
    remove_file(db_path);  // 上次运行的残留文件

    SqliteDatabase db(db_path);
    if (!db.is_open()) {
        std::cout << "sqlite 打开    : 失败（" << db.error_message()
                  << "）\n";
        return;
    }
    std::cout << "sqlite 打开    : " << db_path << "\n";

    // ---- 建表 ----
    const bool created = db.exec(
        "CREATE TABLE files ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  size INTEGER,"
        "  ratio REAL,"
        "  data BLOB)");
    std::cout << "sqlite 建表    : " << (created ? "成功" : "失败") << "\n";

    // ---- 事务批量插入（100 行，部分含 blob；不当提交则全部回滚）----
    const int kRows = 100;
    std::string blob_sample;
    for (int i = 0; i < 64; ++i) {
        blob_sample.push_back(static_cast<char>(i * 7 + 1));  // 含 \0 的二进制
    }
    {
        SqliteTransaction tx(db);
        SqliteStatement ins(db,
            "INSERT INTO files(name, size, ratio, data) VALUES(?, ?, ?, ?)");
        for (int i = 0; i < kRows; ++i) {
            ins.bind_text(1, "file_" + std::to_string(i) + ".bin");
            ins.bind_int64(2, 1024 * (i + 1));
            ins.bind_double(3, 1.0 + i * 0.01);
            if (i % 10 == 0) {
                ins.bind_blob(4, blob_sample);
            } else {
                ins.bind_null(4);
            }
            ins.step();  // bind 自动 reset，循环内直接换参重跑
        }
        tx.commit();
    }
    std::cout << "sqlite 事务    : " << kRows << " 行批量插入，last_rowid="
              << db.last_insert_rowid() << "\n";

    // ---- 行遍历 + 带类型列读取 ----
    SqliteStatement sel(db,
        "SELECT id, name, size, ratio, data FROM files "
        "WHERE size >= ? ORDER BY id");
    sel.bind_int64(1, 1024 * 90);  // i >= 89，共 11 行
    int rows = 0;
    int blob_hits = 0;
    bool blob_ok = true;
    while (sel.step() == SqliteStatement::StepRow) {
        ++rows;
        if (!sel.column_is_null(4)) {
            ++blob_hits;
            if (sel.column_blob(4) != blob_sample) {
                blob_ok = false;  // 二进制往返必须逐字节一致
            }
        }
        if (rows == 1) {
            std::cout << "sqlite 首行    : id=" << sel.column_int(0)
                      << " name=" << sel.column_text(1)
                      << " size=" << sel.column_int64(2)
                      << " ratio=" << sel.column_double(3)
                      << " blob="
                      << (sel.column_is_null(4) ? "NULL"
                                                : std::to_string(
                                                      sel.column_blob(4)
                                                          .size())
                                                      + "B")
                      << "\n";
        }
    }
    std::cout << "sqlite 遍历    : " << rows << " 行（size>=90KB），blob 命中 "
              << blob_hits << "，逐字节一致=" << (blob_ok ? "是" : "否")
              << "\n";

    // ---- query_all：小结果集一次性读取（类型保留）----
    SqliteStatement all(db, "SELECT COUNT(*), SUM(size) FROM files");
    const std::vector<std::vector<SqliteValue>> summary = all.query_all();
    if (!summary.empty() && summary[0].size() == 2) {
        std::cout << "sqlite 汇总    : rows="
                  << summary[0][0].to_int64()
                  << " sum(size)=" << summary[0][1].to_int64() << "\n";
    }

    // ---- 错误处理：约束冲突与坏 SQL 都不抛异常，状态可查且连接可复用 ----
    {
        SqliteStatement dup(db, "INSERT INTO files(name) VALUES(?)");
        dup.bind_null(1);  // name 带 NOT NULL 约束
        std::cout << "sqlite NOT NULL: 拒绝="
                  << (dup.step() == SqliteStatement::StepError ? "是"
                                                               : "否")
                  << "（" << db.error_message() << "）\n";
        dup.bind_text(1, "after_error.bin");  // 出错后 bind 自动 reset，可重试
        std::cout << "sqlite 错误恢复: 重试插入="
              << (dup.step() == SqliteStatement::StepDone ? "成功" : "失败")
              << "\n";

        SqliteStatement bad(db, "SELEKT * FORM files");
        std::cout << "sqlite 坏 SQL  : prepare 拦截="
                  << (bad.is_prepared() ? "否" : "是") << "\n";
    }

    // ---- 事务回滚演示：未提交的行不存在 ----
    {
        SqliteTransaction tx(db);
        SqliteStatement ins(db, "INSERT INTO files(name) VALUES('ghost')");
        ins.step();
        // 不调用 tx.commit()：离开作用域自动回滚
    }
    SqliteStatement cnt(db, "SELECT COUNT(*) FROM files WHERE name='ghost'");
    cnt.step();
    std::cout << "sqlite 回滚    : 未提交行 ghost="
              << (cnt.column_int(0) == 0 ? "已回滚" : "存在（异常！）")
              << "\n";

    remove_file(db_path);
}

// 整合演示：系统信息 + 格式化 + 原子写 + 文件摘要 + HTTP 客户端 + 日志门面
// + 阻塞队列/门闩（一节串起本轮新模块的典型配合用法）
LIBMINI_DEMO(sysinfo_http_log)
{
    // ---- 系统信息 + 人类可读格式化 ----
    std::cout << "hostname       : " << hostname() << "\n";
    std::cout << "pid / cpu      : " << current_pid() << " / " << cpu_count()
              << " 核\n";
    std::cout << "内存 总量/可用 : "
              << format_bytes(static_cast<std::int64_t>(total_physical_memory()))
              << " / "
              << format_bytes(static_cast<std::int64_t>(available_physical_memory()))
              << "\n";
    std::cout << "磁盘 可用      : "
              << format_bytes(static_cast<std::int64_t>(disk_free_bytes(".")))
              << "\n";

    // ---- 原子写配置 + 流式摘要（崩溃安全落盘 + 完整性校验的常见组合）----
    const std::string cfg = "demo_config.json";
    write_file_atomic(cfg, "{\"mode\":\"fast\",\"retries\":3}");
    std::cout << "原子写         : " << cfg << " sha256="
              << sha256_file_hex(cfg).substr(0, 16) << "...\n";

    // ---- 日志门面：控制台 + 滚动文件，级别过滤 ----
    LogFacade::Options o;
    o.file_path = "logs_demo/facade.log";   // 自动建目录，10MB×5 滚动
    o.console = false;
    o.level = LogLevel::Debug;
    if (LogFacade::init(o)) {
        LogFacade::logger()->info("demo 启动 host={} pid={}", hostname(),
                                  current_pid());
        LogFacade::logger()->debug("debug 行可见（级别=Debug）");
        LogFacade::set_level(LogLevel::Warn);
        LogFacade::logger()->info("调级后这行不输出");
        LogFacade::logger()->warn("warn 行输出");
        LogFacade::shutdown();
        std::cout << "日志门面       : logs_demo/facade.log 写入+调级+关闭 ✓\n";
    } else {
        std::cout << "日志门面       : 初始化失败\n";
    }

    // ---- HTTP 客户端：请求一个公共回显端点（失败不致命，演示 API 用法）----
    HttpClient hc("127.0.0.1", 9);   // 死端口：演示传输层错误形态
    hc.set_timeout_ms(300);
    const HttpResponse dead = hc.get("/");
    std::cout << "HTTP 错误形态  : status=" << dead.status
              << " error=\"" << dead.error << "\"\n";

    // ---- BlockingQueue + CountdownLatch：流水线生产消费 ----
    BlockingQueue<int> queue(16);
    CountdownLatch done_latch(2);
    std::atomic<long long> stage_sum{0};
    std::thread consumer([&] {
        int v = 0;
        while (queue.pop(v)) {
            stage_sum += v;
            sleep_for_ms(1);   // 模拟消费耗时
        }
        done_latch.count_down();
    });
    std::thread producer([&] {
        for (int i = 1; i <= 50; ++i) {
            queue.push(i);
        }
        queue.close();
        done_latch.count_down();
    });
    done_latch.wait();
    producer.join();
    consumer.join();
    std::cout << "队列+门闩      : 生产 1..50 消费求和=" << stage_sum.load()
              << "（期望 1275）\n";

    remove_file(cfg);
    remove_tree("logs_demo");
}

}  // namespace

// ------------------------- main -------------------------

int main(int argc, char* argv[])
{
    Args args("demo_libmini", "1.0", "libmini 各模块功能演示");
    args.add_option("only", "", "只运行指定节，逗号分隔", std::string());
    args.add_flag("list", "", "列出全部演示节后退出");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 1;
    }

    if (args.has_flag("list")) {
        std::cout << "可用演示节：\n";
        for (size_t i = 0; i < sections().size(); ++i) {
            std::cout << "  " << sections()[i].name << "\n";
        }
        return 0;
    }

    std::vector<std::string> only;
    const std::string only_arg = args.get_string("only");
    if (!only_arg.empty()) {
        only = split_string(only_arg, ",");
    }

    int ran = 0;
    Stopwatch total;
    for (size_t i = 0; i < sections().size(); ++i) {
        const DemoSection& s = sections()[i];
        bool selected = only.empty();
        for (size_t k = 0; k < only.size(); ++k) {
            if (iequals(only[k], s.name)) {
                selected = true;
                break;
            }
        }
        if (!selected) {
            continue;
        }

        std::cout << "\n=== [" << (i + 1) << "/" << sections().size() << "] "
                  << s.name << " ===\n";
        Stopwatch sw;
        s.run();
        std::cout << "    (" << sw.elapsed_string() << ")\n";
        ++ran;
    }

    std::cout << "\n完成：运行 " << ran << "/" << sections().size()
              << " 节，总耗时 " << total.elapsed_string() << "\n";
    return 0;
}
