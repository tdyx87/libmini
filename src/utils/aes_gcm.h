#ifndef LIBMINI_AES_GCM_H
#define LIBMINI_AES_GCM_H

#include <cstdint>
#include <string>

#include "libmini.h"

namespace libmini {

// AES-256-GCM 认证加密（AEAD）：加密 + 完整性校验一体，密文被篡改
// 时解密直接失败（而不是返回垃圾）。适合本地敏感数据落盘、配置加密。
//
//   // 一次性 API（密钥 32 字节；nonce 12 字节；重复用 nonce 会破坏安全性！）
//   std::string ct = Aes256Gcm::encrypt(key, nonce, plaintext, aad);
//   std::string pt = Aes256Gcm::decrypt(key, nonce, ct, aad);  // 篡改返回空
//
//   // 现场生成随机 nonce 并拼在密文前的"落盘格式"：
//   std::string sealed = Aes256Gcm::seal(key, plaintext);   // nonce||ct||tag
//   std::string pt = Aes256Gcm::open(key, sealed);          // 失败返回空
//
// Windows 实现：CNG（bcrypt.dll，系统自带）；POSIX：OpenSSL EVP。
class LIBMINI_API Aes256Gcm
{
public:
    static constexpr std::size_t kKeySize = 32;
    static constexpr std::size_t kNonceSize = 12;
    static constexpr std::size_t kTagSize = 16;

    // 加密。key 必须 32B、nonce 必须 12B，否则返回空。
    // aad（附加认证数据）参与认证但不加密，可为空。
    // 输出 = 密文（与明文等长）|| 16B tag
    static std::string encrypt(const std::string& key, const std::string& nonce,
                               const std::string& plaintext,
                               const std::string& aad = "");

    // 解密 + 认证。任何校验失败（key/nonce 错、密文或 tag 被篡改、aad 不匹配）
    // 返回空串。
    static std::string decrypt(const std::string& key, const std::string& nonce,
                               const std::string& ciphertext_with_tag,
                               const std::string& aad = "");

    // 落盘格式：随机 12B nonce || 密文 || 16B tag。每次调用 nonce 都不同
    static std::string seal(const std::string& key, const std::string& plaintext,
                            const std::string& aad = "");

    // 解 seal；失败返回空
    static std::string open(const std::string& key, const std::string& sealed,
                            const std::string& aad = "");
};

}  // namespace libmini

#endif  // LIBMINI_AES_GCM_H
