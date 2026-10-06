#ifndef LIBMINI_FILE_UTILS_H
#define LIBMINI_FILE_UTILS_H

#include <cstdint>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 检查路径是否存在（文件或目录均算存在）
LIBMINI_API bool file_exists(const std::string& path);

// 读取文件内容；打不开/出错返回空串
LIBMINI_API std::string read_file(const std::string& path);

// 写入文件内容（覆盖已有文件）
LIBMINI_API bool write_file(const std::string& path, const std::string& content);

// 向文件末尾追加内容（文件不存在则创建）
LIBMINI_API bool append_file(const std::string& path, const std::string& content);

// 原子写文件：先写同目录临时文件并强制落盘（FlushFileBuffers/fsync），
// 再原子替换目标（MoveFileExW/rename，同目录保证同卷）。进程在任意时刻
// 崩溃，目标文件要么是完整旧内容、要么是完整新内容，不会截断或半截。
// 目标所在目录必须已存在；失败时目标保持原样（临时文件会被清理）
LIBMINI_API bool write_file_atomic(const std::string& path,
                                   const std::string& content);

// 获取文件大小；失败返回 0
LIBMINI_API size_t file_size(const std::string& path);

// 列出目录内容（仅文件/子目录名，不含 \".\" 与 \"..\"）；目录不存在返回空
LIBMINI_API std::vector<std::string> list_directory(const std::string& path);

// 删除文件（失败返回 false）
LIBMINI_API bool remove_file(const std::string& path);

// 删除空目录（失败返回 false）
LIBMINI_API bool remove_directory(const std::string& path);

// ------------------ 目录创建 ------------------

// 创建单级目录（父目录必须已存在）
LIBMINI_API bool make_directory(const std::string& path);

// 递归创建目录链（\"a/b/c\" 自动补齐 a、a/b），已存在视为成功
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

// 系统临时目录（TMP/TEMP 环境变量，回退 \"C:\\Windows\\Temp\" 或 \"/tmp\"）
LIBMINI_API std::string temp_directory_path();

// 在 dir 下生成唯一命名的临时文件路径（文件不一定已创建）；
// prefix 缺省为 \"libmini_\"；dir 为空则使用系统临时目录
LIBMINI_API std::string unique_temp_path(const std::string& prefix = std::string("libmini_"),
                               const std::string& dir = std::string());

// ------------------ 流式文件摘要 ------------------

// 64KB 分块读取，内存占用恒定，适用于大文件；文件打不开返回空串

// 文件 SHA-256（64 字符小写十六进制）
LIBMINI_API std::string sha256_file_hex(const std::string& path);

// 文件 MD5（32 字符小写十六进制）
LIBMINI_API std::string md5_file_hex(const std::string& path);

// ------------------ 符号链接 ------------------

// 符号链接的目标（未解引用的原始链接目标）；非符号链接或失败返回空串
LIBMINI_API std::string read_symlink(const std::string& path);

// 创建符号链接 target → link_path（link_path 不存在时创建，已存在符号链接
// 则覆盖；目标已存在且非符号链接则失败）。Windows 要求链接目标是绝对路径
// 或相对于 link_path 父目录的相对路径，且创建目录符号链接需权限/管理员
// 标记；不支持时返回 false
LIBMINI_API bool create_symlink(const std::string& target,
                                const std::string& link_path,
                                bool directory = false);

// 删除符号链接本身（不递归删目标）；非符号链接也可删除（等价 remove_file/
// remove_directory，视类型而定）；失败返回 false
LIBMINI_API bool remove_symlink(const std::string& path);

// ------------------ 文件权限 ------------------

// POSIX 下读取返回权限位（st_mode 中的 S_IRWXU/GR/GX 掩码），Windows 下
// 返回 false。目录也可传入。
LIBMINI_API bool file_permissions(const std::string& path,
                                  std::uint32_t& mode);

// POSIX 下用八进制权限位设置访问权限（umask 仍生效）；Windows 下返回 false。
// mode 示例：0755（rwxr-xr-x）、0600（rw-------）。传入 Directory 也可。
LIBMINI_API bool set_file_permissions(const std::string& path,
                                      std::uint32_t mode);

// ------------------ 目录占用 ------------------

// 目录 tree 的总大小（递归累加所有文件大小，目录自身大小不计入）；
// path 不存在或不是目录返回 0。符号链接目标不解引用（链接本身算 0，
// 与 list_directory_detailed 的大小口径一致）；follow_symlinks 控制是否
// 跳入符号链接指向的目录（默认不跳，避免循环）
LIBMINI_API std::uint64_t directory_size(const std::string& path,
                                         bool follow_symlinks = false);

// ------------------ 作用域临时目录 ------------------

// 创建进程/线程作用域的临时目录：目录名唯一、创建后立即返回路径。
// 创建失败返回空串。dir 为空则用系统临时目录；prefix 缺省 \"libmini_\"
LIBMINI_API std::string unique_temp_directory(const std::string& prefix = std::string("libmini_"),
                                              const std::string& dir = std::string());

}  // namespace libmini

#endif  // LIBMINI_FILE_UTILS_H
