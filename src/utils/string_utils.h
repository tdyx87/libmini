#ifndef LIBMINI_STRING_UTILS_H
#define LIBMINI_STRING_UTILS_H

#include <cstdint>
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

// ---------------- 日志可读性格式化 ----------------

// 字节数的人类可读形式（1024 进制，两位小数，末位单位去尾零）：
//   0 → "0 B"；1536 → "1.5 KB"；1048576 → "1 MB"；
//   负值带负号；≥1 EB 按字节原样输出
LIBMINI_API std::string format_bytes(std::int64_t bytes);

// 时长的人类可读形式（自动选择单位，两位小数，末位单位去尾零）：
//   < 1s 按毫秒（"15 ms"、"1.5 ms"）；< 60s 按秒（"2.5 s"）；
//   < 1h 按 "Xm Y.Y s"（"1m 05.5 s"）；其余按 "Xh Ym Zs"
//   负值带负号
LIBMINI_API std::string format_duration_ms(std::int64_t ms);

}

#endif // LIBMINI_STRING_UTILS_H