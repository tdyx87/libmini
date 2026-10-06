#include "zstd.h"

#include <zstd.h>
#include <zstd_errors.h>

namespace libmini {

optional<std::string> zstd_compress(const std::string& data, int level)
{
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel()) {
        return nullopt;
    }

    const std::size_t bound = ZSTD_compressBound(data.size());
    if (bound == 0) {
        return nullopt;  // ZSTD 溢出检查：输入过大
    }

    std::string out;
    out.resize(bound);
    const std::size_t n =
        ZSTD_compress(&out[0], bound, data.data(), data.size(), level);
    if (ZSTD_isError(n)) {
        return nullopt;
    }
    out.resize(n);
    return optional<std::string>(std::move(out));
}

optional<std::string> zstd_decompress(const std::string& compressed,
                                      std::size_t max_output)
{
    if (compressed.empty()) {
        return nullopt;  // 空输入不是合法 zstd 帧
    }

    // 帧头声明的 content size（未知则为 ZSTD_CONTENTSIZE_UNKNOWN）
    const unsigned long long known =
        ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (known == ZSTD_CONTENTSIZE_ERROR) {
        return nullopt;  // 不是 zstd 帧
    }

    // 已知声明体积：一次性按声明分配；未知：从 4 KiB 起翻倍重试
    // （ZSTD_decompress 是一次性 API，dstSize_tooSmall 时带更大缓冲整体重试）
    std::size_t cap;
    if (known != ZSTD_CONTENTSIZE_UNKNOWN && known != ZSTD_CONTENTSIZE_ERROR) {
        if (max_output != 0 && known > max_output) {
            return nullopt;  // 声明体积已超上限 → 直接拒绝（不分配）
        }
        cap = static_cast<std::size_t>(known);
    } else {
        cap = 4096;
    }
    if (cap == 0) {
        cap = 1;  // 空帧：避免空串上的 &out[0]
    }

    std::string out;
    for (;;) {
        out.resize(cap);
        const std::size_t n =
            ZSTD_decompress(&out[0], out.size(), compressed.data(),
                            compressed.size());
        if (!ZSTD_isError(n)) {
            if (max_output != 0 && n > max_output) {
                return nullopt;
            }
            out.resize(n);
            return optional<std::string>(std::move(out));
        }
        if (ZSTD_getErrorCode(n) != ZSTD_error_dstSize_tooSmall) {
            return nullopt;  // 数据损坏/截断/非 zstd 数据
        }
        // 输出缓冲不足：翻倍后整体重试
        if (max_output != 0 && cap >= max_output) {
            return nullopt;  // 到上限仍不够 → 疑似压缩炸弹
        }
        std::size_t grown = cap * 2;
        if (grown <= cap) {
            return nullopt;  // 溢出
        }
        if (max_output != 0 && grown > max_output) {
            grown = max_output;
        }
        cap = grown;
    }
}

}  // namespace libmini
