// 第二批常用模块（optional/random/env/digest/ini/gzip）的单元测试
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "libmini.h"

// ------------------------------- optional --------------------------------

TEST(OptionalTest, BasicSemantics)
{
    using namespace libmini;
    optional<int> a;
    EXPECT_FALSE(a.has_value());
    EXPECT_FALSE(static_cast<bool>(a));
    EXPECT_EQ(a.value_or(7), 7);

    optional<int> b(42);
    EXPECT_TRUE(b.has_value());
    EXPECT_EQ(*b, 42);
    EXPECT_EQ(b.value(), 42);
    EXPECT_EQ(b.value_or(7), 42);

    b = nullopt;
    EXPECT_FALSE(b.has_value());

    EXPECT_THROW(optional<int>().value(), bad_optional_access);
}

TEST(OptionalTest, CopyMove)
{
    using namespace libmini;
    const std::string kLong = "a long enough string to avoid SSO";

    optional<std::string> src(kLong);
    optional<std::string> copied(src);
    ASSERT_TRUE(copied.has_value());
    EXPECT_EQ(*copied, kLong);

    optional<std::string> moved(std::move(src));
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(*moved, kLong);

    // 自转移不崩溃（标准要求 no-op；我们的实现有 this 检查）
    optional<std::string> self(moved);
    self = std::move(self);
    EXPECT_EQ(*self, kLong);

    // 拷贝空 optional 保持为空
    optional<int> empty;
    optional<int> copied_empty(empty);
    EXPECT_FALSE(copied_empty.has_value());
}

TEST(OptionalTest, ReassignAndEmplace)
{
    using namespace libmini;
    optional<std::string> s;
    s.emplace(5, 'x');
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "xxxxx");

    s = std::string("y");
    EXPECT_EQ(*s, "y");

    s.reset();
    EXPECT_FALSE(s.has_value());

    // 赋值覆盖旧值（析构旧资源）
    optional<std::string> a(std::string("first value"));
    optional<std::string> b(std::string("second value"));
    a = b;
    EXPECT_EQ(*a, "second value");
    EXPECT_EQ(*b, "second value");
}

TEST(OptionalTest, MakeOptionalAndSwap)
{
    using namespace libmini;
    auto m = make_optional(3);
    static_assert(std::is_same<decltype(m), optional<int> >::value,
                  "make_optional deduces optional<int>");
    EXPECT_EQ(*m, 3);

    optional<int> x(1), y(2);
    x.swap(y);
    EXPECT_EQ(*x, 2);
    EXPECT_EQ(*y, 1);
}

// ----------------------------- random_utils ------------------------------

TEST(RandomUtilsTest, IntRange)
{
    using namespace libmini;
    for (int i = 0; i < 200; ++i) {
        const int v = random_int(3, 10);
        EXPECT_GE(v, 3);
        EXPECT_LE(v, 10);
    }
    // 边界相等时恒返回该值
    EXPECT_EQ(random_int(5, 5), 5);
}

