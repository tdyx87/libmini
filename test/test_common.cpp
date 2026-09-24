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

    // 自转移不崩溃（标准要求 no-op；我们的实现有 this 检查）。
    // GCC 13+ 对自转移发 -Wself-move 告警，这里自转移正是测试目标，局部关闭
    optional<std::string> self(moved);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    self = std::move(self);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
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

// ------------------------------- format 辅助 ------------------------------

TEST(FormatHelpersTest, FormatBytes)
{
    using namespace libmini;
    EXPECT_EQ(format_bytes(0), "0 B");
    EXPECT_EQ(format_bytes(512), "512 B");
    EXPECT_EQ(format_bytes(1024), "1 KB");
    EXPECT_EQ(format_bytes(1536), "1.5 KB");
    EXPECT_EQ(format_bytes(1024 * 1024), "1 MB");
    EXPECT_EQ(format_bytes(3 * 1024 * 1024 + 512 * 1024), "3.5 MB");
    EXPECT_EQ(format_bytes((3LL * 1024 * 1024 * 1024) / 2), "1.5 GB");
    EXPECT_EQ(format_bytes(-2048), "-2 KB");
}

TEST(FormatHelpersTest, FormatDurationMs)
{
    using namespace libmini;
    EXPECT_EQ(format_duration_ms(0), "0 ms");
    EXPECT_EQ(format_duration_ms(15), "15 ms");
    EXPECT_EQ(format_duration_ms(2500), "2.5 s");
    EXPECT_EQ(format_duration_ms(65500), "1m 05.5 s");
    EXPECT_EQ(format_duration_ms(3 * 3600000 + 2 * 60000 + 1000),
              "3h 2m 1s");
    EXPECT_EQ(format_duration_ms(-800), "-800 ms");
}

// ------------------------------- 原子写文件 --------------------------------

TEST(AtomicWriteTest, OverwriteAndCrashSafety)
{
    using namespace libmini;
    const std::string fname = "libmini_atomic_test.tmp";
    EXPECT_TRUE(write_file_atomic(fname, "第一版内容 v1"));
    EXPECT_EQ(read_file(fname), "第一版内容 v1");

    // 原子覆盖：内容完整替换
    EXPECT_TRUE(write_file_atomic(fname, "第二版内容 v2"));
    EXPECT_EQ(read_file(fname), "第二版内容 v2");

    // 空内容也合法
    EXPECT_TRUE(write_file_atomic(fname, ""));
    EXPECT_TRUE(read_file(fname).empty());

    EXPECT_TRUE(remove_file(fname));

    // 目标目录不存在时失败，且不留临时文件（先清理历史残留，保证哨兵目录真不存在）
    remove_tree("no_such_dir_atomic_9x7");
    EXPECT_FALSE(write_file_atomic("no_such_dir_atomic_9x7/child.txt", "x"));
    EXPECT_FALSE(file_exists("no_such_dir_atomic_9x7"));
}

TEST(AtomicWriteTest, LargeContent)
{
    using namespace libmini;
    const std::string fname = "libmini_atomic_big.tmp";
    std::string big;
    big.reserve(3 * 1024 * 1024);
    for (int i = 0; i < 3 * 1024; ++i) {
        big += "0123456789abcdef";
    }
    EXPECT_TRUE(write_file_atomic(fname, big));
    EXPECT_EQ(file_size(fname), big.size());
    EXPECT_EQ(read_file(fname), big);
    EXPECT_TRUE(remove_file(fname));
}

// ------------------------------- 文件摘要 ----------------------------------

