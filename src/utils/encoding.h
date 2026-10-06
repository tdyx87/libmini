#ifndef LIBMINI_ENCODING_H
#define LIBMINI_ENCODING_H

#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// Base64 编解码（对应 boost::beast::detail::base64 / commons codec）。
class LIBMINI_API Base64 {
public:
    // 编码；解码失败（非法字符/长度错误）返回 false
    static std::string encode(const std::string& raw);
    static std::string encode(const void* data, std::size_t size);
    static bool decode(const std::string& text, std::string& out);
};

// Base64url 编解码（RFC 4648 §5）：字母表用 '-'/'_' 替代 '+'/'/'，
// 无需转义即可放入 URL、JWT、文件名。默认不带 '=' 填充（JWT 惯例）。
//
//   std::string t = Base64Url::encode(raw);        // 无填充
//   std::string p = Base64Url::encode(raw, true);  // 带 '=' 填充
//   Base64Url::decode(t, out);                     // 填充可有可无
class LIBMINI_API Base64Url {
public:
    // 编码；padded=false（默认）省略 '=' 填充
    static std::string encode(const std::string& raw, bool padded = false);
    static std::string encode(const void* data, std::size_t size,
                              bool padded = false);
    // 字面量专用重载：避免 const char* 同时匹配上面两个重载的歧义
    //（按 ISO 规则 encode("f", true) 是 ambiguous，此重载令其确定无歧义）
    static std::string encode(const char* raw, bool padded = false);
    // 解码；同时接受 url-safe 与标准字母表、带或不带填充。
    // 非法字符/长度错误返回 false
    static bool decode(const std::string& text, std::string& out);
};

// 十六进制编解码
class LIBMINI_API Hex {
public:
    // 大写输出（如 "DEADBEEF"）；lower_case=true 时输出小写
    static std::string encode(const std::string& raw, bool lower_case = false);
    static std::string encode(const void* data, std::size_t size,
                              bool lower_case = false);
    // 解码；非法输入（奇数长度/非十六进制字符）返回 false
    static bool decode(const std::string& text, std::string& out);
};

// URL 编解码（application/x-www-form-urlencoded，空格→'+'）。
class LIBMINI_API UrlEncode {
public:
    // 保留字符集：字母数字与 -_.~（RFC 3986 unreserved）
    static std::string encode(const std::string& raw);
    // '+'→空格，%XX 十六进制转义还原；非法转义序列原样保留
    static std::string decode(const std::string& text);
};

}  // namespace libmini

#endif  // LIBMINI_ENCODING_H
