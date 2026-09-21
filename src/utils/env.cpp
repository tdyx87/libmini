#include "env.h"

#include <cstdlib>

#ifdef _WIN32
#include "win_utf.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace libmini {

// Windows：统一用 W 版 API（内部按 UTF-8 <-> UTF-16 转换，见 win_utf.h）

std::string env_get(const std::string& name, const std::string& default_value)
{
    const std::wstring wname = internal::utf8_to_wide(name);
    const DWORD need = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need == 0) {
        return default_value;  // 变量不存在
    }
    std::wstring wvalue(static_cast<std::size_t>(need), L'\0');
    ::GetEnvironmentVariableW(wname.c_str(), &wvalue[0], need);
    wvalue.resize(wvalue.find(L'\0') == std::wstring::npos
                      ? wvalue.size()
                      : wvalue.find(L'\0'));
    return internal::wide_to_utf8(wvalue);
}

bool env_has(const std::string& name)
{
    return ::GetEnvironmentVariableW(internal::utf8_to_wide(name).c_str(),
                                     nullptr, 0) != 0 ||
           ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

bool env_set(const std::string& name, const std::string& value)
{
    return ::SetEnvironmentVariableW(internal::utf8_to_wide(name).c_str(),
                                     internal::utf8_to_wide(value).c_str()) != 0;
}

bool env_remove(const std::string& name)
{
    return ::SetEnvironmentVariableW(internal::utf8_to_wide(name).c_str(),
                                     nullptr) != 0;
}

#else  // POSIX

#include <cctype>

namespace libmini {

std::string env_get(const std::string& name, const std::string& default_value)
{
    const char* v = ::getenv(name.c_str());
    return v != nullptr ? std::string(v) : default_value;
}

bool env_has(const std::string& name)
{
    return ::getenv(name.c_str()) != nullptr;
}

bool env_set(const std::string& name, const std::string& value)
{
    return ::setenv(name.c_str(), value.c_str(), 1) == 0;
}

bool env_remove(const std::string& name)
{
    return ::unsetenv(name.c_str()) == 0;
}

#endif  // _WIN32

// %VAR% 展开逻辑平台无关（只依赖 env_get/env_has）；POSIX 上同样支持
// （环境变量名本身不含 %，语义无冲突）
std::string env_expand(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '%') {
            const std::size_t close = text.find('%', i + 1);
            if (close != std::string::npos && close > i + 1) {
                const std::string name = text.substr(i + 1, close - i - 1);
                const std::string value = env_get(name);
                if (!value.empty() || env_has(name)) {
                    out += value;
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

}  // namespace libmini
