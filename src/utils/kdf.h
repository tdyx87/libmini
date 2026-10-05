#ifndef LIBMINI_KDF_H
#define LIBMINI_KDF_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "export.h"

namespace libmini {

// 密钥派生与口令密封。
//
// 与既有模块的分工：
//   hmac     —— HMAC 原语（不负责「把口令变成密钥」这件事）
//   aes_gcm  —— AEAD 原语，但只接受 32 字节**原始密钥**，没有口令→密钥的配套
//   kdf（本模块）—— 把口令安全地变成密钥，并提供带版本号的落盘格式
//
// 为什么不把口令直接当 AES 密钥用：口令熵低。直接 AES 加密一个弱口令，
// 攻击者拿到密文后可以离线穷举整个口令空间，而 KDF 的迭代次数把每次
// 猜测的成本放大几千倍——这正是密码库里唯一真正防穷举的东西。

// PBKDF2-HMAC-SHA256（RFC 2898 §5.2）。
//
// 只用 HMAC-SHA256，不引入 scrypt/Argon2/Bcrypt 依赖：跨平台零新增依赖是
// 本库的硬约束之一（同一份代码要在 MSVC / MinGW / GCC / Clang 上都能编译，
// 且不引入平台相关的第三方构建问题）。代价是 PBKDF2 对 GPU/ASIC 不如
// Argon2id 抗并行破解——所以迭代次数默认给得较高，且格式里显式记录了次数，
// 将来要换算法可以平滑迁移（见 PasswordSeal::inspect）。
class LIBMINI_API Pbkdf2HmacSha256 {
public:
    // 最低迭代次数：低于 1000 的 PBKDF2 实际上和「多哈希几次」没区别。
    static constexpr std::size_t kMinIterations = 1000;
    // 默认迭代次数。OWASP 2023 对 PBKDF2-HMAC-SHA256 的建议是 600000，
    // 这里取 210000 作为「安全 / 交互场景延迟」的折中（桌面级 CPU 上
    // 几十毫秒量级），要更高安全性请显式传参。
    static constexpr std::size_t kDefaultIterations = 210000;
    // 硬上限：调用方传入的值超过它会被钳回。不设上限的话，
    // `derive(pw, salt, 0xFFFFFFFF, 32)` 就是一个 CPU DoS 开关——
    // 而迭代次数在 PasswordSeal 的文件头里，是攻击者可控的输入。
    // 取 100 万：单次约 1 秒量级，足够让「拿密文穷举口令」不可行，
    // 又不至于被用来把一台机器钉死。
    static constexpr std::size_t kMaxIterations = 1000000;
    // 推荐盐长度（RFC 2898 建议 ≥ 8 字节，工程上 16 字节是惯例）
    static constexpr std::size_t kSaltSize = 16;

    // 派生 key_bytes 字节密钥。iterations 会被钳到
    // [kMinIterations, kMaxIterations]，盐过短（< 8B）会被拒绝。
    // 参数非法返回空串。
    static std::string derive(const std::string& password,
                              const std::string& salt,
                              std::size_t iterations,
                              std::size_t key_bytes);

    // 同上，输出小写十六进制（长度 = key_bytes*2）
    static std::string derive_hex(const std::string& password,
                                  const std::string& salt,
                                  std::size_t iterations,
                                  std::size_t key_bytes);

    // 校验口令：重新派生后用常量时间比较。派生失败返回 false。
    static bool verify(const std::string& password,
                       const std::string& salt,
                       std::size_t iterations,
                       const std::string& expected_key);

    // 生成随机盐（secure_random，失败返回空串）
    static std::string random_salt(std::size_t salt_bytes = kSaltSize);
};

// 常量时间相等比较。
//
// `a == b` 对字符串会在第一个不同字节就返回，攻击者据此逐字节试探
// 校验值需要 O(n) 次而 O(n²) 次信息；防 timing 攻击必须自己写循环，
// 不能指望编译器/标准库。长度不同也返回 false（长度本身不是秘密）。
LIBMINI_API bool constant_time_equals(const std::string& a,
                                      const std::string& b);

// 口令密封：口令 → PBKDF2 → AES-256-GCM，输出自描述、可版本化的二进制格式。
//
// 落盘格式（全部整数为大端）：
//   偏移 0  : magic  'L' 'M' 'P' 'S'      4B
//   偏移 4  : version                        1B  （当前 = 1）
//   偏移 5  : kdf_id                         1B  （1 = PBKDF2-HMAC-SHA256）
//   偏移 6  : iterations                     4B  uint32
//   偏移 10 : salt_len                       2B  uint16
//   偏移 12 : salt                           salt_len B
//   偏移 12+salt_len : nonce_len            2B  uint16
//   ...     : nonce                          nonce_len B
//   ...     : ciphertext || tag（16B）        其余全部
//
// 之所以自带 magic + 版本 + 算法 ID：加密格式不是「内部数据结构」。
// 三年后想换 Argon2id、或把迭代次数调高，得能识别并迁移旧文件，
// 而不是拿到一段解不开的字节。
class LIBMINI_API PasswordSeal {
public:
    // 密封失败（熵源不可用、参数非法、加密失败）返回空串
    static std::string seal(const std::string& password,
                            const std::string& plaintext,
                            const std::string& aad = "",
                            std::size_t iterations = Pbkdf2HmacSha256::kDefaultIterations);

    // 解密封。口令错、格式被篡改、aad 不匹配一律返回 false——
    // 这三种失败在密码学上**无法区分**，也不该区分（区分开就等于
    // 告诉攻击者「格式是对的，只是密码错了」）。
    static bool open(const std::string& password,
                     const std::string& sealed,
                     std::string& plaintext,
                     const std::string& aad = "");

    // open 的便捷版：失败返回空串。
    // 注意空串既可能是「解密出的明文本来就是空的」，也可能是失败——
    // 明文是否可能为空你自己清楚，要区分就用返回 bool 的 open。
    static std::string open_or_empty(const std::string& password,
                                     const std::string& sealed,
                                     const std::string& aad = "");

    // 只读头部，不做 KDF 和解密——用于迁移/审计（想知道旧文件用的什么
    // 参数来决定要不要重新 seal）。magic 或版本不认识返回 false。
    struct Info {
        std::size_t version = 0;
        std::size_t kdf_id = 0;        // 1 = PBKDF2-HMAC-SHA256
        std::size_t iterations = 0;
        std::string salt;              // 原始字节
        std::string nonce;             // 原始字节
        std::size_t ciphertext_size = 0;  // 含 tag
    };
    static bool inspect(const std::string& sealed, Info& info);

    // 头部参数是否已低于当前推荐值（该重新 seal 了）。
    // 格式不可识别时返回 true（宁可多 seal 一次）。
    static bool needs_reseal(const std::string& sealed,
                             std::size_t min_iterations = Pbkdf2HmacSha256::kDefaultIterations);
};

}  // namespace libmini

#endif  // LIBMINI_KDF_H
