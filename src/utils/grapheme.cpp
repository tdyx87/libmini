#include "grapheme.h"

#include <cstddef>
#include <cstdint>

#include "utf8_codec.h"

namespace libmini {

namespace {

// ------------------ 属性类别 ------------------

// Grapheme_Cluster_Break（UAX #29 表 3）。数值顺序无关紧要，只用等值比较。
enum GcbClass : std::uint8_t {
    kGcbOther = 0,
    kGcbCR,
    kGcbLF,
    kGcbControl,
    kGcbExtend,
    kGcbZWJ,
    kGcbRegionalIndicator,
    kGcbPrepend,
    kGcbSpacingMark,
    kGcbL,
    kGcbV,
    kGcbT,
    kGcbLV,
    kGcbLVT,
};

// Indic_Conjunct_Break（Unicode 15.1 起，GB9c 规则用）
enum IncbClass : std::uint8_t {
    kIncbNone = 0,
    kIncbConsonant,
    kIncbExtend,
    kIncbLinker,
};

// 属性表的一行：闭区间 [lo, hi] 属于 value 类别。表按 lo 升序、互不重叠。
struct GraphemeRange {
    std::uint32_t lo;
    std::uint32_t hi;
    std::uint8_t value;
};

// 两张表二选一：默认是人工整理的启发式表（不依赖生成器），打开
// LIBMINI_UNICODE_FULL_GRAPHEME 后换成 ci/gen_grapheme_tables.py 从 UCD
// 生成的完整表。两者都定义同名数组（kGcbRanges / kExtendedPictographicRanges
// / kIncbRanges 及各自的 Count），于是下面的扫描逻辑完全共享。
#if defined(LIBMINI_UNICODE_FULL_GRAPHEME)
#include "grapheme_tables_full.inc"
#else
#include "grapheme_tables_heuristic.inc"
#endif

std::uint8_t lookup(const GraphemeRange* table, std::size_t count,
                    std::uint32_t code_point)
{
    std::size_t lo = 0;
    std::size_t hi = count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (code_point < table[mid].lo) {
            hi = mid;
        } else if (code_point > table[mid].hi) {
            lo = mid + 1;
        } else {
            return table[mid].value;
        }
    }
    return 0;
}

GcbClass gcb_class(std::uint32_t code_point)
{
    // Hangul 音节块（U+AC00~U+D7A3）：LV / LVT 可由码点精确算出（UAX #29 对
    // GCB=LV/LVT 的定义就是「(码点-AC00) 能否被 28 整除」），因此不进表——
    // 这一条就省掉了近 800 个区间。
    if (code_point >= 0xAC00 && code_point <= 0xD7A3) {
        return ((code_point - 0xAC00) % 28 == 0) ? kGcbLV : kGcbLVT;
    }
    return static_cast<GcbClass>(
        lookup(kGcbRanges, kGcbRangesCount, code_point));
}

bool is_extended_pictographic(std::uint32_t code_point)
{
    return lookup(kExtendedPictographicRanges, kExtendedPictographicRangesCount,
                  code_point) != 0;
}

IncbClass incb_class(std::uint32_t code_point)
{
    return static_cast<IncbClass>(
        lookup(kIncbRanges, kIncbRangesCount, code_point));
}

// ------------------ 边界判定的三个「记忆」 ------------------
//
// UAX #29 的规则要看的不是孤立的一对字符，而是「已消费部分」的几种形态：
//   * GB9c 需要知道「\p{InCB=Linker} \p{InCB=Extend}*」（注意不含前导辅音），
//   * GB11 需要知道「ExtPict Extend* ZWJ」，
//   * GB12/GB13 需要知道紧邻前面连续出现了几个区域指示符。
// 三个小自动机各自增量维护这三点。

// GB9c：已消费部分是否以「Linker Extend*」结尾（Linker 本身可以是 Extend）
void advance_incb(IncbClass klass, bool& linker_run)
{
    if (klass == kIncbLinker) {
        linker_run = true;  // 新的 Linker 重新开始一段
    } else if (klass != kIncbExtend) {
        linker_run = false;  // 只有 Extend 能延续
    }
}

// GB11 的状态：1 = 结尾是 ExtPict（可接 Extend*），2 = 结尾是 …ZWJ，0 = 其它
void advance_extpict(std::uint32_t code_point, GcbClass klass, int& state)
{
    if (is_extended_pictographic(code_point)) {
        state = 1;
    } else if (klass == kGcbExtend) {
        state = (state == 1) ? 1 : 0;
    } else if (klass == kGcbZWJ) {
        state = (state == 1) ? 2 : 0;
    } else {
        state = 0;
    }
}

// 判断 prev 与 next 之间是否存在字形簇边界。规则顺序即优先级：先匹配的先赢。
bool is_grapheme_break(GcbClass prev, GcbClass next, std::uint32_t next_cp,
                       bool incb_linker_run, int extpict_state,
                       std::size_t ri_run)
{
    if (prev == kGcbCR && next == kGcbLF) {
        return false;  // GB3
    }
    if (prev == kGcbCR || prev == kGcbLF || prev == kGcbControl) {
        return true;  // GB4
    }
    if (next == kGcbCR || next == kGcbLF || next == kGcbControl) {
        return true;  // GB5
    }
    if (prev == kGcbL &&
        (next == kGcbL || next == kGcbV || next == kGcbLV || next == kGcbLVT)) {
        return false;  // GB6
    }
    if ((prev == kGcbLV || prev == kGcbV) &&
        (next == kGcbV || next == kGcbT)) {
        return false;  // GB7
    }
    if ((prev == kGcbLVT || prev == kGcbT) && next == kGcbT) {
        return false;  // GB8
    }
    if (next == kGcbExtend || next == kGcbZWJ) {
        return false;  // GB9
    }
    if (next == kGcbSpacingMark) {
        return false;  // GB9a
    }
    if (prev == kGcbPrepend) {
        return false;  // GB9b
    }
    if (incb_linker_run && incb_class(next_cp) == kIncbConsonant) {
        return false;  // GB9c：Linker Extend* × Consonant（Indic 连字）
    }
    if (extpict_state == 2 && is_extended_pictographic(next_cp)) {
        return false;  // GB11：emoji ZWJ 序列不拆
    }
    // GB12 / GB13：两个区域指示符成对（国旗）时不拆——前面的连续个数是奇数
    if (prev == kGcbRegionalIndicator && next == kGcbRegionalIndicator &&
        (ri_run % 2) == 1) {
        return false;
    }
    return true;  // GB999：其余一律断开（每个字符自成一簇）
}

}  // namespace

