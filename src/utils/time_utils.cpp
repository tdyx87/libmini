#include "time_utils.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace libmini {

long long current_timestamp_ms()
{
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

std::string current_time_string()
{
    return format_time(std::chrono::system_clock::now());
}

std::string format_time(std::chrono::system_clock::time_point tp,
                        const std::string& format)
{
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time_t);
#else
    tm = *std::localtime(&time_t);
#endif
    std::stringstream ss;
    ss << std::put_time(&tm, format.c_str());
    return ss.str();
}

// ---------------- 日历运算（Howard Hinnant civil-from-days 算法）----------------

namespace {

// days_from_civil 的核心：公历日期 → 天数（1970-01-01 = 0）
std::int64_t days_from_civil_impl(std::int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;    // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// civil_from_days 的核心：天数 → 公历日期
void civil_from_days_impl(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);           // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const std::int64_t yr = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                // [0, 11]
    d = doy - (153 * mp + 2) / 5 + 1;                                       // [1, 31]
    m = mp + (mp < 10 ? 3 : -9);                                            // [1, 12]
    y = yr + (m <= 2);
}

}  // namespace

bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month)
{
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kDays[month - 1];
}

std::int64_t days_from_civil(int year, int month, int day)
{
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
        return -1;  // 非法日期
    }
    return days_from_civil_impl(year, static_cast<unsigned>(month),
                                static_cast<unsigned>(day));
}

bool civil_from_days(std::int64_t days, int& year, int& month, int& day)
{
    std::int64_t y;
    unsigned m, d;
    civil_from_days_impl(days, y, m, d);
    year = static_cast<int>(y);
    month = static_cast<int>(m);
    day = static_cast<int>(d);
    return true;
}

int weekday_of(std::int64_t days)
{
    // 1970-01-01 是周四（4）。负数天数用「先加偏移再取模」保证非负
    int wd = static_cast<int>((days % 7 + 7 + 4) % 7);
    return wd;  // 0=周日 ... 6=周六
}

std::int64_t next_weekday(std::int64_t days, int weekday)
{
    if (weekday < 0 || weekday > 6) {
        return -1;
    }
    const int cur = weekday_of(days);
    int delta = weekday - cur;
    if (delta <= 0) {
        delta += 7;
    }
    return days + delta;
}

std::int64_t month_start(std::int64_t days)
{
    int y, m, d;
    civil_from_days(days, y, m, d);
    return days - (d - 1);
}

std::int64_t month_end(std::int64_t days)
{
    int y, m, d;
    civil_from_days(days, y, m, d);
    return days + (days_in_month(y, m) - d);
}

std::int64_t add_months(std::int64_t days, int months)
{
    int y, m, d;
    civil_from_days(days, y, m, d);
    // 月序平移（支持跨年与负数）
    std::int64_t total = static_cast<std::int64_t>(y) * 12 + (m - 1) + months;
    const std::int64_t ny = total >= 0 ? total / 12 : (total - 11) / 12;
    const int nm = static_cast<int>(total - ny * 12) + 1;
    // 日号钳制到目标月月末
    const int nd = std::min(d, days_in_month(static_cast<int>(ny), nm));
    return days_from_civil_impl(ny, static_cast<unsigned>(nm), static_cast<unsigned>(nd));
}

std::string date_to_string(std::int64_t days)
{
    int y, m, d;
    civil_from_days(days, y, m, d);
    // 缓冲区取 32：16 的话 GCC -Wformat-truncation 在极端年份（INT_MIN）
    // 下会报截断告警（strict job -Werror 实测）
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return (n > 0 && static_cast<std::size_t>(n) < sizeof(buf))
               ? std::string(buf)
               : std::string();
}

std::int64_t date_from_string(const std::string& s)
{
    int y = 0, m = 0, d = 0;
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return -1;
    }
    // 手工解析（格式已校验为 yyyy-mm-dd 定长）；sscanf 有 MSVC C4996 弃用告警
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
    }
    y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    m = (s[5] - '0') * 10 + (s[6] - '0');
    d = (s[8] - '0') * 10 + (s[9] - '0');
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return -1;
    }
    return days_from_civil(y, m, d);  // 非法日期在内部校验返回 -1
}

// ---------------- ISO-8601 / RFC-3339 ----------------

namespace {

// 从 pos 起读 count 个 ASCII 数字；不足/非数字返回 false 且不动 pos
bool read_int(const std::string& s, std::size_t& pos, int count, long& out)
{
    if (pos + static_cast<std::size_t>(count) > s.size()) {
        return false;
    }
    long v = 0;
    for (int i = 0; i < count; ++i) {
        const char c = s[pos + static_cast<std::size_t>(i)];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    pos += static_cast<std::size_t>(count);
    out = v;
    return true;
}

// 本机时区在 tp 时刻相对 UTC 的秒偏移（东为正）。拿不到时区时返回 0。
std::int64_t local_offset_seconds(std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &t) != 0) return 0;
#else
    std::tm* p = std::localtime(&t);
    if (!p) return 0;
    lt = *p;
#endif
    // 用本地时区的 civil 字段当作 UTC 算一遍 epoch 秒，差值就是偏移
    const std::int64_t as_utc =
        days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400 +
        lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    return static_cast<std::int64_t>(t) - as_utc;
}

}  // namespace

