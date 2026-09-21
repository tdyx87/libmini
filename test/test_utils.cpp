// 新增通用模块（第一批 Boost 风格工具）的单元测试
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "libmini.h"

// ------------------------------ string_algo ------------------------------

TEST(StringAlgoTest, PrefixSuffixContains)
{
    using namespace libmini;
    EXPECT_TRUE(starts_with("hello world", "hello"));
    EXPECT_FALSE(starts_with("hello", "world"));
    EXPECT_TRUE(starts_with("hi", ""));      // 空前缀恒真
    EXPECT_FALSE(starts_with("", "hi"));

    EXPECT_TRUE(ends_with("file.txt", ".txt"));
    EXPECT_FALSE(ends_with("file.txt", ".doc"));
    EXPECT_TRUE(ends_with("x", ""));

    EXPECT_TRUE(contains("hello world", "lo w"));
    EXPECT_FALSE(contains("abc", "xyz"));
    EXPECT_TRUE(contains("abc", ""));
}

TEST(StringAlgoTest, CaseInsensitiveCompare)
{
    using namespace libmini;
    EXPECT_TRUE(iequals("Hello", "hELLO"));
    EXPECT_TRUE(iequals("", ""));
    EXPECT_FALSE(iequals("abc", "abd"));
    EXPECT_FALSE(iequals("abc", "abcd"));
}

TEST(StringAlgoTest, TrimPrefixSuffix)
{
    using namespace libmini;
    EXPECT_EQ(trim_prefix("https://x.com", "https://"), "x.com");
    EXPECT_EQ(trim_prefix("plain", "https://"), "plain");  // 不匹配原样返回

    EXPECT_EQ(trim_suffix("data.json", ".json"), "data");
    EXPECT_EQ(trim_suffix("data.json", ".xml"), "data.json");
}

TEST(StringAlgoTest, Replace)
{
    using namespace libmini;
    EXPECT_EQ(replace_all("a-b-c", "-", "+"), "a+b+c");
    EXPECT_EQ(replace_all("aaa", "aa", "b"), "ba");  // 顺序扫描不回溯
    EXPECT_EQ(replace_all("abc", "", "x"), "abc");   // 空子串不改写
    EXPECT_EQ(replace_first("a-a", "a", "X"), "X-a");
    EXPECT_EQ(replace_first("bbb", "z", "X"), "bbb");
}

TEST(StringAlgoTest, SplitJoin)
{
    using namespace libmini;
    // "  a \t b\nc  " 含 3 个 token：a、b、c（末尾 c 后只有空白）
    const std::vector<std::string> ws = split_whitespace("  a \t b\nc  ");
    ASSERT_EQ(ws.size(), 3u);
    EXPECT_EQ(ws[0], "a");
    EXPECT_EQ(ws[1], "b");
    EXPECT_EQ(ws[2], "c");

    std::vector<std::string> v = split_string("a,,b", ",", false);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[1], "");

    v = split_string("a,,b", ",", true);
    ASSERT_EQ(v.size(), 2u);

    // 空串按 "," 切分得到一个空 token（与 std::stoul 系工具的行为一致）
    v = split_string("", ",", false);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "");

    // 多字符分隔符
    v = split_string("x::y::z", "::", false);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[2], "z");

    EXPECT_EQ(join({"a", "b", "c"}, "-"), "a-b-c");
    EXPECT_EQ(join({}, "-"), "");
    EXPECT_EQ(join({"only"}, "-"), "only");
}

// ------------------------------ lexical_cast ------------------------------

TEST(LexicalCastTest, BasicConversions)
{
    using namespace libmini;
    EXPECT_EQ(lexical_cast<int>("42"), 42);
    EXPECT_EQ(lexical_cast<int>("-7"), -7);
    EXPECT_EQ(lexical_cast<double>("3.5"), 3.5);
    EXPECT_EQ(lexical_cast<std::string>(123), "123");
    EXPECT_EQ(lexical_cast<std::string>(3.25), "3.25");
    EXPECT_EQ(lexical_cast<bool>("1"), true);
    EXPECT_EQ(lexical_cast<std::string>(std::string("same")), "same");
}

