#ifndef LIBMINI_DIGEST_H
#define LIBMINI_DIGEST_H

#include <cstdint>
#include <string>

#include "libmini.h"

namespace libmini {

// MD5 摘要（RFC 1321）。用于缓存键、去重、文件校验等非安全场景。
class LIBMINI_API Md5 {
public:
    Md5() { reset(); }

    void reset();
    void update(const void* data, std::size_t size);
    void update(const std::string& data) { update(data.data(), data.size()); }

    // 结束并输出 16 字节摘要（之后可 reset() 复用）
    std::string finish();

    // 一次性计算，返回 32 字符小写十六进制
    static std::string hex(const std::string& data);

private:
    void process_block(const std::uint8_t* block);

    std::uint32_t state_[4];
    std::uint64_t bit_count_;
    std::uint8_t buffer_[64];
    std::size_t buffer_used_;
};

// SHA-256 摘要（FIPS 180-4）
class LIBMINI_API Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t size);
    void update(const std::string& data) { update(data.data(), data.size()); }

    // 结束并输出 32 字节摘要（之后可 reset() 复用）
    std::string finish();

    // 一次性计算，返回 64 字符小写十六进制
    static std::string hex(const std::string& data);

private:
    void process_block(const std::uint8_t* block);

    std::uint32_t state_[8];
    std::uint64_t bit_count_;
    std::uint8_t buffer_[64];
    std::size_t buffer_used_;
};

}  // namespace libmini

#endif  // LIBMINI_DIGEST_H