TEST(RandomUtilsTest, DoubleRangeAndString)
{
    using namespace libmini;
    for (int i = 0; i < 200; ++i) {
        const double v = random_double(1.5, 2.5);
        EXPECT_GE(v, 1.5);
        EXPECT_LT(v, 2.5);
    }
    EXPECT_NEAR(random_double(0.5, 0.5), 0.5, 1e-12);

    const std::string s = random_string(32);
    EXPECT_EQ(s.size(), 32u);
    for (std::size_t i = 0; i < s.size(); ++i) {
        EXPECT_NE(s[i], '\0');
    }
    // 自定义字母表
    const std::string hexs = random_string(8, "0123456789abcdef");
    for (std::size_t i = 0; i < hexs.size(); ++i) {
        const char c = hexs[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
    }
    EXPECT_EQ(random_string(10, ""), "");
}

TEST(RandomUtilsTest, Pick)
{
    using namespace libmini;
    const std::vector<int> vals = {10, 20, 30};
    for (int i = 0; i < 50; ++i) {
        const int v = random_pick(vals);
        EXPECT_TRUE(v == 10 || v == 20 || v == 30);
    }
}

// --------------------------------- env -----------------------------------

TEST(EnvTest, GetSetRemove)
{
    using namespace libmini;
    EXPECT_FALSE(env_has("LIBMINI_TEST_VAR_XYZ"));

    EXPECT_TRUE(env_set("LIBMINI_TEST_VAR_XYZ", "hello"));
    EXPECT_TRUE(env_has("LIBMINI_TEST_VAR_XYZ"));
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ"), "hello");
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ", "dft"), "hello");

    EXPECT_TRUE(env_remove("LIBMINI_TEST_VAR_XYZ"));
    EXPECT_FALSE(env_has("LIBMINI_TEST_VAR_XYZ"));
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ"), "");
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ", "dft"), "dft");

    // 不存在的变量返回默认值
    EXPECT_EQ(env_get("LIBMINI_NO_SUCH_VAR_42", "fallback"), "fallback");
}

TEST(EnvTest, Expand)
{
    using namespace libmini;
    env_set("LIBMINI_TEST_VAR_AB", "abc");

    EXPECT_EQ(env_expand("x=%LIBMINI_TEST_VAR_AB%!"), "x=abc!");
    EXPECT_EQ(env_expand("%LIBMINI_TEST_VAR_AB%%LIBMINI_TEST_VAR_AB%"), "abcabc");
    EXPECT_EQ(env_expand("no vars here"), "no vars here");
    // 未定义变量原样保留
    EXPECT_EQ(env_expand("%LIBMINI_NO_SUCH_VAR_42%"), "%LIBMINI_NO_SUCH_VAR_42%");
    // 孤立 % 不处理
    EXPECT_EQ(env_expand("50% off"), "50% off");

    env_remove("LIBMINI_TEST_VAR_AB");
}

// -------------------------------- digest ---------------------------------

TEST(Md5Test, KnownVectors)
{
    using namespace libmini;
    EXPECT_EQ(Md5::hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(Md5::hex("a"), "0cc175b9c0f1b6a831c399e269772661");
    EXPECT_EQ(Md5::hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(Md5::hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    EXPECT_EQ(Md5::hex("abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b");
}

TEST(Md5Test, Incremental)
{
    using namespace libmini;
    // 分块喂入与一次性计算结果一致（覆盖 64 字节块边界：1+63=64 正好一块，
    // 后续 70+866 字节再走多块 + 尾部缓冲路径）
    const std::string data(1000, 'x');
    Md5 whole;
    whole.update(data);
    const std::string digest = whole.finish();  // finish 消费状态，只能调一次

    Md5 chunked;
    chunked.update(data.substr(0, 1));
    chunked.update(data.substr(1, 63));
    chunked.update(data.substr(64, 70));
    chunked.update(data.substr(134));

    EXPECT_EQ(digest, chunked.finish());
    EXPECT_EQ(Md5::hex(data), Hex::encode(digest, true));

    // reset 复用
    whole.reset();
    whole.update("abc");
    std::string expect_bytes;
    ASSERT_TRUE(Hex::decode("900150983cd24fb0d6963f7d28e17f72", expect_bytes));
    EXPECT_EQ(whole.finish(), expect_bytes);
}

TEST(Sha256Test, KnownVectors)
{
    using namespace libmini;
    EXPECT_EQ(Sha256::hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(Sha256::hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(Sha256::hex("The quick brown fox jumps over the lazy dog"),
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(Sha256Test, Incremental)
{
    using namespace libmini;
    const std::string data(1000, 'x');
    Sha256 whole;
    whole.update(data);

    Sha256 chunked;
    for (std::size_t i = 0; i < data.size(); i += 37) {
        chunked.update(data.substr(i, 37));
    }
    EXPECT_EQ(whole.finish(), chunked.finish());
}

// ------------------------------- ini_config ------------------------------

TEST(IniConfigTest, ParseAndGet)
{
    using namespace libmini;
    const char* kText =
        "; top comment\n"
        "# another comment\n"
        "global_key = global_value\n"
        "\n"
        "[server]        ; inline comment\n"
        "host = 127.0.0.1\n"
        "port: 8080\n"
        "timeout = 30   # trailing comment\n"
        "verbose = YES\n"
        "title = \"My App\"\n"
        "empty =\n"
        "\n"
        "[paths]\n"
        "data = /var/lib/app\n";

    IniConfig cfg;
    cfg.parse(kText);

    EXPECT_EQ(cfg.get("", "global_key"), "global_value");
    EXPECT_EQ(cfg.get("server", "host"), "127.0.0.1");
    EXPECT_EQ(cfg.get("server", "port"), "8080");  // ':' 分隔符
    EXPECT_EQ(cfg.get_int("server", "port"), 8080);
    EXPECT_EQ(cfg.get_int("server", "timeout"), 30);
    EXPECT_TRUE(cfg.get_bool("server", "verbose"));  // YES
    EXPECT_EQ(cfg.get("server", "title"), "My App"); // 剥引号
    EXPECT_EQ(cfg.get("server", "empty"), "");
    EXPECT_EQ(cfg.get("paths", "data"), "/var/lib/app");

    // 默认值
    EXPECT_EQ(cfg.get("server", "missing", "dft"), "dft");
    EXPECT_EQ(cfg.get_int("server", "missing", -1), -1);
    EXPECT_EQ(cfg.get_double("server", "ratio", 1.5), 1.5);
    EXPECT_TRUE(cfg.get_bool("server", "missing", true));

    EXPECT_TRUE(cfg.has_section("server"));
    EXPECT_FALSE(cfg.has_section("nope"));
    EXPECT_TRUE(cfg.has_key("server", "host"));
    EXPECT_FALSE(cfg.has_key("server", "nope"));

    const std::vector<std::string> ss = cfg.sections();
    ASSERT_EQ(ss.size(), 2u);
    EXPECT_EQ(ss[0], "paths");
    EXPECT_EQ(ss[1], "server");
    const std::vector<std::string> ks = cfg.keys("server");
    ASSERT_EQ(ks.size(), 6u);
}

TEST(IniConfigTest, SetRemoveRoundTrip)
{
    using namespace libmini;
    IniConfig cfg;
    cfg.set("a", "x", "1");
    cfg.set("a", "y", "true");
    cfg.set("", "g", "global");
    cfg.set("b", "z", "2.5");

    EXPECT_EQ(cfg.get_int("a", "x"), 1);
    EXPECT_TRUE(cfg.get_bool("a", "y"));
    EXPECT_EQ(cfg.get_double("b", "z"), 2.5);
    EXPECT_EQ(cfg.get("", "g"), "global");

    // save → parse 往返
    IniConfig cfg2;
    cfg2.parse(cfg.save());
    EXPECT_EQ(cfg2.get_int("a", "x"), 1);
    EXPECT_TRUE(cfg2.get_bool("a", "y"));
    EXPECT_EQ(cfg2.get("", "g"), "global");
    EXPECT_EQ(cfg2.get_double("b", "z"), 2.5);

    cfg.remove("a", "x");
    EXPECT_FALSE(cfg.has_key("a", "x"));
    cfg.remove_section("a");
    EXPECT_FALSE(cfg.has_section("a"));

    cfg.clear();
    EXPECT_TRUE(cfg.sections().empty());
}

// --------------------------------- gzip ----------------------------------

TEST(GzipTest, RoundTrip)
{
    using namespace libmini;
    // 空串、短串、长重复串（考验压缩）、二进制串
    const std::vector<std::string> cases = {
        "",
        "a",
        "hello hello hello hello hello hello hello hello hello",
        std::string(10000, 'z'),
        std::string("binary\x00\x01\xff data", 16)};

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const optional<std::string> compressed = gzip_compress(cases[i]);
        ASSERT_TRUE(compressed.has_value()) << "case " << i;
        const optional<std::string> decompressed = gzip_decompress(*compressed);
        ASSERT_TRUE(decompressed.has_value()) << "case " << i;
        EXPECT_EQ(*decompressed, cases[i]) << "case " << i;
    }
}

TEST(GzipTest, HeaderAndCompressionRatio)
{
    using namespace libmini;
    const std::string payload(5000, 'q');
    const optional<std::string> compressed = gzip_compress(payload, 9);
    ASSERT_TRUE(compressed.has_value());

    // gzip 魔数 1f 8b
    EXPECT_EQ(static_cast<unsigned char>((*compressed)[0]), 0x1F);
    EXPECT_EQ(static_cast<unsigned char>((*compressed)[1]), 0x8B);
    // 高压缩比下输出远小于输入
    EXPECT_LT(compressed->size(), payload.size() / 10);

    const optional<std::string> out = gzip_decompress(*compressed);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, payload);
}

TEST(GzipTest, DecompressRejectsGarbage)
{
    using namespace libmini;
    EXPECT_FALSE(gzip_decompress("").has_value());
    EXPECT_FALSE(gzip_decompress("not gzip at all").has_value());
    EXPECT_FALSE(gzip_decompress(std::string("\x1f\x8b\x08\x00trunc", 9)).has_value());
}

// -------------------------------- async ----------------------------------

TEST(AsyncSchedulerTest, RunAfterExecutesOnce)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    sched.run_after_ms(30, [&calls] { ++calls; });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(calls.load(), 0);   // 未到期不执行

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_EQ(calls.load(), 1);   // 到期执行且只执行一次
    EXPECT_EQ(sched.pending_count(), 0u);
}

TEST(AsyncSchedulerTest, CancelBeforeDue)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    TaskHandle h = sched.run_after_ms(200, [&calls] { ++calls; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    EXPECT_TRUE(h.cancel());      // 尚未到期，取消成功
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(calls.load(), 0);   // 永不执行
    EXPECT_EQ(sched.pending_count(), 0u);

    // 空句柄/重复取消安全
    TaskHandle nil;
    EXPECT_FALSE(nil.cancel());
    EXPECT_FALSE(h.cancel());
}

TEST(AsyncSchedulerTest, PeriodicRunsAndStopsByReturnFalse)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    // 执行 3 次后返回 false 自停
    sched.run_every_ms(20, [&calls]() -> bool {
        return calls.fetch_add(1) + 1 < 3;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(calls.load(), 3);
    EXPECT_EQ(sched.pending_count(), 0u);  // 后续期次已被清理
}

TEST(AsyncSchedulerTest, PeriodicCancelStopsFutureRuns)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    TaskHandle h = sched.run_every_ms(30, [&calls] { return ++calls > 0; });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GE(calls.load(), 1);   // 已至少执行过一次
    EXPECT_TRUE(h.cancel());
    const int at_cancel = calls.load();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(calls.load(), at_cancel);   // 之后不再执行
}

TEST(AsyncSchedulerTest, CancelAllAndDtorStopsCleanly)
{
    using namespace libmini;
    std::atomic<int> calls{0};
    {
        AsyncScheduler sched;
        for (int i = 0; i < 5; ++i) {
            sched.run_after_ms(500, [&calls] { ++calls; });
        }
        EXPECT_EQ(sched.pending_count(), 5u);
        sched.cancel_all();
        EXPECT_EQ(sched.pending_count(), 0u);
        // 析构在任务到期前发生，join 干净退出
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_EQ(calls.load(), 0);
}

TEST(AsyncSchedulerTest, TaskCanScheduleAnotherTask)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> final_calls{0};
    std::atomic<bool> chained{false};
    sched.run_after_ms(20, [&sched, &chained, &final_calls] {
        chained = true;
        sched.run_after_ms(20, [&final_calls] { ++final_calls; });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(chained.load());
    EXPECT_EQ(final_calls.load(), 1);   // 工作线程内提交任务不死锁
}

TEST(RateLimiterTest, BurstThenThrottles)
{
    using namespace libmini;
    // 稳态 20/s，桶 3：前 3 次立即成功，之后开始限流
    RateLimiter limiter(20.0, 3.0);

    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_FALSE(limiter.try_acquire());   // 桶空
    EXPECT_FALSE(limiter.try_acquire());

    // 约 50ms 补 1 个令牌
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_TRUE(limiter.try_acquire());

    // 补的速率有上限
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_LT(limiter.available(), 3.0);   // 未到桶上限
}

TEST(RateLimiterTest, AcquireBlocksUntilTokenAvailable)
{
    using namespace libmini;
    RateLimiter limiter(25.0, 1.0);   // 40ms 一个令牌

    const auto t0 = std::chrono::steady_clock::now();
    const std::int64_t waited = limiter.acquire();   // 桶里 1 个立即拿到
    const std::int64_t waited2 = limiter.acquire();  // 需等约 40ms
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    EXPECT_LE(waited, 5);          // 初始令牌立即可用
    EXPECT_GE(waited2, 25);        // 第二次真实等待
    EXPECT_GE(elapsed, 25);
    EXPECT_LT(elapsed, 500);       // 不会等过头
}

TEST(RateLimiterTest, MultiPermitAcquire)
{
    using namespace libmini;
    RateLimiter limiter(10.0, 5.0);

    EXPECT_TRUE(limiter.try_acquire(5.0));  // 一次取空桶
    EXPECT_FALSE(limiter.try_acquire(1.0));
    EXPECT_FALSE(limiter.try_acquire(2.0));

    // 取超过容量的令牌：需要多个补充周期，最终能拿到
    const std::int64_t waited = limiter.acquire(2.0);
    EXPECT_GE(waited, 100);   // 10/s → 2 个令牌约 200ms
}

// ------------------------------ json_utils --------------------------------

TEST(JsonUtilsTest, ToJsonStringSimpleEscapesSpecialChars)
{
    using namespace libmini;
    // 引号必须转义，否则输出非法 JSON
    EXPECT_EQ(to_json_string_simple("say \"hi\""), "\"say \\\"hi\\\"\"");
    // 反斜杠必须转义
    EXPECT_EQ(to_json_string_simple("a\\b"), "\"a\\\\b\"");
    // 控制字符
    EXPECT_EQ(to_json_string_simple("line1\nline2"), "\"line1\\nline2\"");
    EXPECT_EQ(to_json_string_simple("tab\there"), "\"tab\\there\"");
    // 空串
    EXPECT_EQ(to_json_string_simple(""), "\"\"");
    // 中文原样保留（UTF-8）
    EXPECT_EQ(to_json_string_simple("\xE4\xB8\xAD\xE6\x96\x87"),
              "\"\xE4\xB8\xAD\xE6\x96\x87\"");
}

TEST(JsonUtilsTest, ToJsonStringSimpleProducesValidJson)
{
    using namespace libmini;
    const std::string nasty = "he said \"hi\" \\ done\n\t\x01";
    const std::string encoded = to_json_string_simple(nasty);

    // 输出必须是合法 JSON 字符串字面量，且解析回来等于原串
    JsonValue parsed;
    ASSERT_NO_THROW(parsed = parse_json(encoded));
    ASSERT_TRUE(parsed.is_string());
    EXPECT_EQ(parsed.get<std::string>(), nasty);

    // 旧行为回归基线：普通字符串只包引号
    EXPECT_EQ(to_json_string_simple("test"), "\"test\"");
}

TEST(JsonUtilsTest, EscapeJsonString)
{
    using namespace libmini;
    EXPECT_EQ(escape_json_string("x"), "x");       // 无需转义时原样
    EXPECT_EQ(escape_json_string("a\"b"), "a\\\"b");
    EXPECT_EQ(escape_json_string("a\\b"), "a\\\\b");
    EXPECT_EQ(escape_json_string("n\nm"), "n\\nm");
    EXPECT_EQ(escape_json_string(""), "");

    // 手工拼接 JSON 的推荐用法：动态值经 escape 后拼入模板
    const std::string user_input = "evil \"quote\" and \\ backslash";
    const std::string manual =
        std::string("{\"msg\":\"") + escape_json_string(user_input) + "\"}";
    JsonValue parsed;
    ASSERT_NO_THROW(parsed = parse_json(manual));
    EXPECT_EQ(parsed["msg"].get<std::string>(), user_input);
}

TEST(JsonUtilsTest, ParseJsonSimpleNormalizesAndValidates)
{
    using namespace libmini;
    // 规范化：吃进带空白/换行的 JSON，输出紧凑格式
    EXPECT_EQ(parse_json_simple("{ \"a\" : 1, \"b\" : \"x\" }"),
              "{\"a\":1,\"b\":\"x\"}");
    // 紧凑输入原样输出（旧测试依赖的非空行为不变）
    EXPECT_EQ(parse_json_simple("{\"name\":\"test\",\"value\":123}"),
              "{\"name\":\"test\",\"value\":123}");
    // 数组与标量也支持
    EXPECT_EQ(parse_json_simple("[1, 2, 3]"), "[1,2,3]");
    EXPECT_EQ(parse_json_simple("\"bare\""), "\"bare\"");

    // 非法输入返回空串，不抛异常
    EXPECT_EQ(parse_json_simple("garbage"), "");
    EXPECT_EQ(parse_json_simple(""), "");
    EXPECT_EQ(parse_json_simple("{\"a\":}"), "");
    EXPECT_EQ(parse_json_simple("{\"a\":1,}"), "");
}

// ------------------------------ file_utils --------------------------------

TEST(FileUtilsTest, Utf8ChinesePathRoundTrip)
{
    using namespace libmini;
    const std::string fname = "libmini_测试文件.txt";

    // 写入/存在性/大小/读回 —— 全程 UTF-8 中文路径
    EXPECT_TRUE(write_file(fname, "hello 中文内容"));
    EXPECT_TRUE(file_exists(fname));
    EXPECT_EQ(file_size(fname), 18u);            // 6 + 4×3（UTF-8 汉字）
    EXPECT_EQ(read_file(fname), "hello 中文内容");

    // 二进制内容往返
    const std::string binary("\x00\x01\xff\xfe\xe4\xb8\xad", 7);
    EXPECT_TRUE(write_file(fname, binary));
    EXPECT_EQ(read_file(fname), binary);

    // 目录列表应包含该文件（UTF-8 返回），且不含 "." 与 ".."
    const std::vector<std::string> entries = list_directory(".");
    EXPECT_TRUE(std::find(entries.begin(), entries.end(), fname) != entries.end());
    EXPECT_TRUE(std::find(entries.begin(), entries.end(), ".") == entries.end());
    EXPECT_TRUE(std::find(entries.begin(), entries.end(), "..") == entries.end());

    // 清理
    EXPECT_TRUE(remove_file(fname));
    EXPECT_FALSE(file_exists(fname));

    // 不存在的中文路径
    EXPECT_FALSE(file_exists("不存在_某某_9x7.txt"));
    EXPECT_TRUE(read_file("不存在_某某_9x7.txt").empty());
    EXPECT_EQ(file_size("不存在_某某_9x7.txt"), 0u);
    EXPECT_FALSE(remove_file("不存在_某某_9x7.txt"));
}

TEST(FileUtilsTest, EmptyWriteAndDirectoryListing)
{
    using namespace libmini;
    const std::string fname = "libmini_empty_test.tmp";
    EXPECT_TRUE(write_file(fname, ""));
    EXPECT_TRUE(file_exists(fname));
    EXPECT_EQ(file_size(fname), 0u);
    EXPECT_TRUE(read_file(fname).empty());

    // 覆盖写
    EXPECT_TRUE(write_file(fname, "v2"));
    EXPECT_EQ(read_file(fname), "v2");
    EXPECT_TRUE(remove_file(fname));

    // 当前目录列表非空（至少有测试可执行文件）
    EXPECT_FALSE(list_directory(".").empty());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
