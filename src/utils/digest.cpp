#include "digest.h"

#include <cstring>

#include "encoding.h"

namespace libmini {

namespace {

inline std::uint32_t rotl32(std::uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

inline std::uint32_t rotr32(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

// 64 位旋转：SHA-512 的 σ/Σ 全是 64 位旋转，32 位版本在这里没有意义。
// 旋转量恒小于位宽，移位 32-n 在 n=32 时才为 0，本库不存在这种用法。
inline std::uint64_t rotr64(std::uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

// RFC 1321 常量表 T[i] = floor(2^32 * abs(sin(i+1)))
const std::uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

const std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// FIPS 180-4 §4.2.3：K[i] = 前 80 个素数的立方根小数部分的前 64 位
const std::uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,};

}  // namespace

// ================================ MD5 =================================

void Md5::reset()
{
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    bit_count_ = 0;
    buffer_used_ = 0;
}

void Md5::process_block(const std::uint8_t* p)
{
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<std::uint32_t>(p[i * 4]) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];

    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g, s;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
            static const int kS[4] = {7, 12, 17, 22};
            s = kS[i % 4];
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
            static const int kS[4] = {5, 9, 14, 20};
            s = kS[i % 4];
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
            static const int kS[4] = {4, 11, 16, 23};
            s = kS[i % 4];
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
            static const int kS[4] = {6, 10, 15, 21};
            s = kS[i % 4];
        }
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + kMd5K[i] + m[g], s);
        a = tmp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const void* data, std::size_t size)
{
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    bit_count_ += static_cast<std::uint64_t>(size) * 8;

    if (buffer_used_ > 0) {
        const std::size_t take =
            (64 - buffer_used_ < size) ? 64 - buffer_used_ : size;
        std::memcpy(buffer_ + buffer_used_, p, take);
        buffer_used_ += take;
        p += take;
        size -= take;
        if (buffer_used_ == 64) {
            process_block(buffer_);
            buffer_used_ = 0;
        }
    }
    while (size >= 64) {
        process_block(p);
        p += 64;
        size -= 64;
    }
    if (size > 0) {
        std::memcpy(buffer_, p, size);
        buffer_used_ = size;
    }
}

std::string Md5::finish()
{
    const std::uint64_t bits = bit_count_;

    // 0x80 + 补零 + 8 字节小端长度（不改变已计数的 bit_count_ 语义：
    // 长度字段在追加前就已固定）
    std::uint8_t tail[72];
    std::memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    const std::size_t pad = (buffer_used_ < 56) ? (56 - buffer_used_ - 1)
                                                : (120 - buffer_used_ - 1);
    for (int i = 0; i < 8; ++i) {
        tail[1 + pad + i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    update(tail, 1 + pad + 8);

    std::string out(16, '\0');
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[static_cast<std::size_t>(i * 4 + j)] =
                static_cast<char>((state_[i] >> (8 * j)) & 0xFF);
        }
    }
    return out;
}

std::string Md5::hex(const std::string& data)
{
    Md5 m;
    m.update(data);
    return Hex::encode(m.finish(), true);
}

// ============================== SHA-256 ===============================

void Sha256::reset()
{
    static const std::uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                           0xa54ff53a, 0x510e527f, 0x9b05688c,
                                           0x1f83d9ab, 0x5be0cd19};
    for (int i = 0; i < 8; ++i) {
        state_[i] = kInit[i];
    }
    bit_count_ = 0;
    buffer_used_ = 0;
}

void Sha256::process_block(const std::uint8_t* p)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
        const std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size)
{
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    bit_count_ += static_cast<std::uint64_t>(size) * 8;

    if (buffer_used_ > 0) {
        const std::size_t take =
            (64 - buffer_used_ < size) ? 64 - buffer_used_ : size;
        std::memcpy(buffer_ + buffer_used_, p, take);
        buffer_used_ += take;
        p += take;
        size -= take;
        if (buffer_used_ == 64) {
            process_block(buffer_);
            buffer_used_ = 0;
        }
    }
    while (size >= 64) {
        process_block(p);
        p += 64;
        size -= 64;
    }
    if (size > 0) {
        std::memcpy(buffer_, p, size);
        buffer_used_ = size;
    }
}

std::string Sha256::finish()
{
    const std::uint64_t bits = bit_count_;

    std::uint8_t tail[72];
    std::memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    const std::size_t pad = (buffer_used_ < 56) ? (56 - buffer_used_ - 1)
                                                : (120 - buffer_used_ - 1);
    // SHA-256 长度字段是大端
    for (int i = 0; i < 8; ++i) {
        tail[1 + pad + i] = static_cast<std::uint8_t>(bits >> (8 * (7 - i)));
    }
    update(tail, 1 + pad + 8);

    std::string out(32, '\0');
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[static_cast<std::size_t>(i * 4 + j)] =
                static_cast<char>((state_[i] >> (24 - 8 * j)) & 0xFF);
        }
    }
    return out;
}

std::string Sha256::hex(const std::string& data)
{
    Sha256 s;
    s.update(data);
    return Hex::encode(s.finish(), true);
}

// ================================ SHA-1 ================================

void Sha1::reset()
{
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    state_[4] = 0xc3d2e1f0;
    bit_count_ = 0;
    buffer_used_ = 0;
}

