#ifndef LIBMINI_GZIP_H
#define LIBMINI_GZIP_H

#include <string>

#include "libmini.h"
#include "optional.h"

namespace libmini {

// gzip 压缩/解压（基于已链接的 zlib）。
// 返回 optional：失败（内存不足、数据损坏/截断）时为 nullopt。
// 压缩输出为标准 .gzip 格式（含头尾），可直接写 .gz 文件；
// 解压同时兼容 gzip 与 zlib 包装的流。
LIBMINI_API optional<std::string> gzip_compress(const std::string& data,
                                                int level = 6);
LIBMINI_API optional<std::string> gzip_decompress(const std::string& compressed);

}  // namespace libmini

#endif  // LIBMINI_GZIP_H
