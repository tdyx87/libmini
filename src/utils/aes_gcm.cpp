#include "aes_gcm.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/evp.h>
#endif

#include <cstring>

#include "random_utils.h"

namespace libmini {

#ifdef _WIN32

namespace {

// 进程级 BCRYPT 算法句柄（CNG 约定：算法句柄可全局复用）
struct CngAlgHolder
{
    BCRYPT_ALG_HANDLE alg = nullptr;

    CngAlgHolder()
    {
        // AES-GCM：需要 chain-mode 属性配置
        if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0) {
            ::BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(L"ChainingModeGCM")),
                                sizeof(L"ChainingModeGCM"), 0);
        }
    }
    ~CngAlgHolder()
    {
        if (alg != nullptr) {
            ::BCryptCloseAlgorithmProvider(alg, 0);
        }
    }
};

CngAlgHolder& cng_alg()
{
    static CngAlgHolder holder;
    return holder;
}

}  // namespace

std::string Aes256Gcm::encrypt(const std::string& key, const std::string& nonce,
                               const std::string& plaintext, const std::string& aad)
{
    if (key.size() != kKeySize || nonce.size() != kNonceSize || cng_alg().alg == nullptr) {
        return "";
    }
    BCRYPT_KEY_HANDLE hkey = nullptr;
    if (::BCryptGenerateSymmetricKey(cng_alg().alg, &hkey, nullptr, 0,
                                     reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                     static_cast<ULONG>(key.size()), 0) != 0) {
        return "";
    }

    std::string out;
    do {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        ::BCRYPT_INIT_AUTH_MODE_INFO(info);
        UCHAR tag[kTagSize];
        info.pbTag = tag;
        info.cbTag = sizeof(tag);
        info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.data()));
        info.cbNonce = static_cast<ULONG>(nonce.size());
        UCHAR* aad_buf = nullptr;
        if (!aad.empty()) {
            aad_buf = reinterpret_cast<PUCHAR>(const_cast<char*>(aad.data()));
            info.pbAuthData = aad_buf;
            info.cbAuthData = static_cast<ULONG>(aad.size());
        }

        out.resize(plaintext.size());
        ULONG written = 0;
        const NTSTATUS rc = ::BCryptEncrypt(
            hkey,
            reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
            static_cast<ULONG>(plaintext.size()),
            &info,
            nullptr, 0,
            reinterpret_cast<PUCHAR>(&out[0]), static_cast<ULONG>(out.size()),
            &written, 0);
        if (rc != 0 || written != out.size()) {
            out.clear();
            break;
        }
        out.append(reinterpret_cast<const char*>(tag), sizeof(tag));  // ct || tag
    } while (false);

    ::BCryptDestroyKey(hkey);
    return out;
}

std::string Aes256Gcm::decrypt(const std::string& key, const std::string& nonce,
                               const std::string& ciphertext_with_tag, const std::string& aad)
{
    if (key.size() != kKeySize || nonce.size() != kNonceSize ||
        ciphertext_with_tag.size() < kTagSize || cng_alg().alg == nullptr) {
        return "";
    }
    const std::string ct = ciphertext_with_tag.substr(0, ciphertext_with_tag.size() - kTagSize);
    const UCHAR* tag = reinterpret_cast<const UCHAR*>(
        ciphertext_with_tag.data() + ct.size());

    BCRYPT_KEY_HANDLE hkey = nullptr;
    if (::BCryptGenerateSymmetricKey(cng_alg().alg, &hkey, nullptr, 0,
                                     reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                     static_cast<ULONG>(key.size()), 0) != 0) {
        return "";
    }

    std::string out;
    do {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        ::BCRYPT_INIT_AUTH_MODE_INFO(info);
        UCHAR tag_buf[kTagSize];
        std::memcpy(tag_buf, tag, kTagSize);
        info.pbTag = tag_buf;
        info.cbTag = kTagSize;
        info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.data()));
        info.cbNonce = static_cast<ULONG>(nonce.size());
        if (!aad.empty()) {
            info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(aad.data()));
            info.cbAuthData = static_cast<ULONG>(aad.size());
        }

        out.resize(ct.size());
        ULONG written = 0;
        // 认证失败返回 STATUS_AUTH_TAG_MISMATCH，输出不可用
        const NTSTATUS rc = ::BCryptDecrypt(
            hkey,
            reinterpret_cast<PUCHAR>(const_cast<char*>(ct.data())),
            static_cast<ULONG>(ct.size()),
            &info,
            nullptr, 0,
            reinterpret_cast<PUCHAR>(&out[0]), static_cast<ULONG>(out.size()),
            &written, 0);
        if (rc != 0 || written != out.size()) {
            out.clear();
        }
    } while (false);

    ::BCryptDestroyKey(hkey);
    return out;
}

#else  // POSIX: OpenSSL