TEST(FileDigestTest, Sha256AndMd5MatchKnownValues)
{
    using namespace libmini;
    const std::string fname = "libmini_digest_test.tmp";
    const std::string content = "hello 文件摘要";
    EXPECT_TRUE(write_file(fname, content));

    // 与内存版 digest 对齐（同一实现，口径必须一致）
    EXPECT_EQ(sha256_file_hex(fname), Sha256::hex(content));
    EXPECT_EQ(md5_file_hex(fname), Md5::hex(content));

    // 空文件（SHA-256 of empty 的已知值）
    EXPECT_TRUE(write_file(fname, ""));
    EXPECT_EQ(sha256_file_hex(fname),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    EXPECT_TRUE(remove_file(fname));

    // 文件不存在返回空串
    EXPECT_TRUE(sha256_file_hex("no_such_file_digest.bin").empty());
    EXPECT_TRUE(md5_file_hex("no_such_file_digest.bin").empty());
}

TEST(FileDigestTest, LargeFileStreaming)
{
    using namespace libmini;
    const std::string fname = "libmini_digest_big.tmp";
    // 5MB 内容：分块写后，流式摘要必须与内存摘要一致
    std::string chunk("abcdefgh01234567");
    std::string content;
    for (int i = 0; i < 5 * 1024; ++i) content += chunk;
    EXPECT_TRUE(write_file(fname, content));
    EXPECT_EQ(sha256_file_hex(fname), Sha256::hex(content));
    EXPECT_EQ(md5_file_hex(fname), Md5::hex(content));
    EXPECT_TRUE(remove_file(fname));
}

// ------------------------------- system_info -------------------------------

TEST(SystemInfoTest, BasicQueriesAreSane)
{
    using namespace libmini;
    EXPECT_FALSE(hostname().empty());
    EXPECT_NE(current_pid(), 0u);
    EXPECT_FALSE(executable_path().empty());
    EXPECT_TRUE(file_exists(executable_path()));  // 可执行文件路径真实存在
    EXPECT_GE(cpu_count(), 1);

    EXPECT_GT(total_physical_memory(), 0u);
    EXPECT_GT(available_physical_memory(), 0u);
    EXPECT_LE(available_physical_memory(), total_physical_memory());

    EXPECT_GT(disk_total_bytes("."), 0u);
    EXPECT_GT(disk_free_bytes("."), 0u);
    EXPECT_LE(disk_free_bytes("."), disk_total_bytes("."));
    EXPECT_EQ(disk_total_bytes("no_such_dir_atomic_9x7/none"), 0u);
}

TEST(SystemInfoTest, ExecutablePathIsAbsolute)
{
    using namespace libmini;
    const std::string exe = executable_path();
    ASSERT_FALSE(exe.empty());
#ifdef _WIN32
    EXPECT_EQ(exe.find(':'), 1u);            // "X:..." 盘符
    EXPECT_TRUE(exe.find('\\') != std::string::npos);
#else
    EXPECT_EQ(exe[0], '/');
#endif
}

// ------------------------------ BlockingQueue ------------------------------

TEST(BlockingQueueTest, ProducerConsumer)
{
    using namespace libmini;
    BlockingQueue<int> q(4);
    q.push(1);
    q.push(2);
    EXPECT_EQ(q.size(), 2u);

    int v = 0;
    EXPECT_TRUE(q.try_pop(v, 100));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_pop(v, 100));
    EXPECT_EQ(v, 2);

    // 空队列超时失败
    EXPECT_FALSE(q.try_pop(v, 50));

    // 关闭后：存量可取尽，随后 pop 返回 false，push 被丢弃
    q.push(3);
    q.close();
    EXPECT_TRUE(q.try_pop(v, 100));
    EXPECT_EQ(v, 3);
    EXPECT_FALSE(q.pop(v));
    q.push(4);  // 关闭后丢弃，不阻塞
    EXPECT_TRUE(q.empty());
}

