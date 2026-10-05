#ifndef LIBMINI_GLOB_H
#define LIBMINI_GLOB_H

#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 通配匹配与文件查找，对应 shell 的 `find . -name '*.cpp'` 与
// Python 的 `glob` / `fnmatch`。
//
// 拆成两层是有意的：
//   * glob_match —— 纯字符串算法，不碰文件系统。可以单测、可以内嵌在
//     其他筛选逻辑里，也不会因为遍历顺序不同而给出不同结果。
//   * glob / glob_recursive —— 负责遍历。文件系统相关的复杂度都在这一层，
//     匹配语义集中在一处，不会两处实现漂移。

// ===================== 通配匹配（fnmatch 风格） =====================

// 匹配单个路径名（不含分隔符）是否满足 pattern。
//
// 支持的语法：
//   *      任意长度的任意字符（含空）
//   ?      任意单个字符
//   [abc]  字符集合，支持 a-z 区间与 [!abc] / [^abc] 取反
//   \x     转义下一个字符（Windows 风格；POSIX 下 * 和 ? 不能被转义）
//
// 不支持 **（跨分隔符递归通配）——那是 glob_recursive 的职责，
// 混在匹配器里会让「匹配一个名字」变成「匹配一棵树」，语义会变浑。
//
// 大小写：Windows 上不敏感（文件系统本身不敏感），POSIX 上敏感。
// 跨平台应用若要「两种行为都对」，请自行传入已统一大小写的两侧字符串。
//
// 特殊字符：pattern 以 '[' 开头但没有合法的 ']' 时，按字面量处理
// （例如文件真叫 "[abc].txt" 时不该整个匹配失败）。
LIBMINI_API bool glob_match(const std::string& pattern, const std::string& name);

// glob_recursive 用的选项
struct LIBMINI_API GlobOptions {
    // 结果是否包含目录。false 时只返回文件。
    bool include_directories = false;
    // 结果是否包含隐藏项（以 '.' 开头）。shell 里 * 不匹配前导点，
    // 这里沿用同样的默认：false。需要时显式打开。
    bool include_hidden = false;
    // 是否跟随符号链接目录。默认 false——跟随会让遍历在环形软链上
    // 无限递归（真会发生的，不只是理论风险）。
    bool follow_symlinks = false;
    // 递归深度上限，0 = **不限**。以 root 为 0 计：1 = root 加下一层，
    // 2 = 再下一层。不写成「0 = 只看 root」是因为那样调用方无法表达
    // 「全部深度」——而那恰恰是最常见的诉求。
    int max_depth = 0;
    // 单次遍历最多返回多少条。0 = 不限。上限存在是因为调用方多半要
    // 把结果全塞进内存/界面，一个失控的 "**" 能把进程撑爆。
    std::size_t max_results = 0;
};

// ===================== 遍历与查找 =====================

// 在 root 下查找匹配 pattern 的项。pattern 可以是：
//   "*.cpp"            只匹配当前层（等价于 shell 的 find -maxdepth 1 -name）
//   "sub/*.cpp"        指定一层子目录
//   "*.cpp" + GlobOptions::max_depth 无法表达多层 → 用 glob_recursive
//
// 结果按路径排序（保证可复现：文件系统的遍历顺序不保证稳定，
// 而「同一目录两次列出结果顺序不同」会让调用方的输出无法 diff）。
// root 不存在或不可读返回空。
LIBMINI_API std::vector<std::string> glob(const std::string& root,
                                          const std::string& pattern,
                                          const GlobOptions& options = GlobOptions());

// 递归查找。pattern 只作用于**文件名**（不含目录部分），
// 即 "**/*.cpp" 里的 "**/" 部分由本函数隐含。
//
// 结果同样是排序过的完整路径。
LIBMINI_API std::vector<std::string> glob_recursive(const std::string& root,
                                                    const std::string& pattern,
                                                    const GlobOptions& options = GlobOptions());

// 便捷版：递归查找并按扩展名过滤（pattern 形如 "*.cpp"），
// 只要文件不要目录。日常最常用的形态，少写一堆选项。
LIBMINI_API std::vector<std::string> glob_files_by_extension(
    const std::string& root, const std::string& extension_with_dot);

}  // namespace libmini

#endif  // LIBMINI_GLOB_H
