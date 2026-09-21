#include "base32.h"

#include <cctype>

namespace libmini {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

// 反查表：字符 → 5-bit 值；-1 = 非法
int decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a';
    }
    if (c >= '2' && c <= '7') {
        return c - '2' + 26;
    }
    return -1;
}

}  // namespace

std::string Base32::encode(const void* data, std::size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((size * 8 + 4) / 5 + 4);

    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < size; ++i) {
        buffer = (buffer << 8) | bytes[i];
        bits += 8;
        while (bits >= 5) {
            out.push_back(kAlphabet[(buffer >> (bits - 5)) & 0x1f]);
            bits -= 5;
        }
    }
    if (bits > 0) {
        out.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1f]);
    }
    // RFC 4648：输出长度补齐到 8 的倍数
    while (out.size() % 8 != 0) {
        out.push_back('=');
    }
    return out;
}

std::string Base32::encode(const std::string& raw)
{
    return encode(raw.data(), raw.size());
}

bool Base32::decode(const std::string& text, std::string& out, bool strict)
{
    out.clear();
    std::uint32_t buffer = 0;
    int bits = 0;
    int padding = 0;

    for (const char c : text) {
        if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (c == '=') {
                ++padding;
                if (padding > 6) {
                    return false;  // 填充过多
                }
            } else if (strict) {
                return false;  // 严格模式不接受空白
            }
            continue;
        }
        const int v = decode_char(c);
        if (v < 0) {
            if (strict) {
                return false;
            }
            continue;  // 宽容模式跳过
        }
        if (padding > 0) {
            if (strict) {
                return false;  // 填充后还有数据
            }
            padding = 0;  // 宽容模式允许继续
        }
        buffer = (buffer << 5) | static_cast<std::uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<char>((buffer >> (bits - 8)) & 0xff));
            bits -= 8;
        }
    }
    // 严格模式：剩余 bits 必须全为 0（否则编码不是合法的 Base32）
    if (strict && bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        return false;
    }
    return true;
}

}  // namespace libmini
