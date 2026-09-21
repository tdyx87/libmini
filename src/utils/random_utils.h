#ifndef LIBMINI_RANDOM_UTILS_H
#define LIBMINI_RANDOM_UTILS_H

#include <random>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// 进程级随机引擎（每个线程独立实例，惰性初始化，用 random_device 播种）。
// 一般直接用下面的便捷函数；需要批量生成时取引擎自己用分布更高效。
LIBMINI_API std::mt19937& rng_engine();

// [min, max] 内的均匀随机整数（含两端）
LIBMINI_API int random_int(int min_value, int max_value);

// [min_value, max_value) 内的均匀随机浮点
LIBMINI_API double random_double(double min_value = 0.0, double max_value = 1.0);

// 指定长度的随机字符串，默认字母+数字（可用于临时文件名、请求 ID 等）
LIBMINI_API std::string random_string(std::size_t length,
                                      const std::string& alphabet =
                                          "abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                          "0123456789");

// 从非空容器中随机取一个元素（拷贝返回）
template <typename T>
T random_pick(const std::vector<T>& values)
{
    return values[static_cast<std::size_t>(
        random_int(0, static_cast<int>(values.size()) - 1))];
}

}  // namespace libmini

#endif  // LIBMINI_RANDOM_UTILS_H
