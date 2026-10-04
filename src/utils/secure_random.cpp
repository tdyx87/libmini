#include "secure_random.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <cstdio>
#if defined(__linux__)
#include <sys/random.h>   // getrandom(2)，glibc >= 2.25
#include <unistd.h>
#else
#include <stdlib.h>       // arc4random_buf / arc4random_uniform
#include <unistd.h>
#endif
#endif

namespace libmini {

namespace {

#ifdef _WIN32

bool fill_from_system(void* buffer, std::size_t size)
{
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG：不要 BCryptGenRandom 的算法级提供者
    // （SHA1/256/512），它们是「确定性哈希」不是熵源，直接取系统首选即可。
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr, static_cast<PUCHAR>(buffer),
        static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
}

#elif defined(__linux__)

bool fill_from_system(void* buffer, std::size_t size)
{
    unsigned char* out = static_cast<unsigned char*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::getrandom(out + done, size - done, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // 被信号打断，正常情况
            }
            // 内核太老（无 getrandom）或被 seccomp 屏蔽：退回 /dev/urandom。
            // 不是「弱」退路——/dev/urandom 同样是内核 CSPRNG，只是不走 syscall
            // 快路径；读满为止即成功。
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (done == size) {
        return true;
    }
    std::FILE* fp = std::fopen("/dev/urandom", "rb");
    if (fp == nullptr) {
        return false;
    }
    const std::size_t n = std::fread(out + done, 1, size - done, fp);
    std::fclose(fp);
    return n == size - done;
}

#else  // macOS

bool fill_from_system(void* buffer, std::size_t size)
{
    if (size == 0) {
        return true;
    }
    arc4random_buf(buffer, size);
    return true;
}

#endif

// 字母表小于 256 时的无偏取样：拒绝掉 >= (256 / n) * n 的字节
bool sample_index(const std::string& alphabet, std::size_t& out_index)
{
    const std::size_t n = alphabet.size();
    const unsigned int limit = 256u - (256u % static_cast<unsigned int>(n));
    for (;;) {
        unsigned char byte = 0;
        if (!secure_random_bytes(&byte, 1)) {
            return false;
        }
        if (static_cast<unsigned int>(byte) < limit) {
            out_index = static_cast<std::size_t>(byte) % n;
            return true;
        }
        // 落在拒绝区：重取。拒绝概率最多 (n-1)/256，不构成性能问题
    }
}

const char kHexDigits[] = "0123456789abcdef";
const char kTokenAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace

bool secure_random_bytes(void* buffer, std::size_t size)
{
    if (size == 0) {
        return true;
    }
    if (buffer == nullptr) {
        return false;
    }
    return fill_from_system(buffer, size);
}

std::string secure_random_string(std::size_t size)
{
    std::string out;
    if (size == 0) {
        return out;
    }
    out.resize(size);
    if (!secure_random_bytes(&out[0], size)) {
        return std::string();
    }
    return out;
}

std::string secure_random_chars(std::size_t length,
                                const std::string& alphabet)
{
    std::string out;
    if (length == 0 || alphabet.empty() || alphabet.size() > 256) {
        return out;
    }
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        std::size_t index = 0;
        if (!sample_index(alphabet, index)) {
            return std::string();
        }
        out.push_back(alphabet[index]);
    }
    return out;
}

std::string secure_random_hex(std::size_t byte_count)
{
    if (byte_count == 0) {
        return std::string();
    }
    const std::string raw = secure_random_string(byte_count);
    if (raw.empty()) {
        return std::string();
    }
    std::string out;
    out.reserve(byte_count * 2);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(raw[i]);
        out.push_back(kHexDigits[(byte >> 4) & 0x0F]);
        out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
}

std::uint64_t secure_random_u64()
{
    std::uint64_t value = 0;
    if (!secure_random_bytes(&value, sizeof(value))) {
        return 0;
    }
    return value;
}

std::string secure_token(std::size_t byte_count)
{
    return secure_random_chars(byte_count, kTokenAlphabet);
}

}  // namespace libmini