TEST(LexicalCastTest, Failures)
{
    using namespace libmini;
    EXPECT_THROW(lexical_cast<int>("abc"), bad_lexical_cast);
    EXPECT_THROW(lexical_cast<int>("42x"), bad_lexical_cast);   // 尾部垃圾
    EXPECT_THROW(lexical_cast<int>(""), bad_lexical_cast);
    EXPECT_THROW(lexical_cast<int>("  42"), bad_lexical_cast);  // 不接受前导空白
    EXPECT_THROW(lexical_cast<double>("1.2.3"), bad_lexical_cast);

    EXPECT_EQ(lexical_cast_or<int>("abc", -1), -1);
    EXPECT_EQ(lexical_cast_or<int>("99", -1), 99);
}

// --------------------------------- uuid ----------------------------------

TEST(UuidTest, GenerateAndFormat)
{
    using namespace libmini;
    const Uuid a = Uuid::generate();
    const Uuid b = Uuid::generate();

    EXPECT_NE(a, b);  // 两次生成几乎不可能相同
    const std::string s = a.to_string();
    ASSERT_EQ(s.size(), 36u);
    EXPECT_EQ(s[8], '-');
    EXPECT_EQ(s[13], '-');
    EXPECT_EQ(s[18], '-');
    EXPECT_EQ(s[23], '-');
    // version 4 与 variant 位
    EXPECT_EQ(s[14], '4');
    EXPECT_TRUE(s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');

    EXPECT_EQ(a.to_hex_string().size(), 32u);
}

TEST(UuidTest, ParseRoundTrip)
{
    using namespace libmini;
    const std::string kText = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    Uuid u = Uuid::nil();
    ASSERT_TRUE(Uuid::parse(kText, u));
    EXPECT_EQ(u.to_string(), kText);          // 小写往返一致
    EXPECT_EQ(u.to_hex_string(), "f47ac10b58cc4372a5670e02b2c3d479");

    // 大写与无连字符形式
    Uuid u2;
    EXPECT_TRUE(Uuid::parse("F47AC10B-58CC-4372-A567-0E02B2C3D479", u2));
    EXPECT_EQ(u2, u);
    EXPECT_TRUE(Uuid::parse("f47ac10b58cc4372a5670e02b2c3d479", u2));
    EXPECT_EQ(u2, u);

    // nil 往返
    const Uuid z = Uuid::nil();
    EXPECT_TRUE(z.is_nil());
    EXPECT_TRUE(Uuid::parse(z.to_string(), u2));
    EXPECT_TRUE(u2.is_nil());

    // 非法输入
    Uuid out;
    EXPECT_FALSE(Uuid::parse("", out));
    EXPECT_FALSE(Uuid::parse("f47ac10b58cc4372a5670e02b2c3d47", out));   // 35 位
    EXPECT_FALSE(Uuid::parse("f47ac10b-58cc-4372-a567-0e02b2c3d47g", out));  // 非法字符
    EXPECT_FALSE(Uuid::parse("f47ac10b_58cc-4372-a567-0e02b2c3d479", out));  // 连字符位置错
}

TEST(UuidTest, MapKeyOrdering)
{
    using namespace libmini;
    std::map<Uuid, int> m;
    const Uuid a = Uuid::generate();
    const Uuid b = Uuid::generate();
    m[a] = 1;
    m[b] = 2;
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[a], 1);
    EXPECT_EQ(m[b], 2);
}

// --------------------------------- crc -----------------------------------

