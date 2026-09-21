#ifndef LIBMINI_STRING_UTILS_H
#define LIBMINI_STRING_UTILS_H

#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 字符串分割
LIBMINI_API std::vector<std::string> split(const std::string& str, char delimiter);

// 字符串修剪
LIBMINI_API std::string trim(const std::string& str);

// 字符串替换
LIBMINI_API std::string replace(const std::string& str, const std::string& from, const std::string& to);

// 转换为大写
LIBMINI_API std::string to_upper(const std::string& str);

// 转换为小写
LIBMINI_API std::string to_lower(const std::string& str);

}

#endif // LIBMINI_STRING_UTILS_H