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
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

std::int64_t date_from_string(const std::string& s)
{
    int y = 0, m = 0, d = 0;
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return -1;
    }
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) {
        return -1;
    }
    return days_from_civil(y, m, d);  // 非法日期在内部校验返回 -1
}

}  // namespace libmini