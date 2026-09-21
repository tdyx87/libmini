#ifndef LIBMINI_PATH_UTILS_H
#define LIBMINI_PATH_UTILS_H

#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 路径连接
LIBMINI_API std::string path_join(const std::string& path1, const std::string& path2);

// 依次连接多段路径：path_join("a", "b", "c") → "a/b/c"（Windows 上为 a\b）
template <typename... Rest>
std::string path_join(const std::string& first, const std::string& second,
                      Rest... rest)
{
    return path_join(path_join(first, second), rest...);
}

// 获取目录名："a/b/c.txt" → "a/b"；无分隔符返回 "."
LIBMINI_API std::string dirname(const std::string& path);

// 获取文件名："a/b/c.txt" → "c.txt"
LIBMINI_API std::string basename(const std::string& path);

// 获取扩展名（含点）："a/b/c.tar.gz" → ".gz"；无扩展名返回 ""
LIBMINI_API std::string extension(const std::string& path);

// 替换扩展名："report.txt" + ".md" → "report.md"；
// 原无扩展名则追加；new_ext 为空则移除扩展名
LIBMINI_API std::string replace_extension(const std::string& path,
                              const std::string& new_ext);

// 不含扩展名的文件名："a/b/archive.tar" → "archive"
LIBMINI_API std::string stem(const std::string& path);

// 规范化路径：统一为 '/' 分隔、折叠连续分隔符、解析 "." 与 ".."。
// ".." 不越过根目录（"/a/../../b" → "/b"）与盘符（Windows 保留 "C:/"）；
// 前导 "//" 视为 UNC/协议语义原样保留
LIBMINI_API std::string normalize_path(const std::string& path);

// 是否为绝对路径：'/' 开头，或 Windows 盘符（"C:/..."）与 UNC（"//..."）路径
LIBMINI_API bool path_is_absolute(const std::string& path);

// 相对路径基于 base 目录转为绝对路径（仅字符串操作，不访问文件系统）；
// base 为空则使用 "."；path 已是绝对路径时规范化后原样返回
LIBMINI_API std::string path_absolute(const std::string& path,
                          const std::string& base = std::string());

// 父目录路径："a/b/c.txt" → "a/b"；根目录/无父级的路径返回 ""
// （与 dirname 的区别：dirname 对无分隔符路径返回 "."，本函数返回 ""，
//  便于用 empty() 判断"没有父目录"）
LIBMINI_API std::string parent_path(const std::string& path);

// 以 '/' 统一分隔符（Windows 两种分隔符都接受）
LIBMINI_API std::string path_to_generic(const std::string& path);

// 以平台首选分隔符统一（Windows '\'，其他 '/'）
LIBMINI_API std::string path_to_native(const std::string& path);

// 用系统分隔符把多个组件串成路径（空元素跳过）
LIBMINI_API std::string path_combine(const std::vector<std::string>& components);

// 路径比较：分隔符与大小写不敏感（Windows 语义）；非 Windows 下大小写敏感
LIBMINI_API bool path_equivalent(const std::string& a, const std::string& b);

}  // namespace libmini

#endif  // LIBMINI_PATH_UTILS_H
