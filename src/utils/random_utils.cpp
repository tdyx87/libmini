#include "random_utils.h"

namespace libmini {

std::mt19937& rng_engine()
{
    // 函数内 thread_local：每线程一个实例，惰性初始化，无初始化顺序问题
    static thread_local std::mt19937 engine(std::random_device{}());
    return engine;
}

int random_int(int min_value, int max_value)
{
    std::uniform_int_distribution<int> dist(min_value, max_value);
    return dist(rng_engine());
}

double random_double(double min_value, double max_value)
{
    std::uniform_real_distribution<double> dist(min_value, max_value);
    return dist(rng_engine());
}

std::string random_string(std::size_t length, const std::string& alphabet)
{
    std::string out;
    out.reserve(length);
    if (alphabet.empty()) {
        return out;
    }
    std::uniform_int_distribution<std::size_t> dist(0, alphabet.size() - 1);
    for (std::size_t i = 0; i < length; ++i) {
        out += alphabet[dist(rng_engine())];
    }
    return out;
}

}  // namespace libmini