TEST(BlockingQueueTest, MultiProducerMultiConsumer)
{
    using namespace libmini;
    BlockingQueue<int> q(8);
    std::atomic<int> sum{0};
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 250;

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p, kPerProducer] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.push(p * kPerProducer + i + 1);
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < 3; ++c) {
        consumers.emplace_back([&q, &sum] {
            int v = 0;
            while (q.pop(v)) {   // 关闭且取空后返回 false 退出
                sum += v;
            }
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    // Σ_p Σ_i (p*N + i + 1) = N²·P(P-1)/2 + P·N(N+1)/2
    const std::int64_t expected =
        static_cast<std::int64_t>(kPerProducer) * kPerProducer *
            (kProducers * (kProducers - 1) / 2) +
        static_cast<std::int64_t>(kProducers) *
            (kPerProducer * (kPerProducer + 1) / 2);
    EXPECT_EQ(sum.load(), expected);
}

// ----------------------------- CountdownLatch ------------------------------

TEST(CountdownLatchTest, ReleasesWaitersAtZero)
{
    using namespace libmini;
    CountdownLatch latch(3);
    EXPECT_EQ(latch.count(), 3u);

    std::atomic<int> released{0};
    std::thread waiter([&] {
        latch.wait();
        ++released;
    });
    std::thread waiter2([&] {
        EXPECT_TRUE(latch.wait_for(5000));
        ++released;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(released.load(), 0);   // 未归零前不放行
    latch.count_down();
    latch.count_down();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(released.load(), 0);
    latch.count_down();              // 归零
    waiter.join();
    waiter2.join();
    EXPECT_EQ(released.load(), 2);
    EXPECT_EQ(latch.count(), 0u);

    latch.count_down();              // 已归零后再减：无效且安全
    EXPECT_EQ(latch.count(), 0u);
}

TEST(CountdownLatchTest, WaitForTimeout)
{
    using namespace libmini;
    CountdownLatch latch(2);
    latch.count_down();
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(latch.wait_for(80));   // 还差一次：超时
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(elapsed, 70);
    latch.count_down();
    EXPECT_TRUE(latch.wait_for(0));     // 已归零：立即成功
}

// ------------------------------ ThreadPool 增 ------------------------------

TEST(ThreadPoolIdleTest, WaitIdleDrainsAllTasks)
{
    using namespace libmini;
    ThreadPool pool(3);
    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++done;
        });
    }
    EXPECT_LE(done.load(), 100);
    pool.wait_idle();
    EXPECT_EQ(done.load(), 100);   // wait_idle 返回时全部任务已完成

    // wait_idle 后线程池仍可用
    auto f = pool.submit([] { return 42; });
    EXPECT_EQ(f.get(), 42);
}

TEST(ThreadPoolIdleTest, PendingTasksCountsQueue)
{
    using namespace libmini;
    ThreadPool pool(1);            // 单线程：后提交的任务必然排队
    std::atomic<bool> release{false};
    pool.submit([&release] {
        while (!release.load()) std::this_thread::yield();
    });
    for (int i = 0; i < 5; ++i) {
        pool.submit([] {});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(pool.pending_tasks(), 1u);   // 任务在队列里（≥1，时序宽裕）
    release = true;
    pool.wait_idle();
    EXPECT_EQ(pool.pending_tasks(), 0u);
}

// ------------------------------- HttpClient --------------------------------

#include <httplib.h>  // 测试内起本地服务，验证 HttpClient 的回环行为
#include <spdlog/spdlog.h>  // LogFacade::logger() 返回指针需完整类型才能调方法

namespace {

// 本地 httplib 服务 fixture：端口 0 自动分配，析构自动停
struct LocalHttpServer
{
    httplib::Server server;
    std::thread thread;
    int port = 0;

    LocalHttpServer()
    {
        server.Get("/hello", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("hello http", "text/plain");
        });
        server.Post("/echo", [](const httplib::Request& req,
                                httplib::Response& res) {
            res.set_content(req.body, "text/plain");
        });
        server.Get("/query", [](const httplib::Request& req,
                                httplib::Response& res) {
            std::string out;
            for (const auto& kv : req.params) {
                if (!out.empty()) out += "&";
                out += kv.first + "=" + kv.second;   // httplib 已解码
            }
            res.set_content(out, "text/plain");
        });
        server.Get("/header", [](const httplib::Request& req,
                                 httplib::Response& res) {
            res.set_content(req.get_header_value("X-Token"), "text/plain");
        });
        server.Get("/slow", [](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            res.set_content("slow done", "text/plain");
        });

        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
    }

    ~LocalHttpServer()
    {
        server.stop();
        thread.join();
    }
};

}  // namespace

TEST(HttpClientTest, GetAndStatusCodes)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);

    const HttpResponse r = c.get("/hello");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hello http");
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(r.headers.at("content-type"), "text/plain");  // 键统一小写

    const HttpResponse nf = c.get("/no_such_path");
    EXPECT_EQ(nf.status, 404);
    EXPECT_FALSE(nf.ok());
    EXPECT_TRUE(nf.error.empty());   // 404 是 HTTP 层响应，非传输错误
}

