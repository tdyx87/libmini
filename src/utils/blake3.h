#ifndef LIBMINI_BLAKE3_H
#define LIBMINI_BLAKE3_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "libmini.h"

namespace libmini {

// BLAKE3 摘要（官方规范实现，零第三方依赖）。
//
// 三种模式（规范 C2SP/BLAKE3 §1.1）：
//   * hash        —— 无键哈希，SHA-2 的现代替代（碰撞/抗原像 128-bit 起）
//   * keyed_hash  —— 32 字节密钥的 PRF/MAC，可替代 HMAC
//   * derive_key  —— KDF：context 串先哈希出中间密钥，再哈希密钥材料，
//                    可替代 HKDF（context 应硬编码、全局唯一、应用特定）
//
// 与 Md5/Sha256 一样是纯 C++ 自带实现（同样的理由：三个平台逐字节一致，
// 且摘要算法没有会失败的平台资源）。树形结构按规范的 CV 栈组织：1024 字节
// chunk 为叶子，非 2 的幂的 chunk 数按「左子树满且大」规则合并。
//
// XOF：输出不承诺长度，短输出是长输出的前缀（规范 §4.4，根压缩仅 t 递增）。
//
//   std::string d = Blake3::hex(data);              // 64 字符小写十六进制
//   Blake3 h; h.update(part1); h.update(part2);
//   std::string raw = h.finish();                   // 32 字节原始摘要
//   std::string key = "0123...";                    // 恰好 32 字节
//   std::string mac = Blake3::keyed_hex(key, msg);  // "" 表示 key 长度非法
class LIBMINI_API Blake3 {
public:
    Blake3() { reset(); }

    // 回到无键 hash 模式（finish() 之后复用前也调它）
    void reset();

    // keyed_hash 模式：key 必须恰好 32 字节；长度不符返回 false 且
    // 状态保持不变（对象仍处于原模式）
    bool reset_keyed(const std::string& key);

    // derive_key 模式：立即用 context 走完阶段一（DERIVE_KEY_CONTEXT），
    // 之后 update() 的数据即阶段二的密钥材料
    void reset_derive(const std::string& context);

    void update(const void* data, std::size_t size);
    void update(const std::string& data) { update(data.data(), data.size()); }

    // 结束并输出默认 32 字节摘要（之后需 reset() 才能复用）
    std::string finish();

    // XOF 变长输出：任意字节数（0 表示空串）。前 N 字节恒等于
    // finish(N) 截断，也恒等于默认 32 字节输出的前缀
    std::string finish(std::size_t out_len);

    // 一次性计算，返回小写十六进制
    static std::string hex(const std::string& data);
    // keyed 变体：key 长度非法时返回空串
    static std::string keyed_hex(const std::string& key,
                                 const std::string& data);
    static std::string derive_hex(const std::string& context,
                                  const std::string& data);

private:
    // CV 栈条目：子树的链接值 + 该子树覆盖的 chunk 数（配对合并用）
    struct Entry {
        std::uint32_t cv[8];
        std::uint64_t chunks;
    };

    void init_mode(const std::uint32_t key[8], std::uint32_t mode_flag);
    std::uint32_t start_flag() const;
    // 把已满 1024 字节的当前 chunk 收尾：最后一块压缩（CHUNK_END）→
    // CV 入栈（按大小配对合并）→ 开新 chunk
    void finish_chunk();
    // 父节点压缩（非根）：left||right 两段 32 字节 CV 拼成 64 字节消息
    void parent_cv(const std::uint32_t left[8], const std::uint32_t right[8],
                   std::uint32_t out[8]) const;

    std::uint32_t key_[8];      // 模式密钥：IV / 用户 key / 派生密钥
    std::uint32_t mode_flag_;   // KEYED_HASH / DERIVE_KEY_* 域标志
    std::uint32_t cv_[8];       // 当前 chunk 的链接值
    std::uint64_t chunk_counter_;
    std::uint8_t block_[64];
    std::size_t block_len_;
    std::size_t blocks_compressed_;
    Entry stack_[64];           // 深度 ≤ 63（2^54 个 chunk 也够用，无堆分配）
    std::size_t stack_len_;
};

}  // namespace libmini

#endif  // LIBMINI_BLAKE3_H
