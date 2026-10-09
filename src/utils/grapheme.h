#ifndef LIBMINI_GRAPHEME_H
#define LIBMINI_GRAPHEME_H

#include <cstddef>
#include <string>

#include "libmini.h"
#include "export.h"

namespace libmini {

// ------------------ 字形簇（UAX #29 extended grapheme cluster） ------------------
//
// 字形簇是「用户感知的一个字符」：'e' 后面跟的组合重音算一个，emoji ZWJ 序列
// （👨‍👩‍👧）算一个，国旗（两个区域指示符）算一个，Hangul 音节算一个。
//
// 按码点截断会把这些拆散——结果仍是合法 UTF-8（没切坏字节序列），但视觉上
// 变成「半个字符」：组合重音会落到下一个字符上，emoji 家庭会散成几个人。
// 本模块提供字形簇级的长度 / 偏移，string_algo 里的 utf8_grapheme_* 系列
// 在此之上实现字形簇级的截断与取子串。

// 从 pos 处起的字形簇占用的字节数（至少 1；非法字节按 1 前进）。
// pos >= str.size() 时返回 0。
LIBMINI_API std::size_t grapheme_cluster_step(const std::string& str,
                                              std::size_t pos);

// 字形簇个数（界面上的「几个字」；对比 utf8_length 的码点个数）
LIBMINI_API std::size_t grapheme_cluster_count(const std::string& str);

// 第 index 个字形簇的起始字节偏移（0 起）。index == 簇数时返回 size()，
// 越界返回 std::string::npos
LIBMINI_API std::size_t grapheme_byte_offset(const std::string& str,
                                             std::size_t index);

// 当前是否使用完整的 UAX #29 实现。false 表示走内置的启发式属性表
//（覆盖主要文字，见 grapheme_tables_heuristic.inc 顶部说明），
// true 表示由 -DLIBMINI_UNICODE_FULL_GRAPHEME=ON 切换到 UCD 生成的完整表。
LIBMINI_API bool grapheme_full_conformance();

// 属性表来源说明。完整表返回生成时使用的 Unicode 版本（如 "Unicode 18.0.0"），
// 启发式返回说明串。用于日志、诊断与「同一份文本在不同构建下结果不同」的排查。
LIBMINI_API const char* grapheme_data_version();

}  // namespace libmini

#endif  // LIBMINI_GRAPHEME_H
