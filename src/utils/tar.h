#ifndef LIBMINI_TAR_H
#define LIBMINI_TAR_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "libmini.h"

namespace libmini {

// tar（POSIX ustar）读写（POSIX.1-1988 ustar 子集，纯实现不引依赖）。
//   - 写入：常规文件、目录、符号链接；文件名经 prefix/name 拆分支持到 255 字节
//   - 读取：校验头部 checksum，支持 ustar prefix 拼接；数据按 512 字节块对齐
//   - 文件名按原始字节存取（UTF-8 中文名原样往返）
//
//   TarWriter tw;
//   tw.add_file("README.md", text);
//   tw.add_dir("docs");
//   tw.add_symlink("link", "README.md");
//   std::string bytes = tw.finish();
//
//   TarReader tr;
//   if (tr.open(bytes)) {
//       for (const auto& e : tr.entries()) use(e.name, e.size);
//       std::string text = tr.extract("README.md");   // 失败返回空 + last_error()
//   }

// typeflag 取值（ustar）
enum class TarType : char {
    Regular = '0',       // 常规文件（typeflag '0' 或 '\0' 都算）
    HardLink = '1',
    Symlink = '2',
    Directory = '5',
};

struct TarEntryInfo
{
    std::string name;                // UTF-8 文件名（prefix 已拼接）
    std::uint64_t size = 0;          // 数据字节数（目录/链接为 0）
    TarType type = TarType::Regular;
    std::uint32_t mode = 0;          // 原始 mode 字段（八位权限）
    std::int64_t mtime = 0;          // 修改时间（Unix 秒）
    std::string link_target;         // 符号链接目标（非链接为空）
};

class LIBMINI_API TarWriter
{
public:
    TarWriter();

    // 追加常规文件。data 为文件原始内容
    bool add_file(const std::string& name, const std::string& data,
                  std::uint32_t mode = 0644);

    // 追加目录项（name 会规范化为以 '/' 结尾）
    bool add_dir(const std::string& name, std::uint32_t mode = 0755);

    // 追加符号链接
    bool add_symlink(const std::string& name, const std::string& target,
                     std::uint32_t mode = 0777);

    // 收尾并输出完整 tar 字节流（调用后不可再 add）
    std::string finish();

    // 已追加条目数
    std::size_t count() const;

    const std::string& last_error() const { return error_; }

private:
    bool add_header(const std::string& name, const std::string& link,
                    TarType type, std::uint64_t size, std::uint32_t mode);

    std::string data_;               // 已写出的头部与数据块
    std::size_t count_ = 0;
    std::string error_;
};

class LIBMINI_API TarReader
{
public:
    TarReader();

    // 解析 tar 字节流（内存 tar，不落盘）。失败返回 false + last_error()
    bool open(const std::string& tar_bytes);

    // 目录（按出现顺序）
    const std::vector<TarEntryInfo>& entries() const;

    // 是否包含某条目（精确匹配名字；目录名带尾部 '/'）
    bool contains(const std::string& name) const;

    // 解压指定文件；失败返回空串（last_error() 说明原因）
    std::string extract(const std::string& name);

    // 提取指定序号的文件（与 entries() 顺序一致）
    std::string extract_at(std::size_t index);

    const std::string& last_error() const { return error_; }

private:
    std::string data_;  // 原始 tar 字节（extract 回读用）
    std::vector<TarEntryInfo> entries_;
    std::vector<std::pair<std::size_t, std::size_t>> data_spans_;  // 数据偏移/长度
    std::string error_;
};

}  // namespace libmini

#endif  // LIBMINI_TAR_H
