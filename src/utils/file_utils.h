#ifndef LIBMINI_FILE_UTILS_H
#define LIBMINI_FILE_UTILS_H

#include <cstdint>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 路径统一按 UTF-8 编码处理（Windows 走 W 版 API，中文路径无代码页问题）。

// 检查路径是否存在（文件或目录均算存在）
LIBMINI_API bool file_exists(const std::string& path);

// 读取文件内容；打不开/出错返回空串
LIBMINI_API std::string read_file(const std::string& path);

// 写入文件内容（覆盖已有文件）
LIBMINI_API bool write_file(const std::string& path, const std::string& content);

// 向文件末尾追加内容（文件不存在则创建）
LIBMINI_API bool append_file(const std::string& path, const std::string& content);

// 获取文件大小；失败返回 0
LIBMINI_API size_t file_size(const std::string& path);

// 列出目录内容（仅文件/子目录名，不含 "." 与 ".."）；目录不存在返回空
LIBMINI_API std::vector<std::string> list_directory(const std::string& path);

// 删除文件（失败返回 false）
LIBMINI_API bool remove_file(const std::string& path);

// 删除空目录（失败返回 false）
LIBMINI_API bool remove_directory(const std::string& path);

// ------------------ 目录创建 ------------------

// 创建单级目录（父目录必须已存在）
LIBMINI_API bool make_directory(const std::string& path);

// 递归创建目录链（"a/b/c" 自动补齐 a、a/b），已存在视为成功
LIBMINI_API bool make_directories(const std::string& path);

// ------------------ 复制 / 移动 / 递归删除 ------------------

// 复制文件；目标已存在时覆盖
LIBMINI_API bool copy_file(const std::string& from, const std::string& to);

// 递归复制目录树（含子目录与全部文件）；
// 目标已存在且为目录时并入其中（复制为 to/basename(from)）
LIBMINI_API bool copy_tree(const std::string& from, const std::string& to);

// 重命名/移动文件或目录（同盘符内原子操作）；目标已存在时失败
LIBMINI_API bool rename_path(const std::string& from, const std::string& to);

// 移动文件或目录树：优先 rename，跨盘符时回退为复制+删除
LIBMINI_API bool move_path(const std::string& from, const std::string& to);

// 递归删除目录及其全部内容；path 也可以是文件（此时等价 remove_file）
LIBMINI_API bool remove_tree(const std::string& path);

// ------------------ 目录项与属性 ------------------

enum class EntryKind {
    File,
    Directory,
    Symlink,
    Other,
};

struct DirEntry
{
    std::string name;      // 文件/目录名（不含路径）
    std::string path;      // 完整路径（dir + 分隔符 + name）
    EntryKind kind;        // 类型
    std::uint64_t size;    // 字节（目录为 0）
    std::int64_t mtime_ms; // 修改时间（Unix 毫秒时间戳；取不到为 0）
};

// 列出目录内容（含类型/大小/修改时间），按名称排序；目录不存在返回空
LIBMINI_API std::vector<DirEntry> list_directory_detailed(const std::string& path);

// 路径是否为目录
LIBMINI_API bool is_directory(const std::string& path);

// 最后修改时间（Unix 毫秒时间戳；失败返回 0）
LIBMINI_API std::int64_t file_mtime_ms(const std::string& path);

// ------------------ 临时文件 ------------------

// 系统临时目录（TMP/TEMP 环境变量，回退 "C:\Windows\Temp" 或 "/tmp"）
LIBMINI_API std::string temp_directory_path();

// 在 dir 下生成唯一命名的临时文件路径（文件不一定已创建）；
// prefix 缺省为 "libmini_"；dir 为空则使用系统临时目录
LIBMINI_API std::string unique_temp_path(const std::string& prefix = std::string("libmini_"),
                             const std::string& dir = std::string());

}  // namespace libmini

#endif  // LIBMINI_FILE_UTILS_H
