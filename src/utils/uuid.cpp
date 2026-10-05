#include "uuid.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "digest.h"
#include "secure_random.h"
#include "time_utils.h"

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

// v7 的单调性状态。RFC 9562 §6.2 Method 1 推荐的「随机种子 + 递增」做法：
// 每进入新的一毫秒换一个随机种子，毫秒内靠计数器递增。同一毫秒内的多个
// ID 因此有确定顺序，而相邻毫秒之间又看不出可预测的规律。
struct V7State
{
    std::int64_t last_ms = -1;
    std::uint16_t counter = 0;
    std::mutex mutex;
};

V7State& v7_state()
{
    static V7State state;
    return state;
}

// rand_a 只有 12 位 = 4096 个/ms。突发流量（批量导入、事件风暴）下单毫秒
// 可能超过 4096 个 ID，此时把时间戳推进 1ms 而不是让计数器回绕——回绕会
// 直接破坏单调性，而时间戳前移 1ms 在语义上完全可接受。
constexpr std::uint16_t kV7CounterMax = 0x0FFF;

}  // namespace

Uuid Uuid::generate_v7(std::int64_t unix_ms)
{
    // 48 位无符号毫秒可表示到公元 10889 年。负值与超范围值都落到 epoch，
    // 而不是回绕成别的年份：调用方传了离谱的时间戳，静默生成一个
    // 看着合法的 ID 比生成一个明显异常的更难排查。
    std::uint64_t ms = 0;
    if (unix_ms >= 0 && unix_ms <= 0xFFFFFFFFFFFFLL) {
        ms = static_cast<std::uint64_t>(unix_ms);
    }

    Uuid u;
    // rand_b（byte 8 低 6 位 + byte 9..15）必须随机填满——不填就是
    // 未初始化的栈内存，会被原样写进 ID 并泄漏到日志/接口里。
    // 注意 secure_random_bytes 会失败，此时保留栈内容也不安全，
    // 故先整体清零。
    std::memset(u.bytes, 0, sizeof(u.bytes));
    if (!secure_random_bytes(u.bytes, sizeof(u.bytes))) {
        return nil();
    }

    // bytes 0..5：48 位 Unix 毫秒，大端
    for (int i = 0; i < 6; ++i) {
        u.bytes[i] = static_cast<std::uint8_t>((ms >> (8 * (5 - i))) & 0xFF);
    }
    // bytes 6..7：version(4) | rand_a(12)
    u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0x0F) | 0x70);
    // bytes 8..15：variant(2) | rand_b(62)
    u.bytes[8] = static_cast<std::uint8_t>((u.bytes[8] & 0x3F) | 0x80);
    return u;
}

Uuid Uuid::generate_v7()
{
    const std::int64_t now = current_timestamp_ms();

    std::int64_t effective_ms = now;
    std::uint16_t counter = 0;
    {
        V7State& state = v7_state();
        std::lock_guard<std::mutex> guard(state.mutex);
        // 必须是 now > last_ms 而不是 !=：配额用尽时 last_ms 会被推到
        // 真实时钟「前面」，此时真实 now 仍停在那毫秒，用 != 判断会让
        // 内部时钟回退到 now，产出比上一个更小的 ID，单调性直接破掉。
        // NTP 校时导致系统时钟回拨时同理。
        if (now > state.last_ms) {
            // 新毫秒：换随机种子。熵源不可用时退化成 0，单调性不受影响，
            // 只是少了这层随机（相邻毫秒的 ID 会更相似）。
            std::uint16_t seed = 0;
            secure_random_bytes(&seed, sizeof(seed));
            state.last_ms = now;
            state.counter = static_cast<std::uint16_t>(seed & kV7CounterMax);
        } else {
            // 同一毫秒（含时钟回拨导致的 now <= last_ms）：递增。
            // 本毫秒配额用尽就把内部时间戳 +1ms 继续，绝不让计数器回绕。
            // 代价是持续超过 4096 ID/ms 时内部时钟会慢慢领先真实时钟，
            // 但那个量级下「单调」比「时间戳绝对准确」重要得多。
            if (state.counter >= kV7CounterMax) {
                ++state.last_ms;
                state.counter = 0;
            } else {
                ++state.counter;
            }
        }
        effective_ms = state.last_ms;
        counter = state.counter;
    }

    Uuid u = generate_v7(effective_ms);
    u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0xF0) |
                                           ((counter >> 8) & 0x0F));
    u.bytes[7] = static_cast<std::uint8_t>(counter & 0xFF);
    return u;
}

Uuid Uuid::v5(const Uuid& name_space, const std::string& name)
{
    // RFC 4122 §4.3：SHA-1(namespace 的 16 字节 || name)，取前 16 字节
    Sha1 sha;
    sha.update(name_space.bytes, sizeof(name_space.bytes));
    sha.update(name);
    const std::string digest = sha.finish();  // 20 字节，取前 16

    Uuid u;
    for (int i = 0; i < 16; ++i) {
        u.bytes[i] = static_cast<std::uint8_t>(digest[static_cast<std::size_t>(i)]);
    }
    u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0x0F) | 0x50);
    u.bytes[8] = static_cast<std::uint8_t>((u.bytes[8] & 0x3F) | 0x80);
    return u;
}

Uuid Uuid::namespace_dns()
{
    Uuid u;
    const std::uint8_t raw[16] = {
        0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    std::memcpy(u.bytes, raw, sizeof(raw));
    return u;
}

Uuid Uuid::namespace_url()
{
    Uuid u;
    const std::uint8_t raw[16] = {
        0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    std::memcpy(u.bytes, raw, sizeof(raw));
    return u;
}

Uuid Uuid::namespace_oid()
{
    Uuid u;
    const std::uint8_t raw[16] = {
        0x6b, 0xa7, 0xb8, 0x12, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    std::memcpy(u.bytes, raw, sizeof(raw));
    return u;
}

Uuid Uuid::namespace_x500()
{
    Uuid u;
    const std::uint8_t raw[16] = {
        0x6b, 0xa7, 0xb8, 0x14, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    std::memcpy(u.bytes, raw, sizeof(raw));
    return u;
}

int Uuid::version() const
{
    return static_cast<int>((bytes[6] >> 4) & 0x0F);
}

int Uuid::variant() const
{
    const std::uint8_t b = bytes[8];
    if ((b & 0x80) == 0x00) return 0;  // NCS backward compatibility
    if ((b & 0xC0) == 0x80) return 2;  // RFC 4122
    if ((b & 0xE0) == 0xC0) return 6;  // Microsoft
    return 7;                          // 未来保留
}

std::int64_t Uuid::timestamp_ms() const
{
    if (version() != 7) {
        return 0;
    }
    std::int64_t ms = 0;
    for (int i = 0; i < 6; ++i) {
        ms = (ms << 8) | static_cast<std::uint64_t>(bytes[i]);
    }
    return ms;
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
