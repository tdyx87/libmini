// 新增通用模块（第一批 Boost 风格工具）的单元测试
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libmini.h"
#include "utils/charset.h"
#include "utils/grapheme.h"

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

// ------------------------- string_algo（UTF-8 感知） -------------------------
//
// 核心主张：按码点切分不会破坏多字节序列——任意切点下，输出必须仍是合法
// UTF-8，且是原串的字节前缀。

TEST(Utf8Test, ValidityDetection)
{
    using namespace libmini;
    EXPECT_TRUE(utf8_is_valid(""));
    EXPECT_TRUE(utf8_is_valid("plain ascii"));
    EXPECT_TRUE(utf8_is_valid("中文测试"));          // 3 字节序列
    EXPECT_TRUE(utf8_is_valid("\xF0\x9F\x98\x80"));  // U+1F600，4 字节

    EXPECT_FALSE(utf8_is_valid("\x80"));              // 孤立续字节
    EXPECT_FALSE(utf8_is_valid("\xE4\xB8"));          // 被截断的三字节序列
    EXPECT_FALSE(utf8_is_valid("\xC0\x80"));          // 过长编码（overlong NUL）
    EXPECT_FALSE(utf8_is_valid("\xED\xA0\x80"));      // U+D800，代理项区间
    EXPECT_FALSE(utf8_is_valid("\xF4\x90\x80\x80"));  // U+110000，超出码点上限
    EXPECT_FALSE(utf8_is_valid("ok\xFF"));            // 0xFF 不是合法首字节
}

TEST(Utf8Test, LengthCountsCodePoints)
{
    using namespace libmini;
    EXPECT_EQ(utf8_length(""), 0u);
    EXPECT_EQ(utf8_length("abc"), 3u);
    EXPECT_EQ(utf8_length("中文"), 2u);
    EXPECT_EQ(utf8_length("a中b"), 3u);
    EXPECT_EQ(utf8_length("\xF0\x9F\x98\x80"), 1u);  // emoji：1 个字符、4 字节

    // 对照：字节长度随内容膨胀，界面上的「几个字」要用 utf8_length
    EXPECT_EQ(std::string("中文").size(), 6u);
}

TEST(Utf8Test, ByteOffsetMapping)
{
    using namespace libmini;
    const std::string s = "a中文";  // 1 + 3 + 3 = 7 字节，3 个字符
    EXPECT_EQ(s.size(), 7u);
    EXPECT_EQ(utf8_byte_offset(s, 0), 0u);
    EXPECT_EQ(utf8_byte_offset(s, 1), 1u);
    EXPECT_EQ(utf8_byte_offset(s, 2), 4u);
    EXPECT_EQ(utf8_byte_offset(s, 3), 7u);                 // == size()
    EXPECT_EQ(utf8_byte_offset(s, 4), std::string::npos);  // 越界
}

TEST(Utf8Test, SubstrAndSliceByCodePoint)
{
    using namespace libmini;
    const std::string s = "中文测试";  // 4 个字符、12 字节

    EXPECT_EQ(utf8_substr(s, 1, 2), "文测");
    EXPECT_EQ(utf8_substr(s, 2), "测试");  // count 默认到末尾
    EXPECT_EQ(utf8_substr(s, 0), s);
    EXPECT_EQ(utf8_substr(s, 4), "");  // 刚好落在末尾
    EXPECT_EQ(utf8_substr(s, 9), "");  // 越界 → 空串

    EXPECT_EQ(utf8_slice(s, 1, 3), "文测");
    EXPECT_EQ(utf8_slice(s, 2, std::string::npos), "测试");
    EXPECT_EQ(utf8_slice(s, 3, 1), "");  // end <= begin
    EXPECT_EQ(utf8_slice(s, 0, 0), "");
    EXPECT_EQ(utf8_slice(s, 9, std::string::npos), "");
}

TEST(Utf8Test, TruncateNeverSplitsSequences)
{
    using namespace libmini;
    const std::string s = "中a文b";  // 4 个字符、8 字节

    EXPECT_EQ(utf8_truncate(s, 0), "");
    EXPECT_EQ(utf8_truncate(s, 1), "中");
    EXPECT_EQ(utf8_truncate(s, 3), "中a文");
    EXPECT_EQ(utf8_truncate(s, 10), s);

    // 任意切点：输出必须是合法 UTF-8，且是原串的字节前缀
    for (std::size_t n = 0; n <= utf8_length(s) + 1; ++n) {
        const std::string cut = utf8_truncate(s, n);
        const std::size_t expected = n > utf8_length(s) ? utf8_length(s) : n;
        EXPECT_TRUE(utf8_is_valid(cut)) << "n=" << n;
        EXPECT_EQ(s.compare(0, cut.size(), cut), 0) << "n=" << n;
        EXPECT_EQ(utf8_length(cut), expected) << "n=" << n;
    }

    // 对照：字节级截断会把序列切坏（这正是要避免的）
    EXPECT_FALSE(utf8_is_valid(std::string("中文").substr(0, 1)));
}