TEST(HttpClientTest, PostEchoAndJsonHelper)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);

    const HttpResponse r = c.post("/echo", "负载 body 中文", "text/plain");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body, "负载 body 中文");

    const HttpResponse j = c.post_json("/echo", R"({"k":"v"})");
    EXPECT_TRUE(j.ok());
    EXPECT_EQ(j.body, R"({"k":"v"})");
}

TEST(HttpClientTest, QueryEncoding)
{
    using namespace libmini;
    EXPECT_EQ(HttpClient::build_query({{"a", "1"}, {"b", "2"}}), "a=1&b=2");
    EXPECT_EQ(HttpClient::build_query({}), "");
    // UrlEncode 契约：空格 → '+'（form-urlencoded）；std::map 按键排序
    EXPECT_EQ(HttpClient::build_query({{"q", "x y"}}), "q=x+y");
    EXPECT_EQ(HttpClient::build_query({{"p", "a+b"}}), "p=a%2Bb");  // 字面 + 会被转义

    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);
    const HttpResponse r = c.get("/query", {{"q", "x y"}, {"n", "7"}});
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body, "n=7&q=x y");   // 服务端已解码：+ → 空格，顺序按键排
}

TEST(HttpClientTest, DefaultHeaders)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);
    c.set_default_header("X-Token", "tok-123");
    const HttpResponse r = c.get("/header");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body, "tok-123");

    c.clear_default_headers();
    const HttpResponse r2 = c.get("/header");
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(r2.body, "");
}

TEST(HttpClientTest, TimeoutReportsTransportError)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);
    c.set_timeout_ms(100);           // /slow 睡 400ms：必超时
    const HttpResponse r = c.get("/slow");
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.error.empty());
    EXPECT_FALSE(r.ok());
}

TEST(HttpClientTest, BadUrlReportsError)
{
    using namespace libmini;
    HttpClient bad("host_without_scheme");   // 无 scheme：构造即无效
    const HttpResponse r = bad.get("/x");
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.error.empty());
}

// ------------------------------- LogFacade ---------------------------------

TEST(LogFacadeTest, FileLoggingAndLevelFilter)
{
    using namespace libmini;
    const std::string dir = "logs_facade_test";
    const std::string path = dir + "/app.log";
    remove_tree(dir);   // 干净起点

    LogFacade::Options o;
    o.file_path = path;
    o.console = false;
    o.level = LogLevel::Debug;
    ASSERT_TRUE(LogFacade::init(o));

    spdlog::logger* lg = LogFacade::logger();
    ASSERT_NE(lg, nullptr);
    lg->debug("debug 行 {}", 1);
    lg->info("info 行 {}", 1);
    LogFacade::set_level(LogLevel::Error);
    EXPECT_EQ(LogFacade::level(), LogLevel::Error);
    lg->info("这条不该出现");
    lg->error("error 行");
    LogFacade::flush();
    LogFacade::shutdown();
    EXPECT_EQ(LogFacade::logger(), nullptr);

    const std::string content = read_file(path);
    EXPECT_NE(content.find("debug 行 1"), std::string::npos);
    EXPECT_NE(content.find("info 行 1"), std::string::npos);
    EXPECT_NE(content.find("error 行"), std::string::npos);
    EXPECT_EQ(content.find("这条不该出现"), std::string::npos);

    remove_tree(dir);
}

