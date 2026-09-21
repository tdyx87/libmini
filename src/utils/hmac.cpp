#include "hmac.h"
#include "digest.h"
#include "encoding.h"

#include <cstring>

namespace libmini {

namespace {

// HMAC 块大小（SHA-256 与 MD5 均为 64 字节）
constexpr std::size_t kHmacBlockSize = 64;

}  // namespace

// ==================== HMAC-SHA256 ====================

void HmacSha256::reset()
{
    inner_.reset();
    outer_.reset();
    keyed_ = false;
}

void HmacSha256::set_key(const std::string& key)
{
    unsigned char kipad[kHmacBlockSize];
    unsigned char kopad[kHmacBlockSize];

    if (key.size() > kHmacBlockSize) {
        // RFC 2104：key 长于块大小时，先对 key 求摘要，摘要（原始字节）作为密钥
        Sha256 h;
        h.update(key);
        const std::string digest = h.finish();  // 32 字节原始值
        std::memset(kipad, 0, kHmacBlockSize);
        std::memcpy(kipad, digest.data(), digest.size());
        std::memcpy(kopad, kipad, kHmacBlockSize);
    } else {
        std::memset(kipad, 0, kHmacBlockSize);
        std::memcpy(kipad, key.data(), key.size());
        std::memcpy(kopad, kipad, kHmacBlockSize);
    }
    for (std::size_t i = 0; i < kHmacBlockSize; ++i) {
        kipad[i] ^= 0x36;
        kopad[i] ^= 0x5c;
    }

    inner_.reset();
    inner_.update(kipad, kHmacBlockSize);
    outer_.reset();
    outer_.update(kopad, kHmacBlockSize);
    keyed_ = true;
}

void HmacSha256::update(const void* data, std::size_t size)
{
    inner_.update(data, size);
}

std::string HmacSha256::finish()
{
    // 内层摘要 → 喂给外层
    const std::string inner_digest = inner_.finish();
    outer_.update(inner_digest.data(), inner_digest.size());
    const std::string mac = outer_.finish();
    keyed_ = false;  // finish 后实例不可继续 update，需重新 set_key
    return mac;
}

std::string HmacSha256::raw(const std::string& key, const std::string& data)
{
    HmacSha256 h;
    h.set_key(key);
    h.update(data);
    return h.finish();
}

std::string HmacSha256::hex(const std::string& key, const std::string& data)
{
    return Hex::encode(raw(key, data), /*lower_case=*/true);
}

// ==================== HMAC-MD5 ====================

void HmacMd5::reset()
{
    inner_.reset();
    outer_.reset();
    keyed_ = false;
}

void HmacMd5::set_key(const std::string& key)
{
    unsigned char kipad[kHmacBlockSize];
    unsigned char kopad[kHmacBlockSize];

    if (key.size() > kHmacBlockSize) {
        Md5 h;
        h.update(key);
        const std::string digest = h.finish();  // 16 字节原始值
        std::memset(kipad, 0, kHmacBlockSize);
        std::memcpy(kipad, digest.data(), digest.size());
        std::memcpy(kopad, kipad, kHmacBlockSize);
    } else {
        std::memset(kipad, 0, kHmacBlockSize);
        std::memcpy(kipad, key.data(), key.size());
        std::memcpy(kopad, kipad, kHmacBlockSize);
    }
    for (std::size_t i = 0; i < kHmacBlockSize; ++i) {
        kipad[i] ^= 0x36;
        kopad[i] ^= 0x5c;
    }

    inner_.reset();
    inner_.update(kipad, kHmacBlockSize);
    outer_.reset();
    outer_.update(kopad, kHmacBlockSize);
    keyed_ = true;
}

void HmacMd5::update(const void* data, std::size_t size)
{
    inner_.update(data, size);
}

std::string HmacMd5::finish()
{
    const std::string inner_digest = inner_.finish();
    outer_.update(inner_digest.data(), inner_digest.size());
    const std::string mac = outer_.finish();
    keyed_ = false;
    return mac;
}

std::string HmacMd5::raw(const std::string& key, const std::string& data)
{
    HmacMd5 h;
    h.set_key(key);
    h.update(data);
    return h.finish();
}

std::string HmacMd5::hex(const std::string& key, const std::string& data)
{
    return Hex::encode(raw(key, data), /*lower_case=*/true);
}

}  // namespace libmini