TEST(Utf8Test, TruncateBytesRespectsBoundary)
{
    using namespace libmini;
    const std::string s = "中文";  // 6 字节，2 个字符

    EXPECT_EQ(utf8_truncate_bytes(s, 0), "");
    EXPECT_EQ(utf8_truncate_bytes(s, 2), "");   // 放不下第一个 3 字节字符
    EXPECT_EQ(utf8_truncate_bytes(s, 3), "中");
    EXPECT_EQ(utf8_truncate_bytes(s, 5), "中");  // 宁可短一点，也不切半个「文」
    EXPECT_EQ(utf8_truncate_bytes(s, 6), s);
    EXPECT_EQ(utf8_truncate_bytes(s, 99), s);

    EXPECT_EQ(utf8_truncate_bytes("abcdef", 3), "abc");  // 纯 ASCII 按字节切
}

TEST(Utf8Test, TailKeepsWholeCharacters)
{
    using namespace libmini;
    const std::string s = "中文测试";
    EXPECT_EQ(utf8_tail(s, 0), "");
    EXPECT_EQ(utf8_tail(s, 2), "测试");
    EXPECT_EQ(utf8_tail(s, 3), "文测试");
    EXPECT_EQ(utf8_tail(s, 4), s);
    EXPECT_EQ(utf8_tail(s, 99), s);
}

TEST(Utf8Test, TolerantOnInvalidInput)
{
    using namespace libmini;
    // 非法字节按 1 个「字符」计：不丢字节，也不会让后面的位置整体错位
    const std::string bad = "a\x80\xE4\xB8\xAD";  // 'a' + 孤立续字节 + "中"
    EXPECT_FALSE(utf8_is_valid(bad));
    EXPECT_EQ(utf8_length(bad), 3u);  // 'a'、0x80、'中'
    EXPECT_EQ(utf8_substr(bad, 1, 1), "\x80");
    EXPECT_EQ(utf8_substr(bad, 2, 1), "中");  // 坏字节没让后面错位
    EXPECT_TRUE(utf8_is_valid(utf8_substr(bad, 2, 1)));
}

// ------------------------- string_algo（字形簇感知） -------------------------
//
// 核心主张：码点级截断虽然不会切坏字节序列，却会把一个「用户感知的字符」
// 拆开（组合重音、emoji ZWJ 序列、国旗、Hangul 音节…）。字形簇级操作要么
// 完整保留一个字形簇，要么整个丢弃它——绝不产生「半个字符」。
//
// 下面的样例统一写成显式字节转义，避免源文件编码与编辑器差异影响结果。

namespace {

const char* kEAcute = "e\xCC\x81";                    // e + U+0301 组合重音
const char* kFamily =
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
const char* kFlagCN = "\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3";  // 两个区域指示符
const char* kHeartVs = "\xE2\x9D\xA4\xEF\xB8\x8F";        // U+2764 + VS16
const char* kThumbTone = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD";  // 👍 + 肤色修饰符
const char* kHangulJamo = "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB";  // L + V + T
const char* kIndicConjunct = "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7";  // क + ् + ष

}  // namespace

TEST(GraphemeTest, CountsUserPerceivedCharacters)
{
    using namespace libmini;
    EXPECT_EQ(utf8_grapheme_length(""), 0u);
    EXPECT_EQ(utf8_grapheme_length("abc"), 3u);
    EXPECT_EQ(utf8_grapheme_length("中文测试"), 4u);

    // 组合字符：两个码点、一个字形簇
    EXPECT_EQ(std::string(kEAcute).size(), 3u);
    EXPECT_EQ(utf8_length(kEAcute), 2u);
    EXPECT_EQ(utf8_grapheme_length(kEAcute), 1u);

    // emoji ZWJ 序列：5 个码点、一个字形簇
    EXPECT_EQ(utf8_length(kFamily), 5u);
    EXPECT_EQ(utf8_grapheme_length(kFamily), 1u);

    // 国旗：两个区域指示符成对
    EXPECT_EQ(utf8_grapheme_length(kFlagCN), 1u);

    // 变体选择符（U+FE0F）与 emoji 肤色修饰符都黏在基字符上
    EXPECT_EQ(utf8_grapheme_length(kHeartVs), 1u);
    EXPECT_EQ(utf8_grapheme_length(kThumbTone), 1u);

    // Hangul：分解的 Jamo（L + V + T）合起来是一个音节
    EXPECT_EQ(utf8_length(kHangulJamo), 3u);
    EXPECT_EQ(utf8_grapheme_length(kHangulJamo), 1u);

    // Indic 连字（GB9c）：辅音 + virama + 辅音
    EXPECT_EQ(utf8_length(kIndicConjunct), 3u);
    EXPECT_EQ(utf8_grapheme_length(kIndicConjunct), 1u);

    // 三个国旗是 3 个字形簇（区域指示符成对，不越界黏连）
    const std::string three_flags =
        std::string(kFlagCN) + kFlagCN + kFlagCN;
    EXPECT_EQ(utf8_grapheme_length(three_flags), 3u);
}

