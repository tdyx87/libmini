#include "string_algo.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include "grapheme.h"
#include "utf8_codec.h"

namespace libmini {

namespace {

std::string to_lower_copy_ascii(const std::string& str)
{
    std::string out = str;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

bool starts_with(const std::string& str, const std::string& prefix)
{
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& str, const std::string& suffix)
{
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& str, const std::string& needle)
{
    return str.find(needle) != std::string::npos;
}

bool iequals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    return to_lower_copy_ascii(a) == to_lower_copy_ascii(b);
}

std::string trim_prefix(const std::string& str, const std::string& prefix)
{
    if (starts_with(str, prefix)) {
        return str.substr(prefix.size());
    }
    return str;
}

std::string trim_suffix(const std::string& str, const std::string& suffix)
{
    if (ends_with(str, suffix)) {
        return str.substr(0, str.size() - suffix.size());
    }
    return str;
}

std::string replace_first(const std::string& str, const std::string& from,
                          const std::string& to)
{
    const size_t pos = str.find(from);
    if (pos == std::string::npos) {
        return str;
    }
    std::string result = str;
    result.replace(pos, from.size(), to);
    return result;
}

std::string replace_all(const std::string& str, const std::string& from,
                        const std::string& to)
{
    if (from.empty()) {
        return str;
    }
    std::string result;
    result.reserve(str.size());
    size_t pos = 0;
    for (;;) {
        const size_t found = str.find(from, pos);
        if (found == std::string::npos) {
            result.append(str, pos, std::string::npos);
            break;
        }
        result.append(str, pos, found - pos);
        result.append(to);
        pos = found + from.size();
    }
    return result;
}

std::vector<std::string> split_whitespace(const std::string& str)
{
    std::vector<std::string> tokens;
    const char* ws = " \t\n\r\f\v";
    size_t pos = str.find_first_not_of(ws);
    while (pos != std::string::npos) {
        const size_t end = str.find_first_of(ws, pos);
        if (end == std::string::npos) {
            tokens.push_back(str.substr(pos));
            break;
        }
        tokens.push_back(str.substr(pos, end - pos));
        pos = str.find_first_not_of(ws, end);
    }
    return tokens;
}

std::vector<std::string> split_string(const std::string& str,
                                      const std::string& delimiter,
                                      bool skip_empty)
{
    std::vector<std::string> tokens;
    if (delimiter.empty()) {
        tokens.push_back(str);
        return tokens;
    }

    size_t pos = 0;
    for (;;) {
        const size_t found = str.find(delimiter, pos);
        const std::string token =
            (found == std::string::npos) ? str.substr(pos)
                                         : str.substr(pos, found - pos);
        if (!skip_empty || !token.empty()) {
            tokens.push_back(token);
        }
        if (found == std::string::npos) {
            break;
        }
        pos = found + delimiter.size();
    }
    return tokens;
}

std::string join(const std::vector<std::string>& parts,
                 const std::string& delimiter)
{
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            result += delimiter;
        }
        result += parts[i];
    }
    return result;
}

// ------------------------------ UTF-8 感知操作 ------------------------------
//
// 严格解码与容错步长放在 utf8_codec.h，字形簇分割放在 grapheme.cpp——
// 三者对非法输入的处理必须一致，所以只允许有一份实现。

bool utf8_is_valid(const std::string& str)
{
    std::size_t pos = 0;
    while (pos < str.size()) {
        std::uint32_t code_point = 0;
        const std::size_t length = utf8_detail::decode(str, pos, code_point);
        if (length == 0) {
            return false;
        }
        pos += length;
    }
    return true;
}

std::size_t utf8_length(const std::string& str)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < str.size()) {
        pos += utf8_detail::step(str, pos);
        ++count;
    }
    return count;
}

std::size_t utf8_byte_offset(const std::string& str, std::size_t index)
{
    std::size_t pos = 0;
    std::size_t chars = 0;
    while (pos < str.size() && chars < index) {
        pos += utf8_detail::step(str, pos);
        ++chars;
    }
    if (chars < index) {
        return std::string::npos;  // 字符数不足
    }
    return pos;
}

