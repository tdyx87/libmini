#include "glob.h"

#include <algorithm>

#include "file_utils.h"
#include "path_utils.h"

namespace libmini {

namespace {

// Windows 文件系统本身不区分大小写，POSIX 区分。跟随宿主行为而不是
// 给一个全局开关：调用方要「两种都对」时，应该在传进来之前自己统一
// 大小写，那比在这里猜意图更可控。
bool case_insensitive()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

char lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool nc_equal(char a, char b)
{
    return case_insensitive() ? (lower(a) == lower(b)) : (a == b);
}

// 解析 [...] 集合。成功时返回 true 并输出匹配函数所需的范围列表。
//
// 刻意容忍不规范的写法：文件真叫 "a[bc.txt" 时整体判否会让 glob 找不到
// 一个真实存在的文件，比把它当字面量糟糕得多。所以找不到合法的 ']' 就
// 返回 false，由调用方退化成「'[' 是字面字符」。
struct BracketRange
{
    char lo;
    char hi;
};

bool parse_bracket(const std::string& pattern, std::size_t pos,
                   std::vector<BracketRange>& ranges, bool& negated,
                   std::size_t& next)
{
    // pattern[pos] == '['
    std::size_t i = pos + 1;
    negated = false;
    if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
        negated = true;
        ++i;
    }
    ranges.clear();
    bool closed = false;
    bool first = true;
    while (i < pattern.size()) {
        if (pattern[i] == ']' && !first) {
            closed = true;
            ++i;
            break;
        }
        first = false;
        // 转义的 ']' 是字面量
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            ++i;
        }
        if (i >= pattern.size()) {
            break;
        }
        const char lo = pattern[i];
        // a-z 区间：上界在 i+2，中间那个 '-' 只是连接符
        if (i + 2 < pattern.size() && pattern[i + 1] == '-' &&
            pattern[i + 2] != ']') {
            char hi = pattern[i + 2];
            if (hi == '\\' && i + 3 < pattern.size()) {
                hi = pattern[i + 3];
                i += 1;  // 多消耗一个转义字符
            }
            BracketRange r;
            r.lo = lo;
            r.hi = hi;
            if (case_insensitive() && r.lo != r.hi) {
                // 大小写不敏感时两端都折叠到小写，
                // 否则 [a-z] 匹配不到 'B'
                r.lo = lower(lo);
                r.hi = lower(hi);
            }
            ranges.push_back(r);
            i += 2;  // 跳过 '-' 与上界
            continue;
        }
        BracketRange r;
        r.lo = lo;
        r.hi = lo;
        ranges.push_back(r);
        ++i;
    }
    if (!closed || ranges.empty()) {
        return false;
    }
    next = i;
    return true;
}

// 迭代式通配匹配（经典的贪心回溯写法，不递归，避免深 pattern 爆栈）。
bool match_here(const std::string& pattern, std::size_t p,
                const std::string& name, std::size_t n)
{
    std::size_t star_p = std::string::npos;
    std::size_t star_n = 0;

    while (n < name.size()) {
        // 边界检查必须先于取值：p == pattern.size() 时读 pattern[p] 是
        // 越界。更糟的是越界读到的字节若恰是 '*'，回溯逻辑会把 p 继续
        // 往后推，从而在整块堆内存里向前扫 —— 表现为「匹配一个长 pattern
        // 把进程挂死」，而不是干脆的崩溃或失败。
        if (p >= pattern.size()) {
            // pattern 用尽，名字还有剩余：只能靠最后一个 '*' 继续吃
            if (star_p != std::string::npos) {
                p = star_p + 1;
                ++star_n;
                n = star_n;
                continue;
            }
            return false;
        }
        const char pc = pattern[p];
        if (pc == '*') {
            star_p = p;
            star_n = n;
            ++p;
            continue;
        }
        bool matched = false;
        if (p < pattern.size()) {
            if (pc == '?') {
                // 必须在这里推进 p 与 n：只置 matched 而不推进，末尾的
                // `if (matched) continue;` 会回到同一状态，死循环。
                matched = true;
                ++n;
                ++p;
                continue;
            } else if (pc == '[') {
                std::vector<BracketRange> ranges;
                bool negated = false;
                std::size_t next = 0;
                if (parse_bracket(pattern, p, ranges, negated, next)) {
                    const char nc = case_insensitive() ? lower(name[n]) : name[n];
                    bool hit = false;
                    for (std::size_t i = 0; i < ranges.size(); ++i) {
                        if (nc >= ranges[i].lo && nc <= ranges[i].hi) {
                            hit = true;
                            break;
                        }
                    }
                    matched = (hit != negated);
                    if (matched) {
                        ++n;
                        p = next;
                        continue;
                    }
                } else {
                    // 不合法的集合：'[' 当字面量
                    matched = (nc_equal(name[n], '['));
                    if (matched) {
                        ++n;
                        ++p;
                        continue;
                    }
                }
            } else if (pc == '\\' && p + 1 < pattern.size()) {
                matched = nc_equal(name[n], pattern[p + 1]);
                if (matched) {
                    ++n;
                    p += 2;
                    continue;
                }
            } else {
                matched = nc_equal(name[n], pc);
                if (matched) {
                    ++n;
                    ++p;
                    continue;
                }
            }
        }
        if (matched) {
            continue;
        }
        // 回溯：把上一个 '*' 多吃一个字符再试
        if (star_p != std::string::npos) {
            p = star_p + 1;
            ++star_n;
            n = star_n;
            continue;
        }
        return false;
    }
    // 名字用尽后，pattern 剩下的必须全是 '*'
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

}  // namespace