TEST(Crc32Test, KnownValues)
{
    using namespace libmini;
    // 标准测试向量
    EXPECT_EQ(Crc32::compute(""), 0x00000000u);
    EXPECT_EQ(Crc32::compute("a"), 0xE8B7BE43u);
    EXPECT_EQ(Crc32::compute("abc"), 0x352441C2u);
    EXPECT_EQ(Crc32::compute("123456789"), 0xCBF43926u);
    EXPECT_EQ(Crc32::compute("The quick brown fox jumps over the lazy dog"),
              0x414FA339u);
}

TEST(Crc32Test, IncrementalEqualsOneShot)
{
    using namespace libmini;
    const std::string data = "hello incremental crc world";
    Crc32 c;
    c.update(data.substr(0, 5));
    c.update(data.substr(5, 7));
    c.update(data.substr(12));
    EXPECT_EQ(c.value(), Crc32::compute(data));

    c.reset();
    c.update(data);
    EXPECT_EQ(c.value(), Crc32::compute(data));
}

// ------------------------------- encoding --------------------------------

TEST(Base64Test, RoundTripAndKnownVectors)
{
    using namespace libmini;
    EXPECT_EQ(Base64::encode(""), "");
    EXPECT_EQ(Base64::encode("f"), "Zg==");
    EXPECT_EQ(Base64::encode("fo"), "Zm8=");
    EXPECT_EQ(Base64::encode("foo"), "Zm9v");
    EXPECT_EQ(Base64::encode("foobar"), "Zm9vYmFy");

    std::string out;
    ASSERT_TRUE(Base64::decode("Zm9vYmFy", out));
    EXPECT_EQ(out, "foobar");
    ASSERT_TRUE(Base64::decode("Zm8=", out));
    EXPECT_EQ(out, "fo");

    // 二进制内容往返
    std::string binary;
    for (int i = 0; i < 256; ++i) {
        binary += static_cast<char>(i);
    }
    ASSERT_TRUE(Base64::decode(Base64::encode(binary), out));
    EXPECT_EQ(out, binary);

    // 非法输入
    std::string bad;
    EXPECT_FALSE(Base64::decode("Zm9v!", bad));     // 非法字符
    EXPECT_FALSE(Base64::decode("Z", bad));         // 长度不足
    EXPECT_FALSE(Base64::decode("Zm9vYmF", bad));   // 长度 %4 != 0
    EXPECT_FALSE(Base64::decode("Zm=v", bad));      // '=' 不在末尾
}

TEST(HexTest, RoundTrip)
{
    using namespace libmini;
    EXPECT_EQ(Hex::encode(""), "");
    EXPECT_EQ(Hex::encode("\xDE\xAD\xBE\xEF"), "DEADBEEF");
    EXPECT_EQ(Hex::encode(std::string("\xDE\xAD\xBE\xEF"), true), "deadbeef");

    std::string out;
    ASSERT_TRUE(Hex::decode("DEADBEEF", out));
    EXPECT_EQ(out.size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(out[0]), 0xDE);
    EXPECT_TRUE(Hex::decode("deadbeef", out));
    EXPECT_EQ(static_cast<unsigned char>(out[3]), 0xEF);

    // 二进制往返
    std::string binary;
    for (int i = 0; i < 256; ++i) {
        binary += static_cast<char>(i);
    }
    ASSERT_TRUE(Hex::decode(Hex::encode(binary), out));
    EXPECT_EQ(out, binary);

    EXPECT_FALSE(Hex::decode("ABC", out));   // 奇数长度
    EXPECT_FALSE(Hex::decode("ZZ", out));    // 非法字符
}

