#ifndef LIBMINI_STRING_ALGO_H
#define LIBMINI_STRING_ALGO_H

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

}  // namespace libmini

#endif  // LIBMINI_STRING_ALGO_H