bool glob_match(const std::string& pattern, const std::string& name)
{
    return match_here(pattern, 0, name, 0);
}

namespace {

// 递归遍历。pattern 只作用于文件名（不含目录部分）。
// recurse=false 时只看 root 自身这一层（glob 的语义）；
// recurse=true 时递归整棵树（glob_recursive 的语义）。
void walk(const std::string& root, const std::string& pattern,
          const GlobOptions& options, int depth, bool recurse,
          std::vector<std::string>& out)
{
    if (options.max_results > 0 && out.size() >= options.max_results) {
        return;
    }
    if (options.max_depth > 0 && depth > options.max_depth) {
        return;
    }

    const std::vector<DirEntry> entries = list_directory_detailed(root);
    // list_directory_detailed 已按名称排序；这里再排一次是为了不依赖
    // 那个实现细节——「同一目录两次列出顺序不同」会让调用方无法 diff。
    std::vector<DirEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const DirEntry& a, const DirEntry& b) {
                  return a.name < b.name;
              });

    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (options.max_results > 0 && out.size() >= options.max_results) {
            return;
        }
        const DirEntry& entry = sorted[i];
        const bool is_dir = (entry.kind == EntryKind::Directory);
        if (!options.include_hidden && !entry.name.empty() &&
            entry.name[0] == '.') {
            // 隐藏项跳过，但目录仍要递归进去：.git 里有需要找的文件，
            // 只是 .git 本身不出现在结果里。
            if (!is_dir) {
                continue;
            }
            if (recurse) {
                walk(entry.path, pattern, options, depth + 1, true, out);
            }
            continue;
        }

        if (is_dir) {
            if (options.include_directories &&
                glob_match(pattern, entry.name)) {
                out.push_back(entry.path);
            }
            if (recurse && (options.follow_symlinks ||
                            entry.kind != EntryKind::Symlink)) {
                walk(entry.path, pattern, options, depth + 1, true, out);
            }
            continue;
        }

        if (glob_match(pattern, entry.name)) {
            out.push_back(entry.path);
        }
    }
}

}  // namespace

std::vector<std::string> glob(const std::string& root,
                              const std::string& pattern,
                              const GlobOptions& options)
{
    std::vector<std::string> out;

    // pattern 允许带目录部分（"sub/*.cpp"）。目录部分必须是**字面量**：
    // 多个目录段（"a/*/*.cpp"）交给 glob_recursive，这里明确不支持而不是
    // 半吊子地处理——调用方拿到不符合预期的结果更难排查。
    // 注意判的是 dir_part 而不是 file_part：file_part 才是带通配的那个
    // （"sub/*.cpp" 里 dir_part="sub"、file_part="*.cpp"），
    // 判反了会让所有带目录的 pattern 都落回单层遍历、静默返回空。
    const std::string normalized = path_to_generic(pattern);
    const std::size_t sep = normalized.find_last_of('/');
    if (sep != std::string::npos) {
        const std::string dir_part = normalized.substr(0, sep);
        const std::string file_part = normalized.substr(sep + 1);
        if (dir_part.find_first_of("*?[") == std::string::npos &&
            !file_part.empty()) {
            const std::string base =
                path_join(root, path_to_generic(dir_part));
            const std::vector<DirEntry> entries = list_directory_detailed(base);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (options.max_results > 0 &&
                    out.size() >= options.max_results) {
                    break;
                }
                if (entries[i].kind == EntryKind::Directory &&
                    !options.include_directories) {
                    continue;
                }
                if (!options.include_hidden && !entries[i].name.empty() &&
                    entries[i].name[0] == '.') {
                    continue;
                }
                if (glob_match(file_part, entries[i].name)) {
                    out.push_back(entries[i].path);
                }
            }
            std::sort(out.begin(), out.end());
            return out;
        }
    }

    walk(root, pattern, options, 0, /*recurse=*/false, out);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> glob_recursive(const std::string& root,
                                        const std::string& pattern,
                                        const GlobOptions& options)
{
    std::vector<std::string> out;
    // 允许用户写成 "**/*.cpp"（多数语言的习惯），把前缀剥掉
    std::string effective = pattern;
    while (effective.size() >= 3 && effective.compare(0, 3, "**/") == 0) {
        effective = effective.substr(3);
    }
    if (effective.empty()) {
        effective = "*";
    }
    walk(root, effective, options, 0, /*recurse=*/true, out);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> glob_files_by_extension(
    const std::string& root, const std::string& extension_with_dot)
{
    GlobOptions options;
    options.include_directories = false;
    return glob_recursive(root, "*" + extension_with_dot, options);
}

}  // namespace libmini
