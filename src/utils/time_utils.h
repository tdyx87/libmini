#ifndef LIBMINI_TIME_UTILS_H
#define LIBMINI_TIME_UTILS_H

#include <string>
#include <chrono>
#include <cstdint>

#include "export.h"

namespace libmini {

// 获取当前时间戳（毫秒）
LIBMINI_API long long current_timestamp_ms();

// 获取当前时间字符串（YYYY-MM-DD HH:MM:SS）
LIBMINI_API std::string current_time_string();

// 格式化时间
LIBMINI_API std::string format_time(std::chrono::system_clock::time_point tp, const std::string& format = "%Y-%m-%d %H:%M:%S");

// ---------------- 日历运算 ----------------
// 基于 civil-from-days 算法（Howard Hinnant），纯整数无时区歧义。
// "日期"统一用 Unix epoch 起的天数（day number）表示。

// 当月天数（month 1-12；闰年正确处理 2 月）
LIBMINI_API int days_in_month(int year, int month);

// 是否闰年（格里高利历规则）
LIBMINI_API bool is_leap_year(int year);

// day number（1970-01-01 = 0）与年月日互转。civil_from_days 失败返回 false
LIBMINI_API bool civil_from_days(std::int64_t days, int& year, int& month, int& day);
LIBMINI_API std::int64_t days_from_civil(int year, int month, int day);

// 月份加减（日号自动钳制到目标月末：1/31 + 1月 = 2/28 或 2/29）
LIBMINI_API std::int64_t add_months(std::int64_t days, int months);

// 星期几（0=周日...6=周六，同 tm_wday 语义）
LIBMINI_API int weekday_of(std::int64_t days);

// 下一个星期几（当天不算：当天是周一，next_weekday(d,1) 返回下周一）
LIBMINI_API std::int64_t next_weekday(std::int64_t days, int weekday);

// 月初/月末的 day number
LIBMINI_API std::int64_t month_start(std::int64_t days);
LIBMINI_API std::int64_t month_end(std::int64_t days);

// day number 与 "YYYY-MM-DD" 互转（解析失败返回 -1）
LIBMINI_API std::string date_to_string(std::int64_t days);
LIBMINI_API std::int64_t date_from_string(const std::string& yyyymmdd);

}

#endif // LIBMINI_TIME_UTILS_H