TEST(GraphemeTest, ControlCharactersBreakClusters)
{
    using namespace libmini;
    EXPECT_EQ(utf8_grapheme_length("\r\n"), 1u);  // GB3：CR LF 是一簇
    EXPECT_EQ(utf8_grapheme_length("a\r\nb"), 3u);
    EXPECT_EQ(utf8_grapheme_length("a\rb"), 3u);       // GB4：CR 之后必断
    EXPECT_EQ(utf8_grapheme_length("a\nb"), 3u);
    EXPECT_EQ(utf8_grapheme_length("a\tb"), 3u);
    // 控制字符之后即使跟组合字符也不会黏上（GB4 优先于 GB9）
    EXPECT_EQ(utf8_grapheme_length(std::string("\r") + kEAcute), 2u);

    // Prepend：U+0600 与其后的字符同簇（GB9b）
    EXPECT_EQ(utf8_grapheme_length("\xD8\x80" "0"), 1u);

    // SpacingMark：天城文元音符号（GB9a），且它不是 Extend
    EXPECT_EQ(utf8_grapheme_length("\xE0\xA4\x95\xE0\xA4\xBE"), 1u);

    // ZWJ 只在两个图形字符之间才黏合：a + ZWJ 仍与 b 断开
    EXPECT_EQ(utf8_grapheme_length("a\xE2\x80\x8D" "b"), 2u);
}

TEST(GraphemeTest, TruncateNeverSplitsClusters)
{
    using namespace libmini;
    const std::string s = std::string("a") + kEAcute + kFlagCN + kThumbTone;
    const std::size_t clusters = 4;
    EXPECT_EQ(utf8_grapheme_length(s), clusters);

    EXPECT_EQ(utf8_grapheme_truncate(s, 0), "");
    EXPECT_EQ(utf8_grapheme_truncate(s, 1), "a");
    EXPECT_EQ(utf8_grapheme_truncate(s, 2), std::string("a") + kEAcute);
    EXPECT_EQ(utf8_grapheme_truncate(s, clusters), s);
    EXPECT_EQ(utf8_grapheme_truncate(s, 99), s);

    // 任意切点：输出必须是合法 UTF-8、是原串的字节前缀，且簇数恰为 min(n, 总数)
    for (std::size_t n = 0; n <= clusters + 1; ++n) {
        const std::string cut = utf8_grapheme_truncate(s, n);
        const std::size_t expected = n > clusters ? clusters : n;
        EXPECT_TRUE(utf8_is_valid(cut)) << "n=" << n;
        EXPECT_EQ(s.compare(0, cut.size(), cut), 0) << "n=" << n;
        EXPECT_EQ(utf8_grapheme_length(cut), expected) << "n=" << n;
    }

    // 对照：码点级截断会把 emoji 家庭切成「一个男人 + 悬空的连接符」——
    // 结果是合法 UTF-8，却不是原来那个字形族
    const std::string by_code_point = utf8_truncate(kFamily, 2);
    EXPECT_TRUE(utf8_is_valid(by_code_point));
    EXPECT_NE(by_code_point, std::string(kFamily));
    EXPECT_EQ(by_code_point, "\xF0\x9F\x91\xA8\xE2\x80\x8D");
}

TEST(GraphemeTest, SubstrSliceAndTail)
{
    using namespace libmini;
    const std::string s = std::string(kEAcute) + "x" + kFamily;  // 3 个簇
    EXPECT_EQ(utf8_grapheme_length(s), 3u);

    EXPECT_EQ(utf8_grapheme_byte_offset(s, 0), 0u);
    EXPECT_EQ(utf8_grapheme_byte_offset(s, 1), 3u);  // kEAcute 占 3 字节
    EXPECT_EQ(utf8_grapheme_byte_offset(s, 2), 4u);
    EXPECT_EQ(utf8_grapheme_byte_offset(s, 3), s.size());
    EXPECT_EQ(utf8_grapheme_byte_offset(s, 4), std::string::npos);

    EXPECT_EQ(utf8_grapheme_substr(s, 0, 1), kEAcute);
    EXPECT_EQ(utf8_grapheme_substr(s, 1, 1), "x");
    EXPECT_EQ(utf8_grapheme_substr(s, 2), kFamily);
    EXPECT_EQ(utf8_grapheme_substr(s, 3), "");   // 刚好落在末尾
    EXPECT_EQ(utf8_grapheme_substr(s, 9), "");   // 越界 → 空串

    EXPECT_EQ(utf8_grapheme_slice(s, 1, 3), std::string("x") + kFamily);
    EXPECT_EQ(utf8_grapheme_slice(s, 0, 0), "");
    EXPECT_EQ(utf8_grapheme_slice(s, 2, 1), "");  // end <= begin

    EXPECT_EQ(utf8_grapheme_tail(s, 0), "");
    EXPECT_EQ(utf8_grapheme_tail(s, 2), std::string("x") + kFamily);
    EXPECT_EQ(utf8_grapheme_tail(s, 3), s);
    EXPECT_EQ(utf8_grapheme_tail(s, 99), s);
}

