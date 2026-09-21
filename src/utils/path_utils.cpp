#include "path_utils.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#define LIBMINI_PATH_PREFER_BACKSLASH 1
#else
#define LIBMINI_PATH_PREFER_BACKSLASH 0
#endif

namespace libmini {

namespace {

bool is_sep(char c)
{
    return c == '/' || c == '\\';
}

// 统一转为 '/' 分隔，便于内部处理
std::string to_slashes(const std::string& p)
{
    std::string r = p;
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

// 结构信息：前缀（盘符 "C:" 或 UNC "//server/share"）与主体组件
struct PathParts
{
    std::string prefix;                  // "C:" 或 "//server/share"，可为空
    std::vector<std::string> components; // 主体路径段（不含空段）
    bool absolute = false;               // 主体以 '/' 开头
    bool trailing_sep = false;           // 原路径以分隔符结尾
};

PathParts split_normalized(const std::string& path)
{
    PathParts parts;
    const std::string s = to_slashes(path);

    std::size_t pos = 0;

    // Windows 盘符前缀："C:" 后跟分隔符或字符串结束（排除 "a/b" 中间冒号）
#ifdef _WIN32
    if (s.size() >= 2 && s[1] == ':' && std::isalpha(static_cast<unsigned char>(s[0]))) {
        parts.prefix = s.substr(0, 2);
        pos = 2;
        if (pos < s.size() && s[pos] == '/') {
            parts.absolute = true;
            ++pos;
        }
    } else
#endif
    // UNC/URL 风格前导 "//"：保留到第三个 '/' 或结尾
    if (s.size() >= 2 && s[0] == '/' && s[1] == '/') {
        const std::size_t end = s.find('/', 2);
        if (end == std::string::npos) {
            parts.prefix = s;  // 整串都是 //server
            pos = s.size();
        } else {
            parts.prefix = s.substr(0, end);  // "//server/share"
            pos = end;
            parts.absolute = true;
            ++pos;
        }
    } else if (!s.empty() && s[0] == '/') {
        parts.absolute = true;
        pos = 1;
    }

    // 切分主体组件，解析 "." 与 ".."
    bool at_root = parts.absolute || !parts.prefix.empty();
    while (pos < s.size()) {
        const std::size_t next = s.find('/', pos);
        const std::string comp =
            s.substr(pos, next == std::string::npos ? std::string::npos
                                                    : next - pos);
        if (next == std::string::npos) {
            pos = s.size();
        } else {
            pos = next + 1;
        }
        if (comp.empty() || comp == ".") {
            continue;
        }
        if (comp == "..") {
            if (!parts.components.empty() && parts.components.back() != "..") {
                parts.components.pop_back();  // 回退一级
            } else if (!at_root) {
                parts.components.push_back("..");  // 相对路径无法回退则保留
            }
            // 根/盘符处 ".." 直接丢弃（不越过根）
            continue;
        }
        parts.components.push_back(comp);
    }

    // 结尾分隔符：仅当路径不是纯根（"/"、"C:/"）时保留语义
    parts.trailing_sep =
        !s.empty() && s[s.size() - 1] == '/' && parts.absolute &&
        (parts.prefix.size() + 1 < s.size());
    return parts;
}

// 由 PathParts 重组为 '/' 分隔的规范化路径
std::string join_normalized(const PathParts& parts)
{
    std::string out = parts.prefix;
    // 前缀与首个组件之间的分隔符：
    //   盘符 "C:" + 组件 → "C:/x"；UNC "//server" + 组件 → "//server/x"；
    //   前缀为空 + 绝对 → "/x"；纯 UNC（"//server/share"，无组件）不加尾分隔
    const bool need_sep_after_prefix =
        !parts.components.empty() &&
        (parts.absolute || !parts.prefix.empty());
    if (need_sep_after_prefix &&
        (out.empty() || out[out.size() - 1] != '/')) {
        out += "/";
    }
    for (std::size_t i = 0; i < parts.components.size(); ++i) {
        if (i > 0) {
            out += "/";
        }
        out += parts.components[i];
    }
    if (parts.components.empty() && parts.absolute && parts.prefix.empty()) {
        out = "/";
    }
    if (parts.components.empty() && !parts.prefix.empty() && !parts.absolute) {
        // 纯盘符/UNC 无组件：保持原样（"C:"、"//server/share"）
        out = parts.prefix;
    } else if (parts.trailing_sep && !out.empty() && out[out.size() - 1] != '/') {
        out += "/";
    }
    return out;
}

bool ieq(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

// 是否带有"根名"（root name）：POSIX 的 "/"，或 Windows 的盘符/UNC。
// Windows 上单前导分隔符（"\\x"、"/x"）是当前盘符相对路径，不算根名
bool has_root_name(const std::string& path)
{
    const std::string s = to_slashes(path);
    if (s.empty()) {
        return false;
    }
#ifdef _WIN32
    if (s[0] == '/' && !(s.size() >= 2 && s[1] == '/')) {
        return false;  // "\\x" = 当前盘符根，无根名
    }
    if (s.size() >= 2 && s[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(s[0]))) {
        return true;  // 盘符
    }
    if (s.size() >= 2 && s[0] == '/' && s[1] == '/') {
        return true;  // UNC
    }
    return false;
#else
    return s[0] == '/';
#endif
}

std::string path_join(const std::string& path1, const std::string& path2)
{
    if (path1.empty()) return path2;
    if (path2.empty()) return path1;
    // 第二段带根名（POSIX 绝对 / Windows 盘符或 UNC）时直接替换；
    // Windows 的 "\\file" 是盘符相对路径，仍然拼接
    if (path_is_absolute(path2) && has_root_name(path2)) {
        return path2;
    }
    if (is_sep(path1[path1.size() - 1])) {
        return path1 + path2;
    }
    if (is_sep(path2[0])) {
        return path1 + path2;
    }
#ifdef _WIN32
    return path1 + "\\" + path2;
#else
    return path1 + "/" + path2;
#endif
}

std::string dirname(const std::string& path)
{
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return path.substr(0, 1);  // "/x" → "/"
    return path.substr(0, pos);
}

std::string basename(const std::string& path)
{
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string extension(const std::string& path)
{
    const std::string base = basename(path);
    const std::size_t pos = base.find_last_of('.');
    // 隐藏文件 ".gitignore" 不算扩展名
    if (pos == std::string::npos || pos == 0) return "";
    return base.substr(pos);
}

std::string stem(const std::string& path)
{
    const std::string base = basename(path);
    const std::size_t pos = base.find_last_of('.');
    if (pos == std::string::npos || pos == 0) return base;
    return base.substr(0, pos);
}

std::string replace_extension(const std::string& path,
                              const std::string& new_ext)
{
    const std::string dir = dirname(path);
    const std::string base = basename(path);
    std::size_t pos = base.find_last_of('.');
    if (pos == 0) {
        pos = std::string::npos;  // 隐藏文件：".gitignore" 整体是文件名
    }
    std::string stem_part =
        pos == std::string::npos ? base : base.substr(0, pos);
    std::string ext_part;
    if (!new_ext.empty()) {
        ext_part = new_ext[0] == '.' ? new_ext : "." + new_ext;
    }
    if (dir == ".") {
        return stem_part + ext_part;
    }
    // 用平台分隔符接回目录部分
    return path_join(dir, stem_part + ext_part);
}

std::string normalize_path(const std::string& path)
{
    if (path.empty()) {
        return path;
    }
    const PathParts parts = split_normalized(path);
    return join_normalized(parts);
}

bool path_is_absolute(const std::string& path)
{
    const std::string s = to_slashes(path);
    if (s.empty()) {
        return false;
    }
    if (s[0] == '/') {
        return true;
    }
#ifdef _WIN32
    if (s.size() >= 2 && s[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(s[0]))) {
        return true;
    }
#endif
    return false;
}

std::string path_absolute(const std::string& path, const std::string& base)
{
    if (path_is_absolute(path)) {
        return normalize_path(path);
    }
    std::string b = base.empty() ? std::string(".") : base;
    // base 末尾多余分隔符交给规范化处理
    return normalize_path(path_join(b, path));
}

std::string parent_path(const std::string& path)
{
    // 去掉结尾分隔符再找父级（"a/b/" 与 "C:/" 的父级语义）
    std::string p = path;
    while (!p.empty() && is_sep(p[p.size() - 1])) {
        p.erase(p.size() - 1);
        // 全是分隔符的路径："/" 的父级为空
        if (p.empty()) {
            return std::string();
        }
    }
#ifdef _WIN32
    // 纯盘符（"C:"）没有父级
    if (p.size() == 2 && p[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(p[0]))) {
        return std::string();
    }
#endif
    const std::size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) {
        return std::string();  // 无父级："file.txt"、"C:" → ""
    }
    if (pos == 0) {
        return p.substr(0, 1);  // "/x" → "/"
    }
#ifdef _WIN32
    if (pos == 2 && p[1] == ':') {
        return p.substr(0, 3);  // "C:/x" → "C:/"
    }
#endif
    return p.substr(0, pos);
}

std::string path_to_generic(const std::string& path)
{
    return to_slashes(path);
}

std::string path_to_native(const std::string& path)
{
#if LIBMINI_PATH_PREFER_BACKSLASH
    std::string r = path;
    std::replace(r.begin(), r.end(), '/', '\\');
    return r;
#else
    return to_slashes(path);
#endif
}

std::string path_combine(const std::vector<std::string>& components)
{
    std::string out;
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (components[i].empty()) {
            continue;
        }
        if (out.empty()) {
            out = components[i];
        } else if (is_sep(out[out.size() - 1])) {
            out += components[i];
        } else if (is_sep(components[i][0])) {
            out += components[i];
        } else {
#if LIBMINI_PATH_PREFER_BACKSLASH
            out += "\\";
#else
            out += "/";
#endif
            out += components[i];
        }
    }
    return out;
}

bool path_equivalent(const std::string& a, const std::string& b)
{
    std::string na = normalize_path(a);
    std::string nb = normalize_path(b);
#ifdef _WIN32
    // Windows 路径大小写不敏感
    if (na.size() != nb.size()) {
        return false;
    }
    for (std::size_t i = 0; i < na.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(na[i])) !=
            std::tolower(static_cast<unsigned char>(nb[i]))) {
            return false;
        }
    }
    return true;
#else
    return na == nb;
#endif
}

}  // namespace libmini