std::size_t grapheme_cluster_step(const std::string& str, std::size_t pos)
{
    if (pos >= str.size()) {
        return 0;
    }
    std::uint32_t code_point = 0;
    const std::size_t length = utf8_detail::decode(str, pos, code_point);
    if (length == 0) {
        return 1;  // 非法字节：单独成一个「簇」，与 utf8_* 的容错步长一致
    }

    GcbClass prev_class = gcb_class(code_point);
    int extpict_state = 0;
    advance_extpict(code_point, prev_class, extpict_state);
    bool incb_linker_run = false;
    advance_incb(incb_class(code_point), incb_linker_run);
    std::size_t ri_run = (prev_class == kGcbRegionalIndicator) ? 1u : 0u;

    std::size_t cursor = pos + length;
    while (cursor < str.size()) {
        std::uint32_t next_cp = 0;
        const std::size_t next_len = utf8_detail::decode(str, cursor, next_cp);
        if (next_len == 0) {
            break;  // 非法字节不属于当前簇，留给下一个簇（那里会按 1 前进）
        }
        const GcbClass next_class = gcb_class(next_cp);
        if (is_grapheme_break(prev_class, next_class, next_cp, incb_linker_run,
                              extpict_state, ri_run)) {
            break;
        }

        advance_extpict(next_cp, next_class, extpict_state);
        advance_incb(incb_class(next_cp), incb_linker_run);
        ri_run = (next_class == kGcbRegionalIndicator) ? ri_run + 1 : 0;
        prev_class = next_class;
        cursor += next_len;
    }
    return cursor - pos;
}

std::size_t grapheme_cluster_count(const std::string& str)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < str.size()) {
        const std::size_t step = grapheme_cluster_step(str, pos);
        pos += (step == 0) ? 1 : step;  // step 不会为 0，防御性写法
        ++count;
    }
    return count;
}

std::size_t grapheme_byte_offset(const std::string& str, std::size_t index)
{
    std::size_t pos = 0;
    std::size_t count = 0;
    while (pos < str.size() && count < index) {
        pos += grapheme_cluster_step(str, pos);
        ++count;
    }
    if (count < index) {
        return std::string::npos;  // 簇数不足
    }
    return pos;
}

bool grapheme_full_conformance()
{
#if defined(LIBMINI_UNICODE_FULL_GRAPHEME)
    return true;
#else
    return false;
#endif
}

const char* grapheme_data_version()
{
    // 两个 .inc 都定义同名的 kGraphemeTableUnicodeVersion
    return kGraphemeTableUnicodeVersion;
}

}  // namespace libmini