std::string utf8_substr(const std::string& str, std::size_t index,
                        std::size_t count)
{
    const std::size_t begin = utf8_byte_offset(str, index);
    if (begin == std::string::npos) {
        return std::string();
    }
    if (count == std::string::npos) {
        return str.substr(begin);
    }
    std::size_t pos = begin;
    std::size_t taken = 0;
    while (pos < str.size() && taken < count) {
        pos += utf8_detail::step(str, pos);
        ++taken;
    }
    return str.substr(begin, pos - begin);
}

std::string utf8_slice(const std::string& str, std::size_t begin, std::size_t end)
{
    if (end == std::string::npos) {
        const std::size_t start = utf8_byte_offset(str, begin);
        return start == std::string::npos ? std::string() : str.substr(start);
    }
    if (end <= begin) {
        return std::string();
    }
    return utf8_substr(str, begin, end - begin);
}

std::string utf8_truncate(const std::string& str, std::size_t max_chars)
{
    return utf8_substr(str, 0, max_chars);
}

std::string utf8_truncate_bytes(const std::string& str, std::size_t max_bytes)
{
    if (str.size() <= max_bytes) {
        return str;
    }
    std::size_t pos = 0;
    while (pos < str.size()) {
        const std::size_t step = utf8_detail::step(str, pos);
        if (pos + step > max_bytes) {
            break;  // 再放一个字符就超了：宁可短一点，也不切成半个序列
        }
        pos += step;
    }
    return str.substr(0, pos);
}

std::string utf8_tail(const std::string& str, std::size_t max_chars)
{
    const std::size_t total = utf8_length(str);
    if (total <= max_chars) {
        return str;
    }
    return utf8_substr(str, total - max_chars, std::string::npos);
}

// ------------------------------ 字形簇级操作 ------------------------------

std::size_t utf8_grapheme_length(const std::string& str)
{
    return grapheme_cluster_count(str);
}

std::size_t utf8_grapheme_byte_offset(const std::string& str,
                                      std::size_t index)
{
    return grapheme_byte_offset(str, index);
}

std::string utf8_grapheme_substr(const std::string& str, std::size_t index,
                                 std::size_t count)
{
    const std::size_t begin = grapheme_byte_offset(str, index);
    if (begin == std::string::npos) {
        return std::string();
    }
    if (count == std::string::npos) {
        return str.substr(begin);
    }
    std::size_t cursor = begin;
    std::size_t taken = 0;
    while (cursor < str.size() && taken < count) {
        cursor += grapheme_cluster_step(str, cursor);
        ++taken;
    }
    return str.substr(begin, cursor - begin);
}

std::string utf8_grapheme_slice(const std::string& str, std::size_t begin,
                                std::size_t end)
{
    if (end == std::string::npos) {
        const std::size_t start = grapheme_byte_offset(str, begin);
        return start == std::string::npos ? std::string() : str.substr(start);
    }
    if (end <= begin) {
        return std::string();
    }
    return utf8_grapheme_substr(str, begin, end - begin);
}

std::string utf8_grapheme_truncate(const std::string& str,
                                   std::size_t max_graphemes)
{
    return utf8_grapheme_substr(str, 0, max_graphemes);
}

std::string utf8_grapheme_truncate_bytes(const std::string& str,
                                         std::size_t max_bytes)
{
    if (str.size() <= max_bytes) {
        return str;
    }
    std::size_t cursor = 0;
    while (cursor < str.size()) {
        const std::size_t step = grapheme_cluster_step(str, cursor);
        if (cursor + step > max_bytes) {
            break;  // 再放一个字形簇就超了：宁可短一点，也不拆散它
        }
        cursor += step;
    }
    return str.substr(0, cursor);
}

std::string utf8_grapheme_tail(const std::string& str,
                               std::size_t max_graphemes)
{
    const std::size_t total = grapheme_cluster_count(str);
    if (total <= max_graphemes) {
        return str;
    }
    return utf8_grapheme_substr(str, total - max_graphemes, std::string::npos);
}

}  // namespace libmini