TEST(LogFacadeTest, ReinitAndAsyncMode)
{
    using namespace libmini;
    const std::string dir = "logs_facade_async_test";
    const std::string path1 = dir + "/a.log";
    const std::string path2 = dir + "/b.log";
    remove_tree(dir);

    LogFacade::Options o;
    o.file_path = path1;
    o.console = false;
    ASSERT_TRUE(LogFacade::init(o));
    LogFacade::logger()->warn("第一份配置");

    // 重复 init：切换到第二份配置（幂等，不崩）
    LogFacade::Options o2;
    o2.file_path = path2;
    o2.console = false;
    o2.async_mode = true;            // 异步模式：shutdown 前落盘
    ASSERT_TRUE(LogFacade::init(o2));
    LogFacade::logger()->critical("第二份配置异步");
    LogFacade::flush();
    LogFacade::shutdown();

    // 第一份文件只含旧内容（切换后不再写入）
    EXPECT_NE(read_file(path1).find("第一份配置"), std::string::npos);
    EXPECT_NE(read_file(path2).find("第二份配置异步"), std::string::npos)
        << "shutdown 后异步日志应已落盘";
    remove_tree(dir);
}

TEST(LogFacadeTest, InvalidConfigFailsCleanly)
{
    using namespace libmini;
    LogFacade::Options o;
    o.console = false;               // 既无控制台也无文件：配置无效
    o.file_path.clear();
    EXPECT_FALSE(LogFacade::init(o));

    // 日志门面会自动创建缺失的父目录（设计行为），所以要构造「真创建不了」
    // 的场景：让路径中间组件是一个已存在的文件
    const std::string blocker = "logs_facade_blocker.tmp";
    write_file(blocker, "i am a file");
    LogFacade::Options bad;
    bad.console = false;
    bad.file_path = blocker + "/log.txt";   // blocker 是文件：mkdir 必败
    EXPECT_FALSE(LogFacade::init(bad));
    remove_file(blocker);

    LogFacade::shutdown();   // 恢复无 logger 状态，不影响其他测试
}

// ------------------------------- config_facade -------------------------------

TEST(ConfigFacadeTest, LayeredPriorityDefaultFileEnv)
{
    using namespace libmini;
    const std::string cfg_path = "test_facade_cfg.json";
    write_file(cfg_path, "{\"server\": {\"port\": 9090, \"host\": \"0.0.0.0\"}, \"retries\": 3}");

    ConfigFacade cfg;
    cfg.set_default("server.port", "8080");
    cfg.set_default("server.timeout_ms", "5000");
    cfg.set_default("log.level", "info");

    // 文件加载前：默认层生效
    EXPECT_EQ(cfg.get_int("server.port"), 8080);
    EXPECT_EQ(cfg.source_of("server.port"), "default");

    // 文件加载后：覆盖默认值
    EXPECT_TRUE(cfg.load_file(cfg_path));
    EXPECT_EQ(cfg.get_int("server.port"), 9090);
    EXPECT_EQ(cfg.get_int("retries"), 3);
    EXPECT_EQ(cfg.get("server.host"), "0.0.0.0");
    EXPECT_EQ(cfg.source_of("server.port"), "file");
    // 未被文件覆盖的默认值仍然生效
    EXPECT_EQ(cfg.get_int("server.timeout_ms"), 5000);

    // 环境变量覆盖文件层：MYAPP_SERVER_PORT
    env_set("MYAPP_SERVER_PORT", "7070");
    cfg.set_env_prefix("MYAPP_");
    cfg.refresh_env();
    EXPECT_EQ(cfg.get_int("server.port"), 7070);
    EXPECT_EQ(cfg.source_of("server.port"), "env");
    // 环境变量没覆盖的键不受影响
    EXPECT_EQ(cfg.get_int("retries"), 3);

    env_remove("MYAPP_SERVER_PORT");
    remove_file(cfg_path);
}

