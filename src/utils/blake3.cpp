#include "blake3.h"

#include <cstring>

#include "encoding.h"

namespace libmini {

namespace {

// 规范 §2.2：BLAKE3 的 IV = SHA-256 的 IV
const std::uint32_t kIV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

// 规范 §2.3：消息字置换（每轮后施加）
const std::uint8_t kPermute[16] = {2, 6,  3,  10, 7,  0,  4,  13,
                                   1, 11, 12, 5,  9,  14, 15, 8};

// 规范 §2.4：压缩函数域分隔标志
const std::uint32_t kChunkStart = 0x01;
const std::uint32_t kChunkEnd = 0x02;
const std::uint32_t kParent = 0x04;
const std::uint32_t kRoot = 0x08;
const std::uint32_t kKeyedHash = 0x10;
const std::uint32_t kDeriveKeyContext = 0x20;
const std::uint32_t kDeriveKeyMaterial = 0x40;

inline std::uint32_t rotr32(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

// 规范 §3.2：四分之一轮 G
inline void g(std::uint32_t v[16], int a, int b, int c, int d,
              std::uint32_t x, std::uint32_t y)
{
    v[a] = v[a] + v[b] + x;
    v[d] = rotr32(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = rotr32(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y;
    v[d] = rotr32(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];
    v[b] = rotr32(v[b] ^ v[c], 7);
}

// 规范 §3.3：恰好 7 轮压缩。out 收满 16 个 32 位字（非根输出取前 8 个）
void compress(const std::uint32_t h[8], const std::uint32_t msg[16],
              std::uint64_t t, std::uint32_t len, std::uint32_t flags,
              std::uint32_t out[16])
{
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = msg[i];
    }

    std::uint32_t v[16] = {h[0],
                           h[1],
                           h[2],
                           h[3],
                           h[4],
                           h[5],
                           h[6],
                           h[7],
                           kIV[0],
                           kIV[1],
                           kIV[2],
                           kIV[3],
                           static_cast<std::uint32_t>(t),
                           static_cast<std::uint32_t>(t >> 32),
                           len,
                           flags};

    for (int round = 0; round < 7; ++round) {
        g(v, 0, 4, 8, 12, m[0], m[1]);
        g(v, 1, 5, 9, 13, m[2], m[3]);
        g(v, 2, 6, 10, 14, m[4], m[5]);
        g(v, 3, 7, 11, 15, m[6], m[7]);
        g(v, 0, 5, 10, 15, m[8], m[9]);
        g(v, 1, 6, 11, 12, m[10], m[11]);
        g(v, 2, 7, 8, 13, m[12], m[13]);
        g(v, 3, 4, 9, 14, m[14], m[15]);

        std::uint32_t permuted[16];
        for (int i = 0; i < 16; ++i) {
            permuted[i] = m[kPermute[i]];
        }
        for (int i = 0; i < 16; ++i) {
            m[i] = permuted[i];
        }
    }

    for (int i = 0; i < 8; ++i) {
        out[i] = v[i] ^ v[i + 8];
        out[i + 8] = v[i + 8] ^ h[i];
    }
}

// 块 → 16 个 little-endian 字；len 之外的部分按规范补零
inline void load_block(const std::uint8_t* block, std::size_t len,
                       std::uint32_t m[16])
{
    for (int i = 0; i < 16; ++i) {
        std::uint32_t w = 0;
        for (int j = 0; j < 4; ++j) {
            const std::size_t off = static_cast<std::size_t>(i * 4 + j);
            if (off < len) {
                w |= static_cast<std::uint32_t>(block[off]) << (8 * j);
            }
        }
        m[i] = w;
    }
}

inline std::uint32_t load32_le(const char* p)
{
    const unsigned char* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(u[0]) |
           (static_cast<std::uint32_t>(u[1]) << 8) |
           (static_cast<std::uint32_t>(u[2]) << 16) |
           (static_cast<std::uint32_t>(u[3]) << 24);
}

}  // namespace

void Blake3::init_mode(const std::uint32_t key[8], std::uint32_t mode_flag)
{
    for (int i = 0; i < 8; ++i) {
        key_[i] = key[i];
        cv_[i] = key[i];
    }
    mode_flag_ = mode_flag;
    chunk_counter_ = 0;
    block_len_ = 0;
    blocks_compressed_ = 0;
    stack_len_ = 0;
    std::memset(block_, 0, sizeof(block_));
    std::memset(stack_, 0, sizeof(stack_));
}

void Blake3::reset()
{
    init_mode(kIV, 0);
}

bool Blake3::reset_keyed(const std::string& key)
{
    if (key.size() != 32) {
        return false;  // 状态保持不变
    }
    std::uint32_t k[8];
    for (int i = 0; i < 8; ++i) {
        k[i] = load32_le(key.data() + i * 4);
    }
    init_mode(k, kKeyedHash);
    return true;
}

void Blake3::reset_derive(const std::string& context)
{
    // 阶段一（DERIVE_KEY_CONTEXT）：以 IV 为 key 对 context 串做一次
    // 完整 BLAKE3，输出前 32 字节即阶段二的密钥
    Blake3 ctx;
    ctx.mode_flag_ = kDeriveKeyContext;
    ctx.update(context);
    const std::string derived = ctx.finish(32);

    std::uint32_t k[8];
    for (int i = 0; i < 8; ++i) {
        k[i] = load32_le(derived.data() + i * 4);
    }
    init_mode(k, kDeriveKeyMaterial);
}

std::uint32_t Blake3::start_flag() const
{
    return blocks_compressed_ == 0 ? kChunkStart : 0;
}

void Blake3::parent_cv(const std::uint32_t left[8], const std::uint32_t right[8],
                       std::uint32_t out[8]) const
{
    std::uint32_t m[16];
    for (int i = 0; i < 8; ++i) {
        m[i] = left[i];
        m[8 + i] = right[i];
    }
    std::uint32_t full[16];
    compress(key_, m, 0, 64, kParent | mode_flag_, full);
    for (int i = 0; i < 8; ++i) {
        out[i] = full[i];
    }
}

void Blake3::finish_chunk()
{
    std::uint32_t m[16];
    load_block(block_, block_len_, m);

    std::uint32_t full[16];
    compress(cv_, m, chunk_counter_, static_cast<std::uint32_t>(block_len_),
             start_flag() | kChunkEnd | mode_flag_, full);

    // 新的子树 CV（覆盖 1 个 chunk）按大小配对压栈：与栈顶等大就合并成
    // 父节点——这正是规范「左子树满且大」的增量形式
    std::uint32_t cur[8];
    for (int i = 0; i < 8; ++i) {
        cur[i] = full[i];
    }
    std::uint64_t chunks = 1;
    while (stack_len_ > 0 && stack_[stack_len_ - 1].chunks == chunks) {
        const Entry e = stack_[--stack_len_];
        std::uint32_t merged[8];
        parent_cv(e.cv, cur, merged);
        for (int i = 0; i < 8; ++i) {
            cur[i] = merged[i];
        }
        chunks *= 2;
    }
    for (int i = 0; i < 8; ++i) {
        stack_[stack_len_].cv[i] = cur[i];
    }
    stack_[stack_len_].chunks = chunks;
    ++stack_len_;

    // 开新 chunk：链接值回到模式密钥，chunk 计数 +1
    for (int i = 0; i < 8; ++i) {
        cv_[i] = key_[i];
    }
    ++chunk_counter_;
    blocks_compressed_ = 0;
    block_len_ = 0;
}

void Blake3::update(const void* data, std::size_t size)
{
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        if (block_len_ == 64) {
            // 缓冲满：还有后续数据就先把这一页消化掉。第 16 页填满意味着
            // 当前 chunk 已满 1024 字节 → 收尾入栈换新 chunk
            if (blocks_compressed_ == 15) {
                finish_chunk();
            } else {
                std::uint32_t m[16];
                load_block(block_, 64, m);
                std::uint32_t full[16];
                compress(cv_, m, chunk_counter_, 64,
                         start_flag() | mode_flag_, full);
                for (int i = 0; i < 8; ++i) {
                    cv_[i] = full[i];
                }
                ++blocks_compressed_;
                block_len_ = 0;
            }
        }
        std::size_t take = 64 - block_len_;
        if (take > size) {
            take = size;
        }
        std::memcpy(block_ + block_len_, p, take);
        block_len_ += take;
        p += take;
        size -= take;
    }
}

std::string Blake3::finish()
{
    return finish(32);
}

std::string Blake3::finish(std::size_t out_len)
{
    // 确定根节点的压缩输入（规范 §4.3/§4.4）：随后仅让 t 递增重复压缩，
    // 逐块吐出任意长度输出
    std::uint32_t root_cv[8];
    std::uint32_t root_m[16];
    std::uint32_t root_len;
    std::uint32_t root_flags;
    std::uint64_t root_t0;

    if (stack_len_ == 0) {
        // 只有一个 chunk：该 chunk 即根（规范 §4.3.2 第一条）
        for (int i = 0; i < 8; ++i) {
            root_cv[i] = cv_[i];
        }
        load_block(block_, block_len_, root_m);
        root_len = static_cast<std::uint32_t>(block_len_);
        root_flags = start_flag() | kChunkEnd | kRoot | mode_flag_;
        root_t0 = chunk_counter_;  // 单 chunk 恒为 0，保留一般性
    } else {
        // 最后一个 chunk 先算出 CV（尚不带 ROOT）
        std::uint32_t m[16];
        load_block(block_, block_len_, m);
        std::uint32_t full[16];
        compress(cv_, m, chunk_counter_, static_cast<std::uint32_t>(block_len_),
                 start_flag() | kChunkEnd | mode_flag_, full);
        std::uint32_t cur[8];
        for (int i = 0; i < 8; ++i) {
            cur[i] = full[i];
        }

        // 自上而下弹栈合并：弹出的子树在左，当前值在右（栈中越靠底越早）
        while (stack_len_ > 1) {
            const Entry e = stack_[--stack_len_];
            std::uint32_t merged[8];
            parent_cv(e.cv, cur, merged);
            for (int i = 0; i < 8; ++i) {
                cur[i] = merged[i];
            }
        }
        // 栈底 = 最左子树，与当前值组成根父节点
        for (int i = 0; i < 8; ++i) {
            root_cv[i] = key_[i];
            root_m[i] = stack_[0].cv[i];
            root_m[8 + i] = cur[i];
        }
        stack_len_ = 0;
        root_t0 = 0;
        root_len = 64;
        root_flags = kParent | kRoot | mode_flag_;
    }

    std::string out;
    out.reserve(out_len);
    std::size_t block_index = 0;
    while (out.size() < out_len) {
        std::uint32_t o[16];
        compress(root_cv, root_m, root_t0 + block_index, root_len, root_flags,
                 o);
        for (int w = 0; w < 16 && out.size() < out_len; ++w) {
            for (int b = 0; b < 4 && out.size() < out_len; ++b) {
                out.push_back(
                    static_cast<char>((o[w] >> (8 * b)) & 0xFF));
            }
        }
        ++block_index;
    }
    return out;
}

std::string Blake3::hex(const std::string& data)
{
    Blake3 h;
    h.update(data);
    return Hex::encode(h.finish(), true);
}

std::string Blake3::keyed_hex(const std::string& key, const std::string& data)
{
    Blake3 h;
    if (!h.reset_keyed(key)) {
        return std::string();
    }
    h.update(data);
    return Hex::encode(h.finish(), true);
}

std::string Blake3::derive_hex(const std::string& context,
                               const std::string& data)
{
    Blake3 h;
    h.reset_derive(context);
    h.update(data);
    return Hex::encode(h.finish(), true);
}

}  // namespace libmini