TEST(GraphemeTest, TruncateBytesKeepsWholeClusters)
{
    using namespace libmini;
    const std::string family = kFamily;  // 18 字节、1 个字形簇
    ASSERT_EQ(family.size(), 18u);

    // 放不下整个字形簇就一点都不放——绝不交出「半个 emoji」
    EXPECT_EQ(utf8_grapheme_truncate_bytes(family, 0), "");
    EXPECT_EQ(utf8_grapheme_truncate_bytes(family, 17), "");
    EXPECT_EQ(utf8_grapheme_truncate_bytes(family, 18), family);
    EXPECT_EQ(utf8_grapheme_truncate_bytes(family, 99), family);

    const std::string mixed = std::string("ab") + kFamily;
    EXPECT_EQ(utf8_grapheme_truncate_bytes(mixed, 2), "ab");
    EXPECT_EQ(utf8_grapheme_truncate_bytes(mixed, 4), "ab");
    EXPECT_EQ(utf8_grapheme_truncate_bytes(mixed, 20), mixed);

    EXPECT_EQ(utf8_grapheme_truncate_bytes("abcdef", 3), "abc");
}

TEST(GraphemeTest, TolerantOnInvalidInput)
{
    using namespace libmini;
    // 坏字节单独算一个「字形簇」，且不影响后面的位置——与 utf8_* 完全一致
    const std::string bad = std::string("e\x80") + kEAcute;
    EXPECT_FALSE(utf8_is_valid(bad));
    EXPECT_EQ(utf8_grapheme_length(bad), 3u);  // 'e'、坏字节、U+0301
    EXPECT_EQ(utf8_grapheme_truncate(bad, 1), "e");
    EXPECT_EQ(utf8_grapheme_truncate(bad, 2), "e\x80");
    EXPECT_EQ(utf8_grapheme_truncate(bad, 3), bad);
    EXPECT_EQ(grapheme_cluster_step("\x80\x80", 0), 1u);
    EXPECT_EQ(grapheme_cluster_step("abc", 3), 0u);  // 已在末尾
}

TEST(GraphemeTest, ReportsTableProvenance)
{
    using namespace libmini;
    const char* version = grapheme_data_version();
    ASSERT_NE(version, nullptr);
    EXPECT_GT(std::string(version).size(), 0u);

    // 完整模式与启发式模式的说明串必须能区分开，且与 full_conformance 一致
    const bool full = grapheme_full_conformance();
    const bool says_heuristic =
        std::string(version).find("heuristic") != std::string::npos;
    EXPECT_EQ(full, !says_heuristic);

    // 构建期开关（-DLIBMINI_UNICODE_FULL_GRAPHEME=ON，PUBLIC 定义一路传到本
    // 文件）与运行期实际用的表必须一致。只查运行期结果的话，下面的一致性用例
    // 会「开关空转」——选了启发式分支却仍按运行时判定走白名单，安安静静通过；
    // CI 的 full-grapheme 变体正是靠这条断言保证自己验的是完整表
#if defined(LIBMINI_UNICODE_FULL_GRAPHEME)
    EXPECT_TRUE(full) << "LIBMINI_UNICODE_FULL_GRAPHEME is defined at build "
                      << "time but the runtime tables report \"" << version
                      << "\"";
#endif
}

// ------------------- UAX #29 符合性测试（数据驱动） -------------------
//
// 上面的用例挑的是典型场景，但「典型」证明不了符合性——UAX #29 的正确性只能
// 让官方用例集自己说话。这里逐条跑 Unicode 官方的 GraphemeBreakTest.txt
//（随仓库提交，见 test/data/），把当初探针一次性跑出来的结论变成每次构建都会
// 重跑的断言：属性表换了、规则改了、生成器漂移了，都会立刻在这里红掉。
//
// 数据文件路径由 CMake 注入（见 test/CMakeLists.txt），不用相对路径猜工作目录；
// 未注入时（例如手工单独编译本文件）整个套件不参与编译。

#if defined(LIBMINI_GRAPHEME_TEST_DATA)

