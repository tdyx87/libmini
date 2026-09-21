#include "gzip.h"

#include <cstring>

#include <zlib.h>

namespace libmini {

optional<std::string> gzip_compress(const std::string& data, int level)
{
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    // windowBits = 15 + 16 → gzip 包装
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return nullopt;
    }

    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    strm.avail_in = static_cast<uInt>(data.size());

    std::string out;
    std::size_t produced = 0;
    out.resize(1024);
    int ret;
    do {
        if (produced == out.size()) {
            out.resize(out.size() * 2);
        }
        strm.next_out = reinterpret_cast<Bytef*>(&out[0]) + produced;
        strm.avail_out = static_cast<uInt>(out.size() - produced);
        ret = deflate(&strm, Z_FINISH);
        produced = out.size() - strm.avail_out;
    } while (ret == Z_OK);

    const bool ok = (ret == Z_STREAM_END);
    deflateEnd(&strm);
    if (!ok) {
        return nullopt;
    }
    out.resize(produced);
    return optional<std::string>(std::move(out));
}

optional<std::string> gzip_decompress(const std::string& compressed)
{
    if (compressed.empty()) {
        return nullopt;
    }

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    // windowBits = 15 + 32 → 自动识别 gzip 与 zlib 包装
    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        return nullopt;
    }

    strm.next_in =
        reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    strm.avail_in = static_cast<uInt>(compressed.size());

    std::string out;
    std::size_t produced = 0;
    out.resize(1024);
    int ret;
    for (;;) {
        if (produced == out.size()) {
            out.resize(out.size() * 2);
        }
        strm.next_out = reinterpret_cast<Bytef*>(&out[0]) + produced;
        strm.avail_out = static_cast<uInt>(out.size() - produced);
        ret = inflate(&strm, Z_NO_FLUSH);
        produced = out.size() - strm.avail_out;

        if (ret == Z_STREAM_END) {
            inflateEnd(&strm);
            out.resize(produced);
            return optional<std::string>(std::move(out));
        }
        if (ret == Z_BUF_ERROR && strm.avail_out > 0) {
            break;  // 输入耗尽但流未结束 → 截断/损坏
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            break;  // 数据损坏
        }
        // Z_OK（继续产出）或 Z_BUF_ERROR 且输出满（扩容后继续）
    }

    inflateEnd(&strm);
    return nullopt;
}

}  // namespace libmini
