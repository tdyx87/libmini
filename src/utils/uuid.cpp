#include "uuid.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>

namespace libmini {

Uuid Uuid::generate()
{
    // mt19937_64 不可拷贝；用函数内 static + mutex 保证多线程安全
    static std::mutex mtx;
    static std::mt19937_64 rng(std::random_device{}());

    Uuid u;
    {
        std::lock_guard<std::mutex> lock(mtx);
        const std::uint64_t a = rng();
        const std::uint64_t b = rng();
        std::memcpy(u.bytes, &a, sizeof(a));
        std::memcpy(u.bytes + 8, &b, sizeof(b));
    }

    // version 4：byte[6] 高 4 位 = 0100
    u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0x0F) | 0x40);
    // variant：byte[8] 高 2 位 = 10
    u.bytes[8] = static_cast<std::uint8_t>((u.bytes[8] & 0x3F) | 0x80);
    return u;
}

namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool Uuid::parse(const std::string& str, Uuid& out)
{
    // 支持带连字符（36 字符）与不带连字符（32 字符）两种形式
    const bool has_hyphen = str.size() == 36;
    if (!has_hyphen && str.size() != 32) {
        return false;
    }

    int byte_index = 0;
    int nibble = 0;  // 0 = 高半字节，1 = 低半字节
    std::uint8_t current = 0;

    for (size_t i = 0; i < str.size(); ++i) {
        const char c = str[i];
        if (has_hyphen && c == '-') {
            // 连字符必须出现在 8/13/18/23 位置
            const size_t expect[] = {8, 13, 18, 23};
            bool ok = false;
            for (size_t k = 0; k < 4; ++k) {
                if (i == expect[k]) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                return false;
            }
            continue;
        }
        const int v = hex_value(c);
        if (v < 0) {
            return false;
        }
        current = static_cast<std::uint8_t>((current << 4) | v);
        if (nibble == 1) {
            if (byte_index >= 16) {
                return false;
            }
            out.bytes[byte_index++] = current;
            current = 0;
        }
        nibble ^= 1;
    }
    return byte_index == 16 && nibble == 0;
}

std::string Uuid::to_string() const
{
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

std::string Uuid::to_hex_string() const
{
    char buf[33];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

bool Uuid::operator==(const Uuid& other) const
{
    for (int i = 0; i < 16; ++i) {
        if (bytes[i] != other.bytes[i]) {
            return false;
        }
    }
    return true;
}

bool Uuid::operator!=(const Uuid& other) const
{
    return !(*this == other);
}

bool Uuid::operator<(const Uuid& other) const
{
    for (int i = 0; i < 16; ++i) {
        if (bytes[i] != other.bytes[i]) {
            return bytes[i] < other.bytes[i];
        }
    }
    return false;
}

bool Uuid::is_nil() const
{
    for (int i = 0; i < 16; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

Uuid Uuid::nil()
{
    Uuid u;
    for (int i = 0; i < 16; ++i) {
        u.bytes[i] = 0;
    }
    return u;
}

}  // namespace libmini
