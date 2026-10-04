#include "uuid.h"

#include <cstdio>
#include <cstring>

#include "secure_random.h"

namespace libmini {

Uuid Uuid::generate()
{
    Uuid u;
    // v4 的全部随机性来自这 16 字节。此前用 mt19937_64 + random_device 播种，
    // 但 Mersenne Twister 是可预测的（624 个 32 位输出即可还原状态），而
    // MSVC/MinGW 的 std::random_device 也不是密码学实现。UUID 经常被当作
    // 会话 ID / 订单号 / 授权凭据使用，这些场景不该建立在可预测随机源上。
    if (!secure_random_bytes(u.bytes, sizeof(u.bytes))) {
        // 熵源不可用时返回 nil UUID 而不是伪造一个：调用方能凭 is_nil() 发现，
        // 比拿到一个「看起来正常但随机性不足」的 UUID 更容易排查。
        return nil();
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