TEST(ConfigFacadeTest, IniAndMissingFile)
{
    using namespace libmini;
    const std::string ini_path = "test_facade_cfg.ini";
    write_file(ini_path, "[db]\r\nhost = 127.0.0.1\r\nport = 5432\r\n");

    ConfigFacade cfg;
    EXPECT_FALSE(cfg.load_file("no_such_file_facade.json"));   // 不存在
    EXPECT_FALSE(cfg.load_file("test_facade_cfg.txt"));        // 不支持的扩展名

    EXPECT_TRUE(cfg.load_file(ini_path));
    EXPECT_EQ(cfg.get("db.host"), "127.0.0.1");
    EXPECT_EQ(cfg.get_int("db.port"), 5432);
    EXPECT_TRUE(cfg.has("db.host"));
    EXPECT_FALSE(cfg.has("db.missing"));

    remove_file(ini_path);
}

TEST(ConfigFacadeTest, NestedJsonAndDefaults)
{
    using namespace libmini;
    const std::string p = "test_facade_nested.json";
    write_file(p, "{\"a\": {\"b\": {\"c\": true, \"d\": 1.5}}}");

    ConfigFacade cfg;
    cfg.set_default("fallback.key", "hello");
    EXPECT_TRUE(cfg.load_file(p));
    EXPECT_TRUE(cfg.get_bool("a.b.c"));
    EXPECT_DOUBLE_EQ(cfg.get_double("a.b.d"), 1.5);
    EXPECT_EQ(cfg.get("fallback.key"), "hello");

    // 类型转换失败回落默认值
    EXPECT_EQ(cfg.get_int("fallback.key", 42), 42);
    EXPECT_EQ(cfg.get_int("a.b.d", 7), 7);   // 1.5 不是 int

    remove_file(p);
}

// ------------------------------- net_addr -------------------------------

TEST(NetAddrTest, ParseEndpointForms)
{
    using namespace libmini;
    std::string host;
    int port = 0;

    EXPECT_TRUE(parse_endpoint("192.168.1.5:8080", host, port));
    EXPECT_EQ(host, "192.168.1.5");
    EXPECT_EQ(port, 8080);

    // IPv6 括号形式
    EXPECT_TRUE(parse_endpoint("[::1]:8080", host, port));
    EXPECT_EQ(host, "::1");
    EXPECT_EQ(port, 8080);
    EXPECT_TRUE(parse_endpoint("[::1]", host, port));
    EXPECT_EQ(host, "::1");
    EXPECT_EQ(port, 0);

    // 纯端口 / 主机名 / 带空端口
    EXPECT_TRUE(parse_endpoint("8080", host, port));
    EXPECT_EQ(port, 8080);
    EXPECT_TRUE(parse_endpoint("myhost", host, port));
    EXPECT_EQ(host, "myhost");
    EXPECT_EQ(port, 0);
    EXPECT_TRUE(parse_endpoint("localhost:", host, port));
    EXPECT_EQ(host, "localhost");
    EXPECT_EQ(port, 0);

    // 非法端口
    std::string h2;
    int p2 = 0;
    EXPECT_FALSE(parse_endpoint("host:99999", h2, p2));
    EXPECT_FALSE(parse_endpoint("host:abc", h2, p2));
    EXPECT_FALSE(parse_endpoint("[::1", h2, p2));

    // 缺省封装：失败回落 0.0.0.0:0
    parse_endpoint_or_default("host:abc", host, port);
    EXPECT_EQ(host, "0.0.0.0");
    EXPECT_EQ(port, 0);
}

