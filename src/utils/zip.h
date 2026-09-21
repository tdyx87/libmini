#ifndef LIBMINI_ZIP_H
#define LIBMINI_ZIP_H

#include <cstdint>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// ZIP 压缩包读写（PKZIP APPNOTE 6.3.9 子集，基于已有 zlib 依赖）。
//   - 写入：store（已压缩数据/二进制直接存）与 deflate（文本/可压缩数据）两种方法
//   - 读取：自动识别 store/deflate，CRC 校验
//   - 文件名用 UTF-8（设置通用位标志 bit 11），中文文件名跨平台兼容
//
//   ZipWriter zw;
//   zw.add_file("README.md", readme_text);
//   zw.add_file("data.bin", binary_blob, ZipMethod::Store);
//   std::string zip_bytes = zw.finish();
//
//   ZipReader zr;
//   if (zr.open(zip_bytes)) {
//       for (const auto& e : zr.entries()) use(e.name, e.uncompressed_size);
//       std::string text = zr.extract("README.md");   // 失败返回空 + last_error()
//   }

enum class ZipMethod { Store, Deflate };

struct ZipEntryInfo
{
    std::string name;              // UTF-8 文件名
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint32_t crc32 = 0;
    ZipMethod method = ZipMethod::Deflate;
};

class LIBMINI_API ZipWriter
{
public:
    ZipWriter();
    ~ZipWriter();

    // 追加一个文件。data 为文件原始内容（未压缩）
    bool add_file(const std::string& name, const std::string& data,
                  ZipMethod method = ZipMethod::Deflate);

    // 收尾并输出完整 zip 字节流（调用后不可再 add）
    std::string finish();

    // 已追加文件数
    std::size_t count() const;

private:
    struct Impl;
    Impl* impl_;
};

class LIBMINI_API ZipReader
{
public:
    ZipReader();
    ~ZipReader();

    // 解析 zip 字节流（内存 zip，不落盘）。失败返回 false + last_error()
    bool open(const std::string& zip_bytes);

    // 目录
    const std::vector<ZipEntryInfo>& entries() const;

    // 是否包含某文件（精确匹配文件名）
    bool contains(const std::string& name) const;

    // 解压指定文件；失败返回空串（last_error() 说明原因）
    std::string extract(const std::string& name);

    // 提取指定序号的文件（与 entries() 顺序一致）
    std::string extract_at(std::size_t index);

    const std::string& last_error() const { return error_; }

private:
    std::string data_;
    std::vector<ZipEntryInfo> entries_;
    std::vector<std::pair<std::size_t, std::size_t>> offsets_;  // local header/数据起点
    std::string error_;
};

}  // namespace libmini

#endif  // LIBMINI_ZIP_H
