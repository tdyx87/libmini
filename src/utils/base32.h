#ifndef LIBMINI_BASE32_H
#define LIBMINI_BASE32_H

#include <string>

#include "libmini.h"

namespace libmini {

// Base32 编解码（RFC 4648 §6：A-Z2-7，'=' 填充）。
// 用途：TOTP 两步验证密钥、生成适合人工抄写/口述的短 ID。
class LIBMINI_API Base32 {
public:
    // RFC 4648 标准编码（含 '=' 填充，输出大写）
    static std::string encode(const std::string& raw);
    static std::string encode(const void* data, std::size_t size);

    // 解码：接受大小写、忽略空白；strict=true 时拒绝非法字符与
    // 非法填充（编码输出可无损往返）；解码失败返回 false
    static bool decode(const std::string& text, std::string& out,
                       bool strict = false);
};

}  // namespace libmini

#endif  // LIBMINI_BASE32_H