namespace {

// 一条官方用例：码点序列 + 拼出来的 UTF-8 文本 + 期望的簇边界（字节偏移）
struct GraphemeCase
{
    std::vector<unsigned long> code_points;
    std::string text;
    std::vector<std::size_t> expected_breaks;
};

enum CaseLineResult
{
    kNotACase,    // 注释或空行
    kCaseParsed,  // 一条完整用例
    kCaseBroken   // 有 token 但不是合法用例——不能被静默跳过
};

std::string utf8_encode(unsigned long cp)
{
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

std::string code_point_name(unsigned long cp)
{
    std::ostringstream os;
    os << "U+" << std::uppercase << std::hex << std::setfill('0')
       << std::setw(cp > 0xFFFF ? 5 : 4) << cp;
    return os.str();
}

// 官方文件里每条用例的写法：`÷ 0061 × 0301 ÷ 0062 ÷  # ÷ 0061 × 0301 ÷ 0062 ÷`
// 其中 ÷（U+00F7）表示「允许在此断开」，×（U+00D7）表示「不允许断开」，标记出现
// 在被判定字符的前面。'#' 之后是重复用例本身的注释，丢掉即可。
CaseLineResult parse_case_line(const std::string& raw, GraphemeCase& out)
{
    const std::string line = raw.substr(0, raw.find('#'));
    const std::string kBreak = "\xC3\xB7";     // U+00F7 ÷
    const std::string kNoBreak = "\xC3\x97";   // U+00D7 ×

    std::istringstream tokens(line);
    std::string token;
    bool break_before_next = false;
    bool saw_token = false;

    while (tokens >> token) {
        saw_token = true;
        if (token == kBreak) {
            break_before_next = true;
            continue;
        }
        if (token == kNoBreak) {
            break_before_next = false;
            continue;
        }
        if (token.size() > 8) {
            return kCaseBroken;  // 十六进制码点不会这么长
        }
        char* end = nullptr;
        const unsigned long cp = std::strtoul(token.c_str(), &end, 16);
        if (end == token.c_str() || *end != '\0' || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return kCaseBroken;
        }
        const std::size_t offset = out.text.size();
        out.code_points.push_back(cp);
        out.text += utf8_encode(cp);
        if (break_before_next) {
            out.expected_breaks.push_back(offset);
        }
        break_before_next = false;
    }

    if (!saw_token) {
        return kNotACase;
    }
    if (out.code_points.empty()) {
        return kCaseBroken;  // 只有断点标记、没有码点
    }
    out.expected_breaks.push_back(out.text.size());  // 串尾永远是边界
    return kCaseParsed;
}

// 只关心返回值时用这个，避免把上一条用例的状态带进下一条
CaseLineResult parse_one(const std::string& line, GraphemeCase* out = nullptr)
{
    GraphemeCase parsed;
    const CaseLineResult result = parse_case_line(line, parsed);
    if (out != nullptr) {
        *out = parsed;
    }
    return result;
}

std::string describe_mismatch(const GraphemeCase& c,
                              const std::vector<std::size_t>& actual)
{
    std::ostringstream os;
    os << "  case:";
    for (std::size_t i = 0; i < c.code_points.size(); ++i) {
        os << " " << code_point_name(c.code_points[i]);
    }
    os << "\n    expected breaks:";
    for (std::size_t i = 0; i < c.expected_breaks.size(); ++i) {
        os << " " << c.expected_breaks[i];
    }
    os << "\n    actual   breaks:";
    for (std::size_t i = 0; i < actual.size(); ++i) {
        os << " " << actual[i];
    }
    os << "\n";
    return os.str();
}

// 失败信息里最多列 limit 条，其余折叠成一行计数
std::string summarize(const std::vector<GraphemeCase>& cases,
                      const std::vector<std::vector<std::size_t> >& actuals,
                      std::size_t limit)
{
    std::ostringstream os;
    const std::size_t shown = cases.size() < limit ? cases.size() : limit;
    for (std::size_t i = 0; i < shown; ++i) {
        os << describe_mismatch(cases[i], actuals[i]);
    }
    if (cases.size() > shown) {
        os << "  ... and " << (cases.size() - shown) << " more\n";
    }
    return os.str();
}

}  // namespace

// 解析器自己也要可信：读错的用例必须报出来，否则「全通过」可能只是没读到
TEST(GraphemeTest, Uax29TestFileParser)
{
    GraphemeCase parsed;
    // 末尾的注释是「原样重复用例」，其中也有断点标记——必须被忽略
    EXPECT_EQ(parse_one("\xC3\xB7 0061 \xC3\x97 0301 \xC3\xB7 0062 \xC3\xB7\t"
                        "# \xC3\xB7 0061 \xC3\x97 0301 \xC3\xB7 0062 \xC3\xB7",
                        &parsed),
              kCaseParsed);
    ASSERT_EQ(parsed.code_points.size(), 3u);
    EXPECT_EQ(parsed.code_points[0], 0x61ul);
    EXPECT_EQ(parsed.code_points[1], 0x301ul);
    EXPECT_EQ(parsed.code_points[2], 0x62ul);
    EXPECT_EQ(parsed.text, std::string("a\xCC\x81") + "b");  // 3 字节 + 1 字节
    const std::size_t expected[] = {0, 3, 4};
    EXPECT_TRUE(parsed.expected_breaks ==
                std::vector<std::size_t>(expected, expected + 3));

    // 注释、空行、纯空白都不算用例
    EXPECT_EQ(parse_one("# Verifying UAX #29 rules"), kNotACase);
    EXPECT_EQ(parse_one(""), kNotACase);
    EXPECT_EQ(parse_one("   \t  "), kNotACase);

    // 残缺 / 越界 / 代理区输入必须报错，而不是被当成注释跳过
    EXPECT_EQ(parse_one("\xC3\xB7 0061 zz"), kCaseBroken);
    EXPECT_EQ(parse_one("\xC3\xB7 1 x"), kCaseBroken);
    EXPECT_EQ(parse_one("\xC3\xB7 110000"), kCaseBroken);
    EXPECT_EQ(parse_one("\xC3\xB7 D800"), kCaseBroken);
    EXPECT_EQ(parse_one("\xC3\xB7"), kCaseBroken);
}

// 官方用例集本身就是规格：每条用例的期望断点全部来自文件，逐条断言。
TEST(GraphemeTest, Uax29OfficialTestFile)
{
    using namespace libmini;

    const std::string path = LIBMINI_GRAPHEME_TEST_DATA;
    std::ifstream in(path.c_str(), std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "cannot open the official UAX #29 data file: "
                              << path;

    const std::string kVersionPrefix = "# GraphemeBreakTest-";
    std::vector<GraphemeCase> cases;
    std::vector<std::string> malformed;
    std::string data_version;
    std::size_t declared_lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);  // 容忍 CRLF 检出
        }
        if (line.compare(0, kVersionPrefix.size(), kVersionPrefix) == 0) {
            const std::size_t dot = line.find(".txt");
            if (dot != std::string::npos && dot > kVersionPrefix.size()) {
                data_version = line.substr(kVersionPrefix.size(),
                                           dot - kVersionPrefix.size());
            }
            continue;
        }
        if (line.compare(0, 8, "# Lines:") == 0) {
            declared_lines = static_cast<std::size_t>(
                std::strtoul(line.c_str() + 8, nullptr, 10));
            continue;
        }
        GraphemeCase parsed;
        switch (parse_case_line(line, parsed)) {
            case kCaseParsed:
                cases.push_back(parsed);
                break;
            case kCaseBroken:
                malformed.push_back(line);
                break;
            case kNotACase:
                break;
        }
    }

    EXPECT_TRUE(malformed.empty())
        << "could not parse " << malformed.size() << " line(s), first: "
        << (malformed.empty() ? std::string() : malformed[0]);
    ASSERT_FALSE(cases.empty()) << "no cases parsed from " << path;
    // 文件头声明了多少条，就必须真跑多少条
    if (declared_lines != 0) {
        EXPECT_EQ(cases.size(), declared_lines) << "official cases went missing";
    }

    // 逐条跑：断点集合 = 串首 + 每个允许断的位置 + 串尾
    std::vector<GraphemeCase> failed_cases;
    std::vector<std::vector<std::size_t> > failed_actual;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const GraphemeCase& c = cases[i];
        std::vector<std::size_t> actual;
        for (std::size_t pos = 0; pos < c.text.size();) {
            actual.push_back(pos);
            const std::size_t step = grapheme_cluster_step(c.text, pos);
            if (step == 0) {
                break;  // 防御：真发生了也不该变成死循环
            }
            pos += step;
        }
        if (actual.empty() || actual[actual.size() - 1] != c.text.size()) {
            actual.push_back(c.text.size());
        }
        if (actual == c.expected_breaks) {
            continue;
        }
        failed_cases.push_back(c);
        failed_actual.push_back(actual);
    }

    if (grapheme_full_conformance()) {
        EXPECT_TRUE(failed_cases.empty())
            << "the full UAX #29 tables must satisfy every official case, "
            << failed_cases.size() << " failed:\n"
            << summarize(failed_cases, failed_actual, 8);
    } else {
        // 启发式表只收 BMP 的 InCB / SpacingMark 属性（见
        // grapheme_tables_heuristic.inc 顶部说明），所以补充平面的 Indic 连字
        // 用例过不了。这不是放宽标准：除这一条之外任何偏差都算回归。
        const std::vector<unsigned long> known_gap = {0x11A3A, 0x11A0B};
        for (std::size_t i = 0; i < failed_cases.size(); ++i) {
            EXPECT_EQ(failed_cases[i].code_points, known_gap)
                << "heuristic tables deviate from the official file:\n"
                << describe_mismatch(failed_cases[i], failed_actual[i]);
        }
        // 已知缺口必须真的还在，免得表被悄悄换成完整表也当成通过
        EXPECT_FALSE(failed_cases.empty())
            << "heuristic tables now pass the whole official file? "
            << "update this expectation (and the README) accordingly";
    }

    // 表和用例集必须是同一版 Unicode，否则这个测试比的不是同一套数据
    ASSERT_FALSE(data_version.empty()) << "the data file has no version header";
    const std::string table_version = grapheme_data_version();
    EXPECT_EQ(table_version.compare(0, data_version.size(), data_version), 0)
        << "tables report \"" << table_version << "\" but the test data is Unicode "
        << data_version;
}

