#ifndef LIBMINI_UTF8_CODEC_H
#define LIBMINI_UTF8_CODEC_H

// 内部共享的 UTF-8 解码工具（不导出、不装进公共头文件）。
//
// 字形簇分割（grapheme.cpp）与 UTF-8 感知字符串操作（string_algo.cpp）都要
// 按码点前进，且对非法输入的处理必须完全一致：把它放在一个地方，就不会出现
// 「string_algo 认为这是 1 个码点、grapheme 认为这是 1 个字形簇」这类漂移。

#include <cstddef>
#include <cstdint>
#include <string>

namespace libmini {
namespace utf8_detail {

// 严格解码 pos 处的 UTF-8 字符：成功返回占用的字节数（1~4）并写入码点；
// 非法（非法首/续字节、被截断、过长编码、代理项、超出 U+10FFFF）返回 0。
// 调用方必须保证 pos < str.size()。
inline std::size_t decode(const std::string& str, std::size_t pos,
                          std::uint32_t& code_point)
{
    const unsigned char first = static_cast<unsigned char>(str[pos]);
    if (first < 0x80) {
        code_point = first;
        return 1;
    }

    std::size_t length = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07u;
    } else {
        return 0;  // 0x80~0xBF（孤立续字节）或 0xF8~0xFF（非法首字节）
    }
    if (str.size() - pos < length) {
        return 0;  // 末尾被截断
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char cont = static_cast<unsigned char>(str[pos + i]);
        if ((cont & 0xC0) != 0x80) {
            return 0;  // 续字节格式错误
        }
        value = (value << 6) | (cont & 0x3Fu);
    }
    // 过长编码下限：同样的码点必须用最短形式编码（入参是否合法的重要判据）
    static const std::uint32_t kMinCodePoint[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < kMinCodePoint[length] || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        return 0;
    }
    code_point = value;
    return length;
}

// 容错步长：合法字符取其字节长度，非法字节按 1 前进
//（不丢字节，也不会因一个坏字节让后面的位置整体错位）
inline std::size_t step(const std::string& str, std::size_t pos)
{
    std::uint32_t code_point = 0;
    const std::size_t length = decode(str, pos, code_point);
    return length == 0 ? 1 : length;
}

}  // namespace utf8_detail
}  // namespace libmini

#endif  // LIBMINI_UTF8_CODEC_H
