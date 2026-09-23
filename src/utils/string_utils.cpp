#include "string_utils.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace libmini {

std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    return str.substr(first, last - first + 1);
}

std::string replace(const std::string& str, const std::string& from, const std::string& to) {
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

std::string to_upper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
}

std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string format_bytes(std::int64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    const bool neg = bytes < 0;
    unsigned long long v = neg ? 0ULL - static_cast<unsigned long long>(bytes)
                               : static_cast<unsigned long long>(bytes);
    if (v < 1024) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%llu B", neg ? "-" : "", v);
        return buf;
    }
    double d = static_cast<double>(v);
    int unit = 0;
    while (d >= 1024.0 && unit < 6) {
        d /= 1024.0;
        ++unit;
    }
    if (unit >= 6) {  // ≥1 EB：超出双精度可靠范围，按字节原样输出
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%llu B", neg ? "-" : "", v);
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", d);
    std::string num = buf;
    // 去尾零（"1.50" → "1.5"；"1.00" → "1"），保留至少一位整数
    std::size_t end = num.size() - 1;
    while (end > 0 && num[end] == '0') --end;
    if (num[end] == '.') --end;
    num.erase(end + 1);
    return std::string(neg ? "-" : "") + num + " " + kUnits[unit];
}

std::string format_duration_ms(std::int64_t ms) {
    const bool neg = ms < 0;
    std::int64_t v = neg ? -ms : ms;
    char buf[64];
    std::string prefix = neg ? "-" : "";
    if (v < 1000) {
        std::snprintf(buf, sizeof(buf), "%s%lld ms", prefix.c_str(),
                      static_cast<long long>(v));
        return buf;
    }
    if (v < 60000) {
        const double s = static_cast<double>(v) / 1000.0;
        std::snprintf(buf, sizeof(buf), "%.2f", s);
        std::string num = buf;
        std::size_t end = num.size() - 1;
        while (end > 0 && num[end] == '0') --end;
        if (num[end] == '.') --end;
        num.erase(end + 1);
        return prefix + num + " s";
    }
    if (v < 3600000) {
        const std::int64_t m = v / 60000;
        const double s = static_cast<double>(v % 60000) / 1000.0;
        std::snprintf(buf, sizeof(buf), "%s%lldm %04.1f s", prefix.c_str(),
                      static_cast<long long>(m), s);
        return buf;
    }
    const std::int64_t h = v / 3600000;
    const std::int64_t m = (v % 3600000) / 60000;
    const std::int64_t s = (v % 60000) / 1000;
    std::snprintf(buf, sizeof(buf), "%s%lldh %lldm %llds", prefix.c_str(),
                  static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s));
    return buf;
}

}