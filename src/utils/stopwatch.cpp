#include "stopwatch.h"

#include <cstdio>

namespace libmini {

Stopwatch::clock::duration Stopwatch::elapsed() const
{
    if (!running_) {
        return elapsed_;
    }
    return elapsed_ + (clock::now() - start_);
}

void Stopwatch::pause()
{
    if (running_) {
        elapsed_ += clock::now() - start_;
        running_ = false;
    }
}

void Stopwatch::resume()
{
    if (!running_) {
        start_ = clock::now();
        running_ = true;
    }
}

void Stopwatch::restart()
{
    elapsed_ = clock::duration::zero();
    start_ = clock::now();
    running_ = true;
}

std::int64_t Stopwatch::to_ms(Stopwatch::clock::duration d)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

std::int64_t Stopwatch::to_us(Stopwatch::clock::duration d)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

std::int64_t Stopwatch::to_ns(Stopwatch::clock::duration d)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

std::string Stopwatch::elapsed_string() const
{
    const std::int64_t ns = elapsed_ns();
    char buf[64];
    if (ns < 1000) {
        std::snprintf(buf, sizeof(buf), "%lldns", static_cast<long long>(ns));
    } else if (ns < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.3fus", static_cast<double>(ns) / 1e3);
    } else if (ns < 1000000000LL) {
        std::snprintf(buf, sizeof(buf), "%.3fms", static_cast<double>(ns) / 1e6);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3fs", static_cast<double>(ns) / 1e9);
    }
    return std::string(buf);
}

}  // namespace libmini
