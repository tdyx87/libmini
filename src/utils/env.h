#ifndef LIBMINI_ENV_H
#define LIBMINI_ENV_H

#include <string>

#include "libmini.h"

namespace libmini {

// 读取环境变量；不存在时返回 default_value
LIBMINI_API std::string env_get(const std::string& name,
                                const std::string& default_value = std::string());

// 是否存在该环境变量
LIBMINI_API bool env_has(const std::string& name);

// 设置环境变量（同时更新进程环境与 CRT，子进程可见）。失败返回 false。
LIBMINI_API bool env_set(const std::string& name, const std::string& value);

// 删除环境变量。失败返回 false。
LIBMINI_API bool env_remove(const std::string& name);

// 展开 text 中的 %VAR% 引用（Windows 风格）。
// 未定义的变量原样保留（如 "%UNDEFINED%" 不变）。
LIBMINI_API std::string env_expand(const std::string& text);

}  // namespace libmini

#endif  // LIBMINI_ENV_H