TEST(NetAddrTest, Ipv4RoundTrip)
{
    using namespace libmini;
    const std::uint32_t net = ipv4_from_string("192.168.1.5");
    EXPECT_NE(net, 0u);
    EXPECT_EQ(ipv4_to_string(net), "192.168.1.5");

    EXPECT_EQ(ipv4_from_string("0.0.0.0"), 0u);
    EXPECT_EQ(ipv4_to_string(0), "0.0.0.0");
    EXPECT_EQ(ipv4_from_string("256.1.1.1"), 0u);     // 越界
    EXPECT_EQ(ipv4_from_string("1.2.3"), 0u);          // 段数不足
    EXPECT_EQ(ipv4_from_string("01.2.3.4"), 0u);       // 前导零
    EXPECT_EQ(ipv4_from_string("1.2.3.x"), 0u);        // 非数字

    // 回环地址往返
    const std::uint32_t lo = ipv4_from_string("127.0.0.1");
    EXPECT_EQ(ipv4_to_string(lo), "127.0.0.1");
}

TEST(NetAddrTest, ResolveLocalhost)
{
    using namespace libmini;
    // localhost 一定可解析；IPv4 应排在最前（约定）
    const std::vector<NetAddrEntry> entries = resolve_host("localhost", "");
    ASSERT_FALSE(entries.empty());
    EXPECT_FALSE(entries[0].is_ipv6);
    EXPECT_EQ(entries[0].ip, "127.0.0.1");

    // IPv4 字面量快速路径（不依赖 DNS）
    const std::uint32_t net = resolve_ipv4_net("10.0.0.7");
    EXPECT_NE(net, 0u);
    EXPECT_EQ(ipv4_to_string(net), "10.0.0.7");
}

// ------------------------------- timer_wheel -------------------------------

TEST(TimerWheelTest, SingleShotFiresOnce)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);
    wheel.add_ms(50, [&fired] { ++fired; });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(fired.load(), 1);
    EXPECT_TRUE(wheel.idle());
}

TEST(TimerWheelTest, PeriodicAndCancel)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);

    // 周期任务：跑 3 次后自停（fn 返回 false）
    wheel.add_periodic_ms(30, [&fired]() -> bool {
        return ++fired < 3;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(fired.load(), 3);
    EXPECT_TRUE(wheel.idle());

    // 句柄取消：未触发的任务不再执行
    std::atomic<int> cnt(0);
    TimerWheel::Handle h = wheel.add_ms(100, [&cnt] { ++cnt; });
    EXPECT_TRUE(h.cancel());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(cnt.load(), 0);
    EXPECT_TRUE(wheel.idle());

    // 重复取消无效
    EXPECT_FALSE(h.cancel());
}

TEST(TimerWheelTest, BulkTimersO1Add)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);

    // 海量定时器：10k 添加 + 9k 取消，验证 O(1) 路径不退化
    std::vector<TimerWheel::Handle> handles;
    handles.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        handles.push_back(wheel.add_ms(200 + (i % 500), [&fired] { ++fired; }));
    }
    for (int i = 0; i < 9000; ++i) {
        EXPECT_TRUE(handles[static_cast<std::size_t>(i)].cancel());
    }
    EXPECT_EQ(wheel.pending_count(),
              static_cast<std::size_t>(1000));

    // 剩余 1000 个最终全部触发
    wheel.wait_idle();
    EXPECT_EQ(fired.load(), 1000);
}

TEST(TimerWheelTest, WaitIdleBlocksUntilDone)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);
    wheel.add_ms(60, [&fired] { ++fired; });
    wheel.add_ms(120, [&fired] { ++fired; });

    // wait_idle 应阻塞到两个任务都触发
    const auto t0 = std::chrono::steady_clock::now();
    wheel.wait_idle();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(fired.load(), 2);
    EXPECT_GE(elapsed, 60);   // 至少等到第一个任务到期
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
