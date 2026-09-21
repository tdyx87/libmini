#include "string_algo.h"

#include <algorithm>
#include <cctype>

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

}  // namespace libmini