std::string Aes256Gcm::encrypt(const std::string& key, const std::string& nonce,
                               const std::string& plaintext, const std::string& aad)
{
    if (key.size() != kKeySize || nonce.size() != kNonceSize) {
        return "";
    }
    const EVP_CIPHER* cipher = ::EVP_aes_256_gcm();
    EVP_CIPHER_CTX* ctx = ::EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return "";
    }
    std::string out;
    do {
        int len = 0;
        if (::EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
            ::EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1 ||
            ::EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char*>(key.data()),
                                 reinterpret_cast<const unsigned char*>(nonce.data())) != 1) {
            break;
        }
        if (!aad.empty() &&
            ::EVP_EncryptUpdate(ctx, nullptr, &len,
                                reinterpret_cast<const unsigned char*>(aad.data()),
                                static_cast<int>(aad.size())) != 1) {
            break;
        }
        out.resize(plaintext.size());
        int total = 0;
        if (!plaintext.empty() &&
            ::EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&out[0]), &len,
                                reinterpret_cast<const unsigned char*>(plaintext.data()),
                                static_cast<int>(plaintext.size())) != 1) {
            out.clear();
            break;
        }
        total = len;
        if (::EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]) + total,
                                  &len) != 1) {
            out.clear();
            break;
        }
        out.resize(static_cast<std::size_t>(total) + len);
        unsigned char tag[kTagSize];
        if (::EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, tag) != 1) {
            out.clear();
            break;
        }
        out.append(reinterpret_cast<const char*>(tag), kTagSize);
    } while (false);
    ::EVP_CIPHER_CTX_free(ctx);
    return out;
}

std::string Aes256Gcm::decrypt(const std::string& key, const std::string& nonce,
                               const std::string& ciphertext_with_tag, const std::string& aad)
{
    if (key.size() != kKeySize || nonce.size() != kNonceSize ||
        ciphertext_with_tag.size() < kTagSize) {
        return "";
    }
    const std::string ct = ciphertext_with_tag.substr(0, ciphertext_with_tag.size() - kTagSize);
    const unsigned char* tag = reinterpret_cast<const unsigned char*>(
        ciphertext_with_tag.data() + ct.size());

    const EVP_CIPHER* cipher = ::EVP_aes_256_gcm();
    EVP_CIPHER_CTX* ctx = ::EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return "";
    }
    std::string out;
    do {
        int len = 0;
        if (::EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
            ::EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1 ||
            ::EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char*>(key.data()),
                                 reinterpret_cast<const unsigned char*>(nonce.data())) != 1) {
            break;
        }
        if (!aad.empty() &&
            ::EVP_DecryptUpdate(ctx, nullptr, &len,
                                reinterpret_cast<const unsigned char*>(aad.data()),
                                static_cast<int>(aad.size())) != 1) {
            break;
        }
        out.resize(ct.size());
        int total = 0;
        if (!ct.empty() &&
            ::EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&out[0]), &len,
                                reinterpret_cast<const unsigned char*>(ct.data()),
                                static_cast<int>(ct.size())) != 1) {
            out.clear();
            break;
        }
        total = len;
        unsigned char tag_copy[kTagSize];
        std::memcpy(tag_copy, tag, kTagSize);
        if (::EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize, tag_copy) != 1) {
            out.clear();
            break;
        }
        if (::EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]) + total,
                                  &len) != 1) {
            out.clear();  // 认证失败
            break;
        }
        out.resize(static_cast<std::size_t>(total) + len);
    } while (false);
    ::EVP_CIPHER_CTX_free(ctx);
    return out;
}

#endif  // _WIN32

std::string Aes256Gcm::seal(const std::string& key, const std::string& plaintext,
                            const std::string& aad)
{
    if (key.size() != kKeySize) {
        return "";
    }
    // 随机 nonce：random_string 的字符池不适合二进制，直接用随机字节串
    std::string nonce;
    for (std::size_t i = 0; i < kNonceSize; i += 4) {
        const std::uint32_t r = static_cast<std::uint32_t>(random_int(0, 0x7fffffff)) ^
                                (static_cast<std::uint32_t>(random_int(0, 0x7fffffff)) << 1);
        nonce.append(reinterpret_cast<const char*>(&r), sizeof(r));
    }
    nonce.resize(kNonceSize);
    const std::string ct = encrypt(key, nonce, plaintext, aad);
    if (ct.empty()) {
        return "";
    }
    return nonce + ct;
}

std::string Aes256Gcm::open(const std::string& key, const std::string& sealed,
                            const std::string& aad)
{
    if (sealed.size() < kNonceSize + kTagSize) {
        return "";
    }
    const std::string nonce = sealed.substr(0, kNonceSize);
    const std::string ct = sealed.substr(kNonceSize);
    return decrypt(key, nonce, ct, aad);
}

}  // namespace libmini
