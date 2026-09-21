#include "encoding.h"

#include <cstdint>

namespace libmini {

namespace {

const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline bool is_base64_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
}

inline int base64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool is_hex_char(unsigned char c)
{
    return hex_value(c) >= 0;
}

inline char to_hex_digit(unsigned char v, bool lower_case)
{
    static const char kUpper[] = "0123456789ABCDEF";
    static const char kLower[] = "0123456789abcdef";
    return lower_case ? kLower[v & 0x0F] : kUpper[v & 0x0F];
}

inline bool is_url_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '~';
}

}  // namespace

// ------------------------------ Base64 ------------------------------

std::string Base64::encode(const void* data, std::size_t size)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(((size + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(p[i]) << 16) |
                                (static_cast<std::uint32_t>(p[i + 1]) << 8) |
                                static_cast<std::uint32_t>(p[i + 2]);
        out += kBase64Chars[(n >> 18) & 0x3F];
        out += kBase64Chars[(n >> 12) & 0x3F];
        out += kBase64Chars[(n >> 6) & 0x3F];
        out += kBase64Chars[n & 0x3F];
    }
    const std::size_t rem = size - i;
    if (rem == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(p[i]) << 16;
        out += kBase64Chars[(n >> 18) & 0x3F];
        out += kBase64Chars[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const std::uint32_t n = (static_cast<std::uint32_t>(p[i]) << 16) |
                                (static_cast<std::uint32_t>(p[i + 1]) << 8);
        out += kBase64Chars[(n >> 18) & 0x3F];
        out += kBase64Chars[(n >> 12) & 0x3F];
        out += kBase64Chars[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::string Base64::encode(const std::string& raw)
{
    return encode(raw.data(), raw.size());
}

bool Base64::decode(const std::string& text, std::string& out)
{
    out.clear();

    // 尾部 '=' 只允许出现在末尾（0、1 或 2 个）
    std::size_t len = text.size();
    std::size_t pad = 0;
    while (len > 0 && text[len - 1] == '=') {
        ++pad;
        --len;
    }
    if (pad > 2 || (len + pad) % 4 != 0) {
        return false;
    }

    std::uint32_t buf = 0;
    int bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const int v = base64_value(static_cast<unsigned char>(text[i]));
        if (v < 0) {
            return false;
        }
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    // 被丢弃的尾部位必须为 0（严格模式）
    if ((buf & ((1u << bits) - 1)) != 0) {
        return false;
    }
    return true;
}

// ------------------------------ Hex ---------------------------------

std::string Hex::encode(const void* data, std::size_t size, bool lower_case)
{
    static const char* kUpper = "0123456789ABCDEF";
    static const char* kLower = "0123456789abcdef";
    const char* digits = lower_case ? kLower : kUpper;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[p[i] >> 4];
        out += digits[p[i] & 0x0F];
    }
    return out;
}

std::string Hex::encode(const std::string& raw, bool lower_case)
{
    return encode(raw.data(), raw.size(), lower_case);
}

bool Hex::decode(const std::string& text, std::string& out)
{
    if (text.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hex_value(static_cast<unsigned char>(text[i]));
        const int lo = hex_value(static_cast<unsigned char>(text[i + 1]));
        if (hi < 0 || lo < 0) {
            return false;
        }
        out += static_cast<char>((hi << 4) | lo);
    }
    return true;
}

// --------------------------- UrlEncode -------------------------------

std::string UrlEncode::encode(const std::string& raw)
{
    static const char* kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == ' ') {
            out += '+';  // application/x-www-form-urlencoded 约定
        } else if (is_url_unreserved(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kDigits[c >> 4];
            out += kDigits[c & 0x0F];
        }
    }
    return out;
}

std::string UrlEncode::decode(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size() &&
                   is_hex_char(static_cast<unsigned char>(text[i + 1])) &&
                   is_hex_char(static_cast<unsigned char>(text[i + 2]))) {
            const int hi = hex_value(static_cast<unsigned char>(text[i + 1]));
            const int lo = hex_value(static_cast<unsigned char>(text[i + 2]));
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

}  // namespace libmini