std::string format_iso8601(std::chrono::system_clock::time_point tp, bool utc,
                           bool with_millis)
{
    std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          tp.time_since_epoch())
                          .count();

    // 时区后缀（偏移按 tp 所在时刻的本机时区）
    std::string tz;
    if (utc) {
        tz = "Z";
    } else {
        const std::int64_t off = local_offset_seconds(tp);
        std::int64_t a = off < 0 ? -off : off;
        int hh = static_cast<int>(a / 3600);
        int mm = static_cast<int>((a % 3600) / 60);  // 忽略历史上 LMT 的秒级余数
        if (hh > 99) { hh = 99; mm = 0; }
        tz += (off < 0 ? '-' : '+');
        tz += static_cast<char>('0' + hh / 10);
        tz += static_cast<char>('0' + hh % 10);
        tz += ':';
        tz += static_cast<char>('0' + mm / 10);
        tz += static_cast<char>('0' + mm % 10);
        ms += off * 1000;  // 换算到本地墙上时间
    }

    // 向下取整切分天/毫秒（负时间戳：1970 前）
    std::int64_t days = ms / 86400000;
    std::int64_t rem = ms % 86400000;
    if (rem < 0) {
        rem += 86400000;
        --days;
    }
    int y, mo, d;
    civil_from_days(days, y, mo, d);
    const int hh = static_cast<int>(rem / 3600000);
    rem %= 3600000;
    const int mi = static_cast<int>(rem / 60000);
    rem %= 60000;
    const int se = static_cast<int>(rem / 1000);
    const int msf = static_cast<int>(rem % 1000);

    // 缓冲 64：-Wformat-truncation 下 %04d 最坏（INT_MIN）11 字节，其余至多 20
    char buf[64];
    const int n = with_millis
        ? std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                        y, mo, d, hh, mi, se, msf)
        : std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                        y, mo, d, hh, mi, se);
    if (n <= 0) return std::string();
    return std::string(buf, static_cast<std::size_t>(n)) + tz;
}

bool parse_iso8601(const std::string& text,
                   std::chrono::system_clock::time_point& out)
{
    std::size_t pos = 0;
    long y, mo, d;
    if (!read_int(text, pos, 4, y)) return false;
    if (pos < text.size() && text[pos] == '-') ++pos;  // 扩展格式的 '-'
    if (!read_int(text, pos, 2, mo)) return false;
    if (pos < text.size() && text[pos] == '-') ++pos;
    if (!read_int(text, pos, 2, d)) return false;

    // 日期之后必须有时间（'T'/'t'/' ' 分隔）
    if (pos >= text.size()) return false;
    const char sep = text[pos];
    if (sep != 'T' && sep != 't' && sep != ' ') return false;
    ++pos;

    long hh, mi, ss = 0;
    if (!read_int(text, pos, 2, hh)) return false;
    if (pos < text.size() && text[pos] == ':') ++pos;
    if (!read_int(text, pos, 2, mi)) return false;
    // 秒可省略；扩展格式用 ':' 引导，基本格式（无 ':'）直接跟两位数字
    if (pos < text.size() && text[pos] == ':') {
        ++pos;
        if (!read_int(text, pos, 2, ss)) return false;
    } else if (pos + 2 <= text.size() && text[pos] >= '0' && text[pos] <= '9' &&
               text[pos + 1] >= '0' && text[pos + 1] <= '9') {
        if (!read_int(text, pos, 2, ss)) return false;
    }

    // 可选小数秒：'.' 或 ',' 引导；超出毫秒的部分直接截断
    long frac_ms = 0;
    if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
        ++pos;
        const std::size_t start = pos;
        long v = 0;
        int digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 3) {
                v = v * 10 + (text[pos] - '0');
                ++digits;
            }
            ++pos;
        }
        if (pos == start) return false;  // '.' 后没数字
        while (digits < 3) {
            v *= 10;
            ++digits;
        }
        frac_ms = v;
    }

    // 可选时区：Z/z、±HH:MM、±HHMM、±HH；缺省视为 UTC
    std::int64_t off = 0;
    if (pos < text.size()) {
        const char z = text[pos];
        if (z == 'Z' || z == 'z') {
            ++pos;
        } else if (z == '+' || z == '-') {
            ++pos;
            long oh, om = 0;
            if (!read_int(text, pos, 2, oh)) return false;
            if (pos < text.size() && text[pos] == ':') ++pos;
            if (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                if (!read_int(text, pos, 2, om)) return false;
            }
            if (oh > 23 || om > 59) return false;
            off = oh * 3600 + om * 60;
            if (z == '-') off = -off;
        } else {
            return false;  // 尾随垃圾
        }
    }
    if (pos != text.size()) return false;

    // 字段范围校验（越界 → 非法）
    if (mo < 1 || mo > 12) return false;
    if (y < 0 || y > 9999) return false;
    if (d < 1 || d > days_in_month(static_cast<int>(y), static_cast<int>(mo))) {
        return false;
    }
    if (hh > 23 || mi > 59 || ss > 60) return false;  // 60：容忍闰秒 23:59:60

    // 已用 days_in_month 校验过，days 不会命中 days_from_civil 的 -1 失败值
    const std::int64_t days =
        days_from_civil(static_cast<int>(y), static_cast<int>(mo),
                        static_cast<int>(d));
    const std::int64_t ms = (days * 86400 + hh * 3600 + mi * 60 + ss - off) *
                                1000 +
                            frac_ms;
    using dur = std::chrono::system_clock::duration;
    out = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<dur>(std::chrono::milliseconds(ms)));
    return true;
}

}  // namespace libmini