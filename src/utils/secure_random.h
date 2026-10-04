#ifndef LIBMINI_SECURE_RANDOM_H
#define LIBMINI_SECURE_RANDOM_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "export.h"

namespace libmini {

// 密码学安全的随机源。
//
// 与 utils/random_utils.h 的分工（别用错，这是本模块存在的理由）：
//   random_utils —— std::mt19937 + random_device 播种，供「需要一点随机性」
//                   的场景用：洗牌、抽样、临时文件名、测试数据。它是
//                   **可预测**的：给定 624 个 32 位输出即可还原内部状态，
//                   且 MSVC/MinGW 上的 std::random_device 本身也不是
//                   密码学实现（libstdc++ 会退回 mt19937）。
//   secure_random —— 系统 CSPRNG（Windows BCryptGenRandom、Linux getrandom、
//                   macOS arc4random_buf）。凡是与密钥、nonce、token、
//                   会话 ID 沾边的东西，都必须走这里。
//
// 失败语义：与库里其他模块一致，返回 bool / 空串，不抛异常。
// 系统熵源不可用（容器里 /dev/urandom 被禁、精简内核无 getrandom）时
// secure_random_bytes 返回 false，调用方必须检查——静默退化成弱随机比
// 直接失败更危险。

// 填充 size 字节密码学随机数据到 buffer。size 为 0 视为成功（无操作）。
// buffer 为空且 size > 0 返回 false。
LIBMINI_API bool secure_random_bytes(void* buffer, std::size_t size);

// 便捷：返回 size 字节的随机串（**可能含 NUL**，按二进制用）
LIBMINI_API std::string secure_random_string(std::size_t size);

// 便捷：从 alphabet 里等概率取 length 个字符。
// 用拒绝采样消除取模偏置——直接 % alphabet.size() 会让靠前的字符多出现
// （256 不是字符数的倍数时偏差可达 1/256，用于口令就是可测的弱点）。
// alphabet 为空返回空串。
LIBMINI_API std::string secure_random_chars(std::size_t length,
                                            const std::string& alphabet);

// 便捷：byte_count 字节随机数的十六进制文本（输出小写，长度 = byte_count*2）
LIBMINI_API std::string secure_random_hex(std::size_t byte_count = 16);

// 便捷：单个 64 位随机数
LIBMINI_API std::uint64_t secure_random_u64();

// 便捷：可直接当口令/令牌用的随机串（Base64url 字母表，默认 32 字节 = 256 bit）
LIBMINI_API std::string secure_token(std::size_t byte_count = 32);

}  // namespace libmini

#endif  // LIBMINI_SECURE_RANDOM_H