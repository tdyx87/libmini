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

// SHA-1 摘要（RFC 3174）。
//
// ⚠️ 安全状态：SHA-1 的**碰撞**已被攻破（SHAttered，2017），任何需要抗
// 碰撞的场景都不要再用它。本类存在的唯一理由是兼容——老协议、SHA-1 结尾的
// 证书指纹（fingerprint）、某些去重键的历史数据。**新代码请用 Sha256 或
// Sha512。** 与 Md5 同理：不实现抗攻击的对策，但要在调用处标明意图，
// 否则「SHA-1 和 SHA-256 长得一样」的观感会让人误以为两者安全性相当。
class LIBMINI_API Sha1 {
public:
    Sha1() { reset(); }

    void reset();
    void update(const void* data, std::size_t size);
    void update(const std::string& data) { update(data.data(), data.size()); }

    // 结束并输出 20 字节摘要（之后可 reset() 复用）
    std::string finish();

    // 一次性计算，返回 40 字符小写十六进制
    static std::string hex(const std::string& data);

private:
    void process_block(const std::uint8_t* block);

    std::uint32_t state_[5];
    std::uint64_t bit_count_;
    std::uint8_t buffer_[64];
    std::size_t buffer_used_;
};

// SHA-512 摘要（FIPS 180-4），SHA-2 家族中宽度最大的一个。
//
// 用途与选型要点：
//   * 64 字节输出在**抗碰撞**上远强于 SHA-256。需要「身份/内容唯一标识」
//     时首选它——SHA-256/SHA-512 的选择与输出长度无关，只与安全余量有关。
//   * 输出 512 bit 不是 SHA-256 的「加强版」，两者抗碰撞能力同为 2^256，
//     SHA-512 只是截断攻击门槛更高。
//   * 性能：纯 C++ 实现的 SHA-512 在 32 位平台上比 SHA-256 慢不少
//     （64 位运算要拆成两次 32 位）。批量小数据用 SHA-256 更划算。
//
// 为什么不走系统加密库（Windows BCrypt / OpenSSL EVP）：Md5/Sha256 一直
// 是本文件内自带的纯 C++ 实现，摘要算法没有「密钥句柄」「上下文分配」
// 这些会失败的东西，也不该有平台间不同的失败语义。同一份实现 = 三个平台
// 逐字节一致的结果，这在跨平台系统里比对速更值钱。
class LIBMINI_API Sha512 {
public:
    Sha512() { reset(); }

    void reset();
    void update(const void* data, std::size_t size);
    void update(const std::string& data) { update(data.data(), data.size()); }

    // 结束并输出 64 字节摘要（之后可 reset() 复用）
    std::string finish();

    // 一次性计算，返回 128 字符小写十六进制
    static std::string hex(const std::string& data);

private:
    void process_block(const std::uint8_t* block);

    std::uint64_t state_[8];
    std::uint64_t bit_count_;   // 128 位计数器：SHA-512 的长度字段是 16 字节
    std::uint64_t bit_count_hi_;
    std::uint8_t buffer_[128];
    std::size_t buffer_used_;
};

}  // namespace libmini

#endif  // LIBMINI_DIGEST_H
