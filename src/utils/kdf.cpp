#include "kdf.h"

#include <cstring>

#include "aes_gcm.h"
#include "hmac.h"
#include "secure_random.h"

namespace libmini {

// C++11 里 static constexpr 数据成员只有在使用时不发生 ODR-use（按值取）才
// 不需要外部定义。测试里 EXPECT_EQ 会把参数绑定到 const 引用上，属于 ODR-use，
// 没有定义就是链接期 undefined reference，故这里补齐。
constexpr std::size_t Pbkdf2HmacSha256::kMinIterations;
constexpr std::size_t Pbkdf2HmacSha256::kDefaultIterations;
constexpr std::size_t Pbkdf2HmacSha256::kMaxIterations;
constexpr std::size_t Pbkdf2HmacSha256::kSaltSize;

namespace {

constexpr char kMagic[4] = { 'L', 'M', 'P', 'S' };
constexpr std::size_t kVersion = 1;
constexpr std::size_t kKdfPbkdf2Sha256 = 1;

constexpr std::size_t kHeaderFixedSize = 12;  // magic(4) version(1) kdf(1) iter(4) slen(2)
// 解析头部时的字段上限：长度字段是 2 字节可写 65535，但盐超过 64 字节、
// nonce 超过 32 字节的格式我们从未产出过，当损坏处理（避免按攻击者给的
// 长度做巨额分配）。
constexpr std::size_t kMaxSaltLen = 64;
constexpr std::size_t kMaxNonceLen = 32;

// 密封明文前置的域分隔标记。
//
// 它解决一个真实歧义：Aes256Gcm::decrypt 失败时返回空串，而「明文本来
// 就是空的」也返回空串——两者在返回值上不可区分，open 就没法判断成功与
// 否了。加一个必然存在的前导字节，失败（空）就永远不会是合法输出。
constexpr char kPlaintextMarker = '\x01';

void put_u16_be(std::string& out, std::size_t value)
{
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void put_u32_be(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

std::size_t get_u16_be(const std::string& in, std::size_t offset)
{
    const std::size_t hi = static_cast<unsigned char>(in[offset]);
    const std::size_t lo = static_cast<unsigned char>(in[offset + 1]);
    return (hi << 8) | lo;
}

std::uint32_t get_u32_be(const std::string& in, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) |
                static_cast<unsigned char>(in[offset + i]);
    }
    return value;
}

// 解析头部。填充 ok 之外的字段前，调用方需自行检查布尔返回值。
bool parse_header(const std::string& sealed, PasswordSeal::Info& info)
{
    if (sealed.size() < kHeaderFixedSize) {
        return false;
    }
    if (std::memcmp(sealed.data(), kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    info.version = static_cast<unsigned char>(sealed[4]);
    if (info.version != kVersion) {
        return false;  // 未知版本：结构都可能变了，不猜
    }
    info.kdf_id = static_cast<unsigned char>(sealed[5]);
    if (info.kdf_id != kKdfPbkdf2Sha256) {
        return false;  // 未知 KDF：不猜
    }
    info.iterations = get_u32_be(sealed, 6);

    const std::size_t salt_len = get_u16_be(sealed, 10);
    if (salt_len < 8 || salt_len > kMaxSaltLen) {
        return false;
    }
    std::size_t pos = kHeaderFixedSize;
    if (sealed.size() < pos + salt_len + 2) {
        return false;
    }
    info.salt = sealed.substr(pos, salt_len);
    pos += salt_len;

    const std::size_t nonce_len = get_u16_be(sealed, pos);
    if (nonce_len != Aes256Gcm::kNonceSize || nonce_len > kMaxNonceLen) {
        return false;
    }
    pos += 2;
    if (sealed.size() < pos + nonce_len) {
        return false;
    }
    info.nonce = sealed.substr(pos, nonce_len);
    pos += nonce_len;

    info.ciphertext_size = sealed.size() - pos;
    // 至少要装下 GCM 的 tag，否则解不开
    if (info.ciphertext_size < Aes256Gcm::kTagSize) {
        return false;
    }
    return true;
}

std::string build_header(std::size_t iterations, const std::string& salt,
                         const std::string& nonce)
{
    std::string header;
    header.reserve(kHeaderFixedSize + salt.size() + 2 + nonce.size());
    header.append(kMagic, sizeof(kMagic));
    header.push_back(static_cast<char>(kVersion));
    header.push_back(static_cast<char>(kKdfPbkdf2Sha256));
    put_u32_be(header, static_cast<std::uint32_t>(iterations));
    put_u16_be(header, salt.size());
    header.append(salt);
    put_u16_be(header, nonce.size());
    header.append(nonce);
    return header;
}

// 头部进入 GCM 的 AAD：这样迭代次数、盐、nonce 被改动都会导致解密失败。
// 否则攻击者能把 iterations 改成 1，密钥立刻退化成一次哈希。
std::string gcm_aad(const std::string& header, const std::string& user_aad)
{
    std::string aad;
    aad.reserve(header.size() + user_aad.size());
    aad.append(header);
    aad.append(user_aad);
    return aad;
}

}  // namespace

// ==================== PBKDF2-HMAC-SHA256 ====================

std::string Pbkdf2HmacSha256::derive(const std::string& password,
                                     const std::string& salt,
                                     std::size_t iterations,
                                     std::size_t key_bytes)
{
    if (key_bytes == 0) {
        return std::string();
    }
    // RFC 2898 §5.2 明确 salt 应至少 8 字节。短于此的盐基本等于没有，
    // 而我们自己产出的永远是 16 字节，所以这里拒绝而不是补齐。
    if (salt.size() < 8) {
        return std::string();
    }
    if (iterations < kMinIterations) {
        iterations = kMinIterations;
    }
    if (iterations > kMaxIterations) {
        iterations = kMaxIterations;
    }

    std::string derived;
    derived.reserve(key_bytes);

    // 输出按 32 字节一块（hLen），块索引从 1 开始、大端四字节
    std::uint32_t block_index = 1;
    while (derived.size() < key_bytes) {
        unsigned char index_be[4];
        index_be[0] = static_cast<unsigned char>((block_index >> 24) & 0xff);
        index_be[1] = static_cast<unsigned char>((block_index >> 16) & 0xff);
        index_be[2] = static_cast<unsigned char>((block_index >> 8) & 0xff);
        index_be[3] = static_cast<unsigned char>(block_index & 0xff);

        // U1 = PRF(P, S || INT(i))
        HmacSha256 mac;
        mac.set_key(password);
        mac.update(salt);
        mac.update(index_be, sizeof(index_be));
        std::string u = mac.finish();
        std::string block = u;

        // T_i = U1 xor U2 xor ... xor Uc
        for (std::size_t round = 1; round < iterations; ++round) {
            mac.set_key(password);
            mac.update(u);
            u = mac.finish();
            for (std::size_t i = 0; i < block.size(); ++i) {
                block[i] = static_cast<char>(
                    static_cast<unsigned char>(block[i]) ^
                    static_cast<unsigned char>(u[i]));
            }
        }

        derived.append(block);
        if (block_index == 0xffffffffu) {
            break;  // 理论上限：key_bytes 超过 128GB，没有意义
        }
        ++block_index;
    }

    // 末块按需截断：PBKDF2 输出长度由 key_bytes 指定，不是 hLen 的整数倍
    derived.resize(key_bytes);
    return derived;
}

std::string Pbkdf2HmacSha256::derive_hex(const std::string& password,
                                         const std::string& salt,
                                         std::size_t iterations,
                                         std::size_t key_bytes)
{
    const std::string key = derive(password, salt, iterations, key_bytes);
    if (key.empty()) {
        return std::string();
    }
    static const char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(key.size() * 2);
    for (std::size_t i = 0; i < key.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(key[i]);
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0f]);
    }
    return out;
}

bool Pbkdf2HmacSha256::verify(const std::string& password,
                              const std::string& salt,
                              std::size_t iterations,
                              const std::string& expected_key)
{
    if (expected_key.empty()) {
        return false;
    }
    const std::string actual = derive(password, salt, iterations, expected_key.size());
    if (actual.empty()) {
        return false;
    }
    return constant_time_equals(actual, expected_key);
}

std::string Pbkdf2HmacSha256::random_salt(std::size_t salt_bytes)
{
    if (salt_bytes == 0) {
        return std::string();
    }
    return secure_random_string(salt_bytes);
}

// ==================== 常量时间比较 ====================

bool constant_time_equals(const std::string& a, const std::string& b)
{
    // 长度不同也照样把较短的那个比完：提前 return 会让「前 n 字节相等、
    // 第 n+1 字节不等」比「第 1 字节就不等」快，攻击者能据此逐位试探。
    // 长度本身不是秘密（部署时通常已知），但走完 min(len) 次循环更稳。
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < common; ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(a[i]) ^
                    static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

// ==================== 口令密封 ====================

std::string PasswordSeal::seal(const std::string& password,
                               const std::string& plaintext,
                               const std::string& aad,
                               std::size_t iterations)
{
    if (password.empty()) {
        // 空口令密封出来的文件任何人都能解（等价于没加密），与其产出
        // 一个看起来有保护其实没有的文件，不如直接失败。
        return std::string();
    }
    if (iterations < Pbkdf2HmacSha256::kMinIterations) {
        iterations = Pbkdf2HmacSha256::kMinIterations;
    }
    if (iterations > Pbkdf2HmacSha256::kMaxIterations) {
        iterations = Pbkdf2HmacSha256::kMaxIterations;
    }

    const std::string salt = Pbkdf2HmacSha256::random_salt(
        Pbkdf2HmacSha256::kSaltSize);
    if (salt.size() != Pbkdf2HmacSha256::kSaltSize) {
        return std::string();  // 熵源不可用
    }
    const std::string nonce = secure_random_string(Aes256Gcm::kNonceSize);
    if (nonce.size() != Aes256Gcm::kNonceSize) {
        return std::string();
    }

    const std::string key = Pbkdf2HmacSha256::derive(
        password, salt, iterations, Aes256Gcm::kKeySize);
    if (key.size() != Aes256Gcm::kKeySize) {
        return std::string();
    }

    const std::string header = build_header(iterations, salt, nonce);
    std::string inner;
    inner.reserve(plaintext.size() + 1);
    inner.push_back(kPlaintextMarker);
    inner.append(plaintext);

    const std::string ciphertext = Aes256Gcm::encrypt(
        key, nonce, inner, gcm_aad(header, aad));
    if (ciphertext.empty()) {
        return std::string();
    }

    std::string sealed;
    sealed.reserve(header.size() + ciphertext.size());
    sealed.append(header);
    sealed.append(ciphertext);
    return sealed;
}

bool PasswordSeal::open(const std::string& password,
                        const std::string& sealed,
                        std::string& plaintext,
                        const std::string& aad)
{
    plaintext.clear();

    Info info;
    if (!parse_header(sealed, info)) {
        return false;
    }
    // 头部里的 iterations 是攻击者可控的。seal 只会写入
    // [kMinIterations, kMaxIterations] 内的值，范围外直接判否——
    // 若按它来算，一次 open 就能变成 10^7 次 HMAC 的 CPU DoS。
    if (info.iterations < Pbkdf2HmacSha256::kMinIterations ||
        info.iterations > Pbkdf2HmacSha256::kMaxIterations) {
        return false;
    }

    const std::string key = Pbkdf2HmacSha256::derive(
        password, info.salt, info.iterations, Aes256Gcm::kKeySize);
    if (key.size() != Aes256Gcm::kKeySize) {
        return false;
    }

    const std::size_t header_size =
        sealed.size() - info.ciphertext_size;
    const std::string header = sealed.substr(0, header_size);
    const std::string ciphertext = sealed.substr(header_size);

    const std::string inner = Aes256Gcm::decrypt(
        key, info.nonce, ciphertext, gcm_aad(header, aad));
    if (inner.empty() || inner[0] != kPlaintextMarker) {
        return false;  // 口令错 / 密文被改 / aad 不匹配——三者不可区分
    }
    plaintext = inner.substr(1);
    return true;
}

std::string PasswordSeal::open_or_empty(const std::string& password,
                                        const std::string& sealed,
                                        const std::string& aad)
{
    std::string plaintext;
    if (!open(password, sealed, plaintext, aad)) {
        return std::string();
    }
    return plaintext;
}

bool PasswordSeal::inspect(const std::string& sealed, Info& info)
{
    info = Info();
    return parse_header(sealed, info);
}

bool PasswordSeal::needs_reseal(const std::string& sealed,
                                std::size_t min_iterations)
{
    Info info;
    if (!parse_header(sealed, info)) {
        return true;  // 认不出来：当作需要重新 seal
    }
    return info.iterations < min_iterations;
}

}  // namespace libmini