void Sha1::process_block(const std::uint8_t* p)
{
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(p[i * 4 + 3]);
    }
    // 消息调度：W[t] = rotl(W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16], 1)
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2];
    std::uint32_t d = state_[3], e = state_[4];

    for (int i = 0; i < 80; ++i) {
        std::uint32_t f;
        std::uint32_t k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        const std::uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = tmp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(const void* data, std::size_t size)
{
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    bit_count_ += static_cast<std::uint64_t>(size) * 8;

    if (buffer_used_ > 0) {
        const std::size_t take =
            (64 - buffer_used_ < size) ? 64 - buffer_used_ : size;
        std::memcpy(buffer_ + buffer_used_, p, take);
        buffer_used_ += take;
        p += take;
        size -= take;
        if (buffer_used_ == 64) {
            process_block(buffer_);
            buffer_used_ = 0;
        }
    }
    while (size >= 64) {
        process_block(p);
        p += 64;
        size -= 64;
    }
    if (size > 0) {
        std::memcpy(buffer_, p, size);
        buffer_used_ = size;
    }
}

std::string Sha1::finish()
{
    const std::uint64_t bits = bit_count_;

    std::uint8_t tail[72];
    std::memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    const std::size_t pad = (buffer_used_ < 56) ? (56 - buffer_used_ - 1)
                                                : (120 - buffer_used_ - 1);
    for (int i = 0; i < 8; ++i) {
        tail[1 + pad + i] = static_cast<std::uint8_t>(bits >> (8 * (7 - i)));
    }
    update(tail, 1 + pad + 8);

    std::string out(20, '\0');
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[static_cast<std::size_t>(i * 4 + j)] =
                static_cast<char>((state_[i] >> (24 - 8 * j)) & 0xFF);
        }
    }
    return out;
}

std::string Sha1::hex(const std::string& data)
{
    Sha1 s;
    s.update(data);
    return Hex::encode(s.finish(), true);
}

// ================================ SHA-512 ================================

void Sha512::reset()
{
    // FIPS 180-4 §5.3.3：前 8 个素数的小数部分的平方根取小数前 64 位
    state_[0] = 0x6a09e667f3bcc908ull;
    state_[1] = 0xbb67ae8584caa73bull;
    state_[2] = 0x3c6ef372fe94f82bull;
    state_[3] = 0xa54ff53a5f1d36f1ull;
    state_[4] = 0x510e527fade682d1ull;
    state_[5] = 0x9b05688c2b3e6c1full;
    state_[6] = 0x1f83d9abfb41bd6bull;
    state_[7] = 0x5be0cd19137e2179ull;
    bit_count_ = 0;
    bit_count_hi_ = 0;
    buffer_used_ = 0;
}

void Sha512::process_block(const std::uint8_t* p)
{
    std::uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        std::uint64_t value = 0;
        for (int j = 0; j < 8; ++j) {
            value = (value << 8) | static_cast<std::uint64_t>(p[i * 8 + j]);
        }
        w[i] = value;
    }
    // 消息展开：σ0 与 σ1 是 SHA-256 那些函数的 64 位对应版本，
    // 只是旋转量更大（1/8/7 → 1/8/7，19/61/6 → 19/61/6）。
    for (int i = 16; i < 80; ++i) {
        const std::uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^
                                 (w[i - 15] >> 7);
        const std::uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^
                                 (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint64_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 80; ++i) {
        const std::uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        const std::uint64_t ch = (e & f) ^ (~e & g);
        const std::uint64_t temp1 = h + S1 + ch + kSha512K[i] + w[i];
        const std::uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint64_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha512::update(const void* data, std::size_t size)
{
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    const std::uint64_t add = static_cast<std::uint64_t>(size) * 8;
    // SHA-512 的长度字段是 128 位。直接 `bit_count_ += add` 在超过
    // 2^61 字节后会静默回绕（摘要算错但不报错，比崩更难查），故显式补高位。
    if (bit_count_ + add < bit_count_) {
        ++bit_count_hi_;
    }
    bit_count_ += add;

    if (buffer_used_ > 0) {
        const std::size_t take =
            (128 - buffer_used_ < size) ? 128 - buffer_used_ : size;
        std::memcpy(buffer_ + buffer_used_, p, take);
        buffer_used_ += take;
        p += take;
        size -= take;
        if (buffer_used_ == 128) {
            process_block(buffer_);
            buffer_used_ = 0;
        }
    }
    while (size >= 128) {
        process_block(p);
        p += 128;
        size -= 128;
    }
    if (size > 0) {
        std::memcpy(buffer_, p, size);
        buffer_used_ = size;
    }
}

std::string Sha512::finish()
{
    const std::uint64_t bits_lo = bit_count_;
    const std::uint64_t bits_hi = bit_count_hi_;

    // 填充到 112 mod 128，随后是 16 字节（128 位）长度字段，均为大端
    std::uint8_t tail[144];
    std::memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    const std::size_t pad = (buffer_used_ < 112) ? (112 - buffer_used_ - 1)
                                                 : (240 - buffer_used_ - 1);
    for (int i = 0; i < 8; ++i) {
        tail[1 + pad + i] =
            static_cast<std::uint8_t>(bits_hi >> (8 * (7 - i)));
    }
    for (int i = 0; i < 8; ++i) {
        tail[1 + pad + 8 + i] =
            static_cast<std::uint8_t>(bits_lo >> (8 * (7 - i)));
    }
    update(tail, 1 + pad + 16);

    std::string out(64, '\0');
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            out[static_cast<std::size_t>(i * 8 + j)] =
                static_cast<char>((state_[i] >> (56 - 8 * j)) & 0xFF);
        }
    }
    return out;
}

std::string Sha512::hex(const std::string& data)
{
    Sha512 s;
    s.update(data);
    return Hex::encode(s.finish(), true);
}

}  // namespace libmini