TEST(UrlEncodeTest, RoundTrip)
{
    using namespace libmini;
    EXPECT_EQ(UrlEncode::encode("abc123-_.~"), "abc123-_.~");  // unreserved 原样
    EXPECT_EQ(UrlEncode::encode("a b"), "a+b");
    EXPECT_EQ(UrlEncode::encode("a&b=c"), "a%26b%3Dc");
    EXPECT_EQ(UrlEncode::encode("\xE4\xB8\xAD"), "%E4%B8%AD");  // UTF-8 "中"

    EXPECT_EQ(UrlEncode::decode("a+b"), "a b");
    EXPECT_EQ(UrlEncode::decode("a%26b%3Dc"), "a&b=c");
    EXPECT_EQ(UrlEncode::decode("%E4%B8%AD"), "\xE4\xB8\xAD");

    // 往返
    const std::string raw = "q=hello world&x=1&name=张三";
    EXPECT_EQ(UrlEncode::decode(UrlEncode::encode(raw)), raw);

    // 非法转义原样保留
    EXPECT_EQ(UrlEncode::decode("100%"), "100%");
    EXPECT_EQ(UrlEncode::decode("%G1"), "%G1");
}

// ------------------------------- stopwatch -------------------------------

TEST(StopwatchTest, MeasuresElapsed)
{
    using namespace libmini;
    Stopwatch sw;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::int64_t ms = sw.elapsed_ms();
    EXPECT_GE(ms, 40);
    EXPECT_LE(ms, 2000);

    EXPECT_GE(sw.elapsed_us(), ms * 1000);
    EXPECT_GE(sw.elapsed_ns(), sw.elapsed_us() * 1000);
    // 两次读取之间时间会前进，用宽松比较验证换算一致
    EXPECT_NEAR(sw.elapsed_seconds(), static_cast<double>(sw.elapsed_ns()) / 1e9,
                1e-3);
    EXPECT_FALSE(sw.elapsed_string().empty());
}

TEST(StopwatchTest, PauseResumeRestart)
{
    using namespace libmini;
    Stopwatch sw;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sw.pause();
    const std::int64_t first = sw.elapsed_ms();
    EXPECT_FALSE(sw.is_running());

    // 暂停期间时间不再增长
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(sw.elapsed_ms(), first);

    sw.resume();
    EXPECT_TRUE(sw.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_GE(sw.elapsed_ms(), first + 30);

    sw.restart();
    EXPECT_TRUE(sw.is_running());
    // 余量设计：Windows 定时器粒度 15.6ms，sleep_for 会向上取整，
    // 第二段必须远小于 first（first ≈ 60ms + 取整）才不会在
    // 高负载/取整叠加时与 first 打平（CI 实测 40 vs 40）
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_LT(sw.elapsed_ms(), first);  // 已清零重新计时
}

// ------------------------------ scope_guard ------------------------------

TEST(ScopeGuardTest, RunsOnDestruction)
{
    using namespace libmini;
    int calls = 0;
    {
        auto g = make_scope_guard([&calls] { ++calls; });
        EXPECT_EQ(calls, 0);
    }
    EXPECT_EQ(calls, 1);
}

TEST(ScopeGuardTest, DismissCancels)
{
    using namespace libmini;
    int calls = 0;
    {
        auto g = make_scope_guard([&calls] { ++calls; });
        g.dismiss();
    }
    EXPECT_EQ(calls, 0);
}

TEST(ScopeGuardTest, RunsOnExceptionPath)
{
    using namespace libmini;
    int cleanup_calls = 0;
    auto act = [&cleanup_calls] {
        auto g = make_scope_guard([&cleanup_calls] { ++cleanup_calls; });
        throw std::runtime_error("boom");
    };
    EXPECT_THROW(act(), std::runtime_error);
    EXPECT_EQ(cleanup_calls, 1);  // 异常路径也保证清理
}

TEST(ScopeGuardTest, MoveTransfersOwnership)
{
    using namespace libmini;
    int calls = 0;
    {
        ScopeGuard<std::function<void()>> a([&calls] { ++calls; });
        ScopeGuard<std::function<void()>> b(std::move(a));  // 移动后 a 不再执行
        // a、b 在此块结束，b 的析构执行一次（a 已被移动，不再执行）
    }
    EXPECT_EQ(calls, 1);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
