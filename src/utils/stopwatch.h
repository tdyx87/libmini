#ifndef LIBMINI_STOPWATCH_H
#define LIBMINI_STOPWATCH_H

#include <chrono>
#include <cstdint>
#include <string>

#include "libmini.h"

namespace libmini {

// 高精度计时器（对应 boost::timer::cpu_timer 的核心功能）。
// 默认构造即开始计时；支持暂停/恢复、分段计时。
class LIBMINI_API Stopwatch {
public:
    Stopwatch() : start_(clock::now()), elapsed_(clock::duration::zero()), running_(true) {}

    // 是否正在计时
    bool is_running() const { return running_; }

    // 已流逝的毫秒数（暂停期间不计入）
    std::int64_t elapsed_ms() const { return to_ms(elapsed()); }
    std::int64_t elapsed_us() const { return to_us(elapsed()); }
    std::int64_t elapsed_ns() const { return to_ns(elapsed()); }

    // 已流逝秒数（浮点，如 1.5 秒）
    double elapsed_seconds() const
    {
        return static_cast<double>(elapsed_ns()) / 1e9;
    }

    // 人类可读格式，如 "1.234s"、"567ms"、"12us"
    std::string elapsed_string() const;

    // 控制操作
    void pause();     // 暂停计时
    void resume();    // 恢复计时
    void restart();   // 清零并重新开始

private:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    clock::duration elapsed() const;
    static std::int64_t to_ms(clock::duration d);
    static std::int64_t to_us(clock::duration d);
    static std::int64_t to_ns(clock::duration d);

    time_point start_;      // 本段计时的起点
    clock::duration elapsed_;  // 已累计的时长（不含当前段）
    bool running_;
};

}  // namespace libmini

#endif  // LIBMINI_STOPWATCH_H
