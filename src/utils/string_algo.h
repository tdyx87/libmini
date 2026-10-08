#ifndef LIBMINI_STRING_ALGO_H
#define LIBMINI_STRING_ALGO_H

#include <cstddef>
#include <string>
#include <vector>

#include "libmini.h"
#include "export.h"

namespace libmini {

// ------------------ boost::algorithm::string 风格的扩展 ------------------

// 是否以 prefix 开头 / 以 suffix 结尾
LIBMINI_API bool starts_with(const std::string& str, const std::string& prefix);
LIBMINI_API bool ends_with(const std::string& str, const std::string& suffix);

// 包含子串
LIBMINI_API bool contains(const std::string& str, const std::string& needle);

// 大小写不敏感比较
LIBMINI_API bool iequals(const std::string& a, const std::string& b);

// 截掉前缀/后缀（不匹配时原样返回）
LIBMINI_API std::string trim_prefix(const std::string& str, const std::string& prefix);
LIBMINI_API std::string trim_suffix(const std::string& str, const std::string& suffix);

// 只保留第一次出现的替换 / 全部替换
LIBMINI_API std::string replace_first(const std::string& str, const std::string& from,
                          const std::string& to);
LIBMINI_API std::string replace_all(const std::string& str, const std::string& from,
                        const std::string& to);

// 以任意空白切分（连续空白算一个分隔符，自动去空 token）
LIBMINI_API std::vector<std::string> split_whitespace(const std::string& str);

// 用字符串分隔符切分（保留空 token；skip_empty=true 时去掉空 token）
LIBMINI_API std::vector<std::string> split_string(const std::string& str,
                                      const std::string& delimiter,
                                      bool skip_empty = false);

// 用分隔符连接
LIBMINI_API std::string join(const std::vector<std::string>& parts,
                 const std::string& delimiter);

// ------------------ UTF-8 感知操作 ------------------
//
// 下面这组函数以「码点」（Unicode 码位）而不是字节为单位工作，因此按字符
// 计数、截断、取子串时绝不会把多字节序列切成两半。UTF-8 的前缀性质保证了：
// 只要切在码点边界上，合法输入的输出必然仍是合法 UTF-8。
//
// 非法输入（含被截断的序列）的处理：把每个非法字节当作一个「字符」继续
// 前进——既不丢字节，也不会因为一个坏字节让后面的位置整体错位。要严格
// 拒绝非法输入，请先用 utf8_is_valid() 判定。

// 是否为合法 UTF-8：拒绝过长编码（overlong）、UTF-16 代理项区间
// （U+D800~U+DFFF）、超出 U+10FFFF 的码点，以及被截断的多字节序列
LIBMINI_API bool utf8_is_valid(const std::string& str);

// 码点个数（即界面上说的「几个字」），不是字节数
LIBMINI_API std::size_t utf8_length(const std::string& str);

// 第 index 个码点（0 起）的起始字节偏移；index == 字符数时返回 size()，
// 越界返回 std::string::npos。用于把字符位置映射回字节级 API
LIBMINI_API std::size_t utf8_byte_offset(const std::string& str, std::size_t index);

// 从第 index 个码点起取 count 个码点；count == std::string::npos 表示到末尾。
// index 越界返回空串
LIBMINI_API std::string utf8_substr(const std::string& str, std::size_t index,
                                    std::size_t count = std::string::npos);

// Python 风格切片：取码点区间 [begin, end)；end == npos 表示到末尾
LIBMINI_API std::string utf8_slice(const std::string& str, std::size_t begin,
                                   std::size_t end);

// 截到最多 max_chars 个码点（超出部分整体丢弃）
LIBMINI_API std::string utf8_truncate(const std::string& str, std::size_t max_chars);

// 截到最多 max_bytes 字节，且不切坏多字节序列——结果可能比 max_bytes 更短，
// 但一定落在码点边界上。用于按字节长度受限的场景：DB 列、协议字段
LIBMINI_API std::string utf8_truncate_bytes(const std::string& str, std::size_t max_bytes);

// 尾部保留最多 max_chars 个码点（日志/摘要里保留结尾比保留开头更常见）
LIBMINI_API std::string utf8_tail(const std::string& str, std::size_t max_chars);

}  // namespace libmini

#endif  // LIBMINI_STRING_ALGO_H