#endif  // LIBMINI_GRAPHEME_TEST_DATA

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

TEST(Base64UrlTest, RoundTripAndKnownVectors)
{
    using namespace libmini;
    // RFC 4648 §5 已知向量：默认无填充
    EXPECT_EQ(Base64Url::encode(""), "");
    EXPECT_EQ(Base64Url::encode("f"), "Zg");
    EXPECT_EQ(Base64Url::encode("fo"), "Zm8");
    EXPECT_EQ(Base64Url::encode("foo"), "Zm9v");
    EXPECT_EQ(Base64Url::encode("foobar"), "Zm9vYmFy");
    // 带填充与标准 Base64 一致（无 '+'/'/' 时）；字面量走 const char* 重载
    EXPECT_EQ(Base64Url::encode("f", true), "Zg==");    // 产生 url-safe 特有字符的输入：0xFB 0xFF 0xBF 标准为 "+/+/"，url-safe 为 "-_-_"
    const std::string bytes("\xFB\xFF\xBF", 3);
    const std::string b64 = Base64::encode(bytes);
    const std::string url = Base64Url::encode(bytes);
    EXPECT_EQ(b64, "+/+/" );
    EXPECT_NE(url, b64);
    EXPECT_EQ(url.find_first_of("+/"), std::string::npos);
    EXPECT_NE(url.find_first_of('-'), std::string::npos);
    EXPECT_NE(url.find_first_of('_'), std::string::npos);
    EXPECT_EQ(url, "-_-_" );

    // 无填充解码
    std::string out;
    ASSERT_TRUE(Base64Url::decode("Zm9vYmFy", out));
    EXPECT_EQ(out, "foobar");
    ASSERT_TRUE(Base64Url::decode("Zg", out));              // 无填充
    EXPECT_EQ(out, "f");
    ASSERT_TRUE(Base64Url::decode("Zg==", out));            // 带填充也接受
    EXPECT_EQ(out, "f");
    // url-safe 字符
    ASSERT_TRUE(Base64Url::decode("-_-_", out));
    EXPECT_EQ(out, bytes);

    // 往返（含二进制）
    std::string binary;
    for (int i = 0; i < 256; ++i) {
        binary += static_cast<char>(i);
    }
    ASSERT_TRUE(Base64Url::decode(Base64Url::encode(binary), out));
    EXPECT_EQ(out, binary);

    // 非法输入
    std::string bad;
    EXPECT_FALSE(Base64Url::decode("Zm9v!", bad));   // 非法字符
    EXPECT_FALSE(Base64Url::decode("Z", bad));       // 4k+1 长度
    EXPECT_FALSE(Base64Url::decode("Zm=v", bad));    // '=' 不在末尾
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

// -------------------------------- charset --------------------------------

namespace {

// 用字节值拼接字符串：十六进制转义（\x87 之类）与相邻十六进制字符极易看错
std::string byte_string(std::initializer_list<int> values)
{
    std::string out;
    for (int v : values) {
        out.push_back(static_cast<char>(v));
    }
    return out;
}

// 精简 libc（如 musl）可能没带 GBK 模块，相关用例整体跳过
bool gbk_supported()
{
    return libmini::CharsetConverter::is_supported(libmini::Charset::Utf8,
                                                   libmini::Charset::Gbk);
}

}  // namespace

TEST(CharsetTest, Utf8ToUtf16KnownBytes)
{
    using namespace libmini;
    const std::string utf8 = byte_string({0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87});

    std::string out;
    ASSERT_EQ(CharsetConverter::convert(utf8, Charset::Utf8, Charset::Utf16Le, out),
              CharsetStatus::Ok);
    EXPECT_EQ(out, byte_string({0x2D, 0x4E, 0x87, 0x65}));  // 中文（小端）

    ASSERT_EQ(CharsetConverter::convert(utf8, Charset::Utf8, Charset::Utf16Be, out),
              CharsetStatus::Ok);
    EXPECT_EQ(out, byte_string({0x4E, 0x2D, 0x65, 0x87}));  // 中文（大端）
}

TEST(CharsetTest, Utf16EndiannessSwap)
{
    using namespace libmini;
    const std::string le = byte_string({0x2D, 0x4E, 0x87, 0x65});

    std::string out;
    ASSERT_EQ(CharsetConverter::convert(le, Charset::Utf16Le, Charset::Utf16Be, out),
              CharsetStatus::Ok);
    EXPECT_EQ(out, byte_string({0x4E, 0x2D, 0x65, 0x87}));

    ASSERT_EQ(CharsetConverter::convert(out, Charset::Utf16Be, Charset::Utf16Le, out),
              CharsetStatus::Ok);
    EXPECT_EQ(out, le);
}

TEST(CharsetTest, GbkRoundTripThroughUtf8)
{
    using namespace libmini;
    if (!gbk_supported()) {
        GTEST_SKIP() << "GBK not available on this platform";
    }
    const std::string gbk = byte_string({0xD6, 0xD0, 0xCE, 0xC4});
    const std::string utf8 = byte_string({0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87});

    bool ok = false;
    EXPECT_EQ(CharsetConverter::to_utf8(gbk, Charset::Gbk, &ok), utf8);
    EXPECT_TRUE(ok);

    ok = false;
    EXPECT_EQ(CharsetConverter::from_utf8(utf8, Charset::Gbk, &ok), gbk);
    EXPECT_TRUE(ok);

    // 经 UTF-16 中转应与直转结果一致
    std::string via_utf16;
    ASSERT_EQ(CharsetConverter::convert(gbk, Charset::Gbk, Charset::Utf16Le, via_utf16),
              CharsetStatus::Ok);
    std::string back;
    ASSERT_EQ(CharsetConverter::convert(via_utf16, Charset::Utf16Le, Charset::Gbk, back),
              CharsetStatus::Ok);
    EXPECT_EQ(back, gbk);
}

TEST(CharsetTest, SameCharsetIsPassthrough)
{
    using namespace libmini;
    const std::string raw = byte_string({0x00, 0xFF, 0x80});  // 非法 UTF-8 也原样返回
    std::string out;
    EXPECT_EQ(CharsetConverter::convert(raw, Charset::Utf8, Charset::Utf8, out),
              CharsetStatus::Ok);
    EXPECT_EQ(out, raw);
}

TEST(CharsetTest, RejectsInvalidInput)
{
    using namespace libmini;
    std::string out;

    // UTF-8：0xFF 不是合法首字节；0xE4 0xB8 是被截断的三字节序列
    EXPECT_EQ(CharsetConverter::convert(byte_string({0xFF}), Charset::Utf8,
                                        Charset::Utf16Le, out),
              CharsetStatus::InvalidSequence);
    EXPECT_EQ(CharsetConverter::convert(byte_string({0xE4, 0xB8}), Charset::Utf8,
                                        Charset::Utf16Le, out),
              CharsetStatus::InvalidSequence);

    // UTF-16：奇数长度、孤立高代理项
    EXPECT_EQ(CharsetConverter::convert(byte_string({0x2D}), Charset::Utf16Le,
                                        Charset::Utf8, out),
              CharsetStatus::InvalidSequence);
    EXPECT_EQ(CharsetConverter::convert(byte_string({0x00, 0xD8}), Charset::Utf16Le,
                                        Charset::Utf8, out),
              CharsetStatus::InvalidSequence);
    EXPECT_EQ(CharsetConverter::convert(byte_string({0xD8, 0x00}), Charset::Utf16Be,
                                        Charset::Utf8, out),
              CharsetStatus::InvalidSequence);

    if (gbk_supported()) {
        // GBK：半截双字节序列
        EXPECT_EQ(CharsetConverter::convert(byte_string({0xD6}), Charset::Gbk,
                                            Charset::Utf8, out),
                  CharsetStatus::InvalidSequence);
        // 目标编码装不下：U+1F600 不在 GBK 里，不得静默替换成 '?'
        EXPECT_EQ(CharsetConverter::convert(byte_string({0xF0, 0x9F, 0x98, 0x80}),
                                            Charset::Utf8, Charset::Gbk, out),
                  CharsetStatus::InvalidSequence);
    }
}

TEST(CharsetTest, OutputUntouchedOnFailure)
{
    using namespace libmini;
    std::string out = "keep-me";
    EXPECT_EQ(CharsetConverter::convert(byte_string({0xFF}), Charset::Utf8,
                                        Charset::Utf16Le, out),
              CharsetStatus::InvalidSequence);
    EXPECT_EQ(out, "keep-me");
}

TEST(CharsetTest, EmptyInputAndNames)
{
    using namespace libmini;
    std::string out;
    EXPECT_EQ(CharsetConverter::convert(std::string(), Charset::Utf8, Charset::Gbk, out),
              CharsetStatus::Ok);
    EXPECT_TRUE(out.empty());

    EXPECT_STREQ(CharsetConverter::name(Charset::Gbk), "GBK");
    EXPECT_STREQ(CharsetConverter::name(Charset::Utf16Be), "UTF-16BE");
    EXPECT_TRUE(CharsetConverter::is_supported(Charset::Utf8, Charset::Utf16Le));
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
    // 注意求值顺序：elapsed_ns() 与 elapsed_us() 各自独立取当前时钟，
    // 若在同一断言里写 EXPECT_GE(elapsed_ns(), elapsed_us()*1000)，
    // gtest 宏对两实参的求值顺序不确定——ns 先读、us 后读时，µs 截断值
    // ×1000 可能反超 ns 读数（慢机上差几十纳秒即翻转）。固定顺序预读：
    // 先 us 后 ns，后读时间恒 >= 前读，比较才确定成立。
    const std::int64_t us = sw.elapsed_us();
    EXPECT_GE(sw.elapsed_ns(), us * 1000);
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

    // 清零验证走确定性路径：记下 restart 前的累计读数（≥ first+30 ≈ 90ms），
    // restart 后立即暂停读数——读数只含几条指令的耗时，与累计值相差一个
    // 量级以上。不依赖 sleep 的上界：慢机上过冲只会让累计值更大，断言自愈
    // （旧写法睡 15ms 后断言 < first≈60ms，sleep 过冲越线即误报，macOS CI 实测 69 vs 61）
    const std::int64_t acc = sw.elapsed_ms();
    sw.restart();
    EXPECT_TRUE(sw.is_running());
    sw.pause();
    EXPECT_LT(sw.elapsed_ms(), acc / 2);  // 已清零重新计时
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
