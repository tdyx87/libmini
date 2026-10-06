#ifndef LIBMINI_ZSTD_H
#define LIBMINI_ZSTD_H

#include <cstddef>
#include <string>

#include "libmini.h"
#include "optional.h"

namespace libmini {

// zstd 压缩/解压（基于已链接的 zstd 库）。
// 返回 optional：失败（内存不足、数据损坏/截断、超出 max_output）为 nullopt。
//   - 压缩输出为标准 zstd 帧（magic 28 B5 2F FD），可被 zstd CLI / 其它实现读取
//   - 解压自动读帧头的 content size 预分配；未知时流式扩容
//   - 解压带防炸弹上限 max_output（默认 1 GiB），0 表示不限制
//
//   optional<std::string> packed = zstd_compress(blob, 9);   // level 1..22
//   optional<std::string> raw = zstd_decompress(*packed);
LIBMINI_API optional<std::string> zstd_compress(const std::string& data,
                                                int level = 3);
LIBMINI_API optional<std::string> zstd_decompress(
    const std::string& compressed, std::size_t max_output = 1u << 30);

}  // namespace libmini

#endif  // LIBMINI_ZSTD_H
