#ifndef LIBMINI_WIN_UTF_H
#define LIBMINI_WIN_UTF_H

// 内部头文件：UTF-8 std::string 与 UTF-16 std::wstring 的相互转换。
// 仅供 libmini 内部 Windows 实现使用，不对外导出。
#ifdef _WIN32

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace libmini {
namespace internal {

// UTF-8 → UTF-16
inline std::wstring utf8_to_wide(const std::string& utf8)
{
    if (utf8.empty()) {
        return std::wstring();
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        &out[0], need);
    return out;
}

// UTF-16 → UTF-8
inline std::string wide_to_utf8(const wchar_t* wide, std::size_t count)
{
    if (count == 0) {
        return std::string();
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide,
                                         static_cast<int>(count), nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(count), &out[0], need,
                        nullptr, nullptr);
    return out;
}

inline std::string wide_to_utf8(const std::wstring& wide)
{
    return wide_to_utf8(wide.data(), wide.size());
}

}  // namespace internal
}  // namespace libmini

#endif  // _WIN32

#endif  // LIBMINI_WIN_UTF_H
