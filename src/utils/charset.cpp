#include "charset.h"

#include <cstddef>

// 平台选择：默认 Windows 用 Win32 代码页 API、其它平台用 iconv。
// 定义 LIBMINI_CHARSET_USE_ICONV 可强制走 iconv（需自行链接 libiconv）：
// 用于在 Windows 上比对两种实现，或统一依赖 libiconv 的部署场景
#if defined(_WIN32) && !defined(LIBMINI_CHARSET_USE_ICONV)
#define LIBMINI_CHARSET_USE_WIN32_API 1
#endif

#ifdef LIBMINI_CHARSET_USE_WIN32_API
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <climits>
#else
#include <cerrno>
#include <iconv.h>
#endif

namespace libmini {

namespace {

// ---------------- 平台无关的 UTF-16 校验 ----------------

// 按字节序读取 index 处的 UTF-16 码元
unsigned int utf16_unit_at(const std::string& utf16, std::size_t index,
                           bool little_endian)
{
    const unsigned char b0 = static_cast<unsigned char>(utf16[index]);
    const unsigned char b1 = static_cast<unsigned char>(utf16[index + 1]);
    if (little_endian) {
        return static_cast<unsigned int>(b0) |
               (static_cast<unsigned int>(b1) << 8);
    }
    return (static_cast<unsigned int>(b0) << 8) |
           static_cast<unsigned int>(b1);
}

// UTF-16 字节串是否合法：长度必须为偶数，代理项必须成对。
// 放在公共路径而非各平台实现内，保证 Windows 与 POSIX 的判定完全一致
//（iconv 与 Win32 对孤立代理项的处理未必相同）。
bool valid_utf16(const std::string& utf16, bool little_endian)
{
    if (utf16.size() % 2 != 0) {
        return false;
    }
    const std::size_t units = utf16.size() / 2;
    for (std::size_t i = 0; i < units; ++i) {
        const unsigned int unit = utf16_unit_at(utf16, i * 2, little_endian);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 >= units) {
                return false;  // 末尾孤立的高代理项（被截断）
            }
            const unsigned int low =
                utf16_unit_at(utf16, (i + 1) * 2, little_endian);
            if (low < 0xDC00 || low > 0xDFFF) {
                return false;  // 高代理项后面不是低代理项
            }
            ++i;  // 低代理项已消费
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;  // 孤立低代理项
        }
    }
    return true;
}

#ifdef LIBMINI_CHARSET_USE_WIN32_API

// ---------------- Windows：Win32 代码页 ----------------

// 代码页只覆盖「字节串」编码；UTF-16 走宽字符路径，不经过代码页
UINT code_page_of(Charset charset)
{
    switch (charset) {
        case Charset::Utf8: return CP_UTF8;
        case Charset::Gbk:  return 936;  // 简体中文 Windows 的传统编码
        default:            return 0;
    }
}

bool multibyte_to_wide(const std::string& input, UINT code_page,
                       std::wstring& wide)
{
    if (input.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    if (input.empty()) {
        wide.clear();
        return true;
    }
    const int count = static_cast<int>(input.size());
    // MB_ERR_INVALID_CHARS：非法/截断序列直接失败，不替换成 U+FFFD。
    // 代码页 936 实测接受该标志（拒绝时返回 ERROR_NO_UNICODE_TRANSLATION）
    const int need = ::MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS,
                                           input.data(), count, nullptr, 0);
    if (need <= 0) {
        return false;
    }
    wide.assign(static_cast<std::size_t>(need), L'\0');
    const int written = ::MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS,
                                              input.data(), count, &wide[0], need);
    return written == need;
}

bool wide_to_multibyte(const std::wstring& wide, UINT code_page,
                       std::string& out)
{
    if (wide.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    if (wide.empty()) {
        out.clear();
        return true;
    }
    const int count = static_cast<int>(wide.size());
    // 双字节代码页（如 936）不接受 WC_ERR_INVALID_CHARS，会返回
    // ERROR_INVALID_FLAGS；改由 lpUsedDefaultChar 检出「被替换成 '?'」的字符，
    // 语义与 POSIX 侧 iconv 的 EILSEQ 对齐。UTF-8 支持该标志，直接用它，
    // 此时 lpDefaultChar / lpUsedDefaultChar 必须为 NULL。
    const bool strict = (code_page == CP_UTF8);
    const DWORD flags = strict ? WC_ERR_INVALID_CHARS : 0;
    BOOL used_default = FALSE;

    const int need = ::WideCharToMultiByte(code_page, flags, wide.data(), count,
                                           nullptr, 0, nullptr,
                                           strict ? nullptr : &used_default);
    if (need <= 0 || used_default) {
        return false;
    }
    out.assign(static_cast<std::size_t>(need), '\0');
    used_default = FALSE;
    const int written = ::WideCharToMultiByte(code_page, flags, wide.data(),
                                              count, &out[0], need, nullptr,
                                              strict ? nullptr : &used_default);
    return written == need && !used_default;
}

void utf16_bytes_to_wide(const std::string& input, bool little_endian,
                         std::wstring& wide)
{
    const std::size_t units = input.size() / 2;  // 合法性已在外层校验
    wide.resize(units);
    for (std::size_t i = 0; i < units; ++i) {
        wide[i] = static_cast<wchar_t>(utf16_unit_at(input, i * 2, little_endian));
    }
}

void wide_to_utf16_bytes(const std::wstring& wide, bool little_endian,
                         std::string& out)
{
    out.resize(wide.size() * 2);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const unsigned int unit =
            static_cast<unsigned int>(static_cast<unsigned short>(wide[i]));
        const char hi = static_cast<char>((unit >> 8) & 0xFFu);
        const char lo = static_cast<char>(unit & 0xFFu);
        out[i * 2] = little_endian ? lo : hi;
        out[i * 2 + 1] = little_endian ? hi : lo;
    }
}

CharsetStatus convert_impl(const std::string& input, Charset from, Charset to,
                           std::string& output)
{
    // UTF-16 作中枢：字节串编码先解成宽字符，再编成目标编码
    std::wstring wide;
    if (from == Charset::Utf16Le || from == Charset::Utf16Be) {
        utf16_bytes_to_wide(input, from == Charset::Utf16Le, wide);
    } else if (!multibyte_to_wide(input, code_page_of(from), wide)) {
        return CharsetStatus::InvalidSequence;
    }

    if (to == Charset::Utf16Le || to == Charset::Utf16Be) {
        wide_to_utf16_bytes(wide, to == Charset::Utf16Le, output);
        return CharsetStatus::Ok;
    }
    if (!wide_to_multibyte(wide, code_page_of(to), output)) {
        return CharsetStatus::InvalidSequence;
    }
    return CharsetStatus::Ok;
}

#else

// ---------------- 其它平台（或 LIBMINI_CHARSET_USE_ICONV）：iconv ----------------

// iconv 需要非 const 的输入指针（它不修改输入内容）
bool iconv_convert(const std::string& input, const char* from_name,
                   const char* to_name, std::string& output,
                   CharsetStatus& status)
{
    iconv_t cd = ::iconv_open(to_name, from_name);
    if (cd == (iconv_t)-1) {
        status = CharsetStatus::Unsupported;
        return false;
    }

    std::string buffer(input.size() + 16, '\0');
    std::size_t in_pos = 0;
    std::size_t out_pos = 0;
    int failure = 0;

    for (;;) {
        char* in_ptr = const_cast<char*>(input.data()) + in_pos;
        std::size_t in_left = input.size() - in_pos;
        char* out_ptr = &buffer[0] + out_pos;
        std::size_t out_left = buffer.size() - out_pos;

        const std::size_t rc =
            ::iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        in_pos = input.size() - in_left;
        out_pos = buffer.size() - out_left;

        if (rc != static_cast<std::size_t>(-1)) {
            break;  // 输入耗尽，转换完成
        }
        failure = errno;
        if (failure == E2BIG) {
            buffer.resize(buffer.size() * 2);  // 输出不够，扩容后接着转
            failure = 0;
            continue;
        }
        break;
    }

    ::iconv_close(cd);

    if (failure != 0) {
        // EILSEQ = 非法序列；EINVAL = 末尾被截断的多字节序列
        status = (failure == EILSEQ || failure == EINVAL)
                     ? CharsetStatus::InvalidSequence
                     : CharsetStatus::Unsupported;
        return false;
    }
    buffer.resize(out_pos);
    output = buffer;
    return true;
}

CharsetStatus convert_impl(const std::string& input, Charset from, Charset to,
                           std::string& output)
{
    CharsetStatus status = CharsetStatus::Ok;
    if (!iconv_convert(input, CharsetConverter::name(from),
                       CharsetConverter::name(to), output, status)) {
        return status;
    }
    return CharsetStatus::Ok;
}

#endif  // LIBMINI_CHARSET_USE_WIN32_API

}  // namespace

// ---------------- 公开接口 ----------------

CharsetStatus CharsetConverter::convert(const std::string& input,
                                        Charset from,
                                        Charset to,
                                        std::string& output)
{
    if (from == to) {
        output = input;  // 同编码直通（见头文件：不做合法性校验）
        return CharsetStatus::Ok;
    }
    if (from == Charset::Utf16Le && !valid_utf16(input, true)) {
        return CharsetStatus::InvalidSequence;
    }
    if (from == Charset::Utf16Be && !valid_utf16(input, false)) {
        return CharsetStatus::InvalidSequence;
    }
    return convert_impl(input, from, to, output);
}

bool CharsetConverter::is_supported(Charset from, Charset to)
{
    if (from == to) {
        return true;
    }
#ifdef LIBMINI_CHARSET_USE_WIN32_API
    // 代码页 936 与 UTF-8/UTF-16 自 Windows NT 起始终随系统提供
    return true;
#else
    iconv_t cd = ::iconv_open(name(to), name(from));
    if (cd == (iconv_t)-1) {
        return false;
    }
    ::iconv_close(cd);
    return true;
#endif
}

std::string CharsetConverter::to_utf8(const std::string& input, Charset from,
                                      bool* ok)
{
    std::string output;
    const CharsetStatus status = convert(input, from, Charset::Utf8, output);
    if (ok != nullptr) {
        *ok = (status == CharsetStatus::Ok);
    }
    return status == CharsetStatus::Ok ? output : std::string();
}

std::string CharsetConverter::from_utf8(const std::string& utf8, Charset to,
                                        bool* ok)
{
    std::string output;
    const CharsetStatus status = convert(utf8, Charset::Utf8, to, output);
    if (ok != nullptr) {
        *ok = (status == CharsetStatus::Ok);
    }
    return status == CharsetStatus::Ok ? output : std::string();
}

const char* CharsetConverter::name(Charset charset)
{
    switch (charset) {
        case Charset::Utf8:    return "UTF-8";
        case Charset::Utf16Le: return "UTF-16LE";
        case Charset::Utf16Be: return "UTF-16BE";
        case Charset::Gbk:     return "GBK";
    }
    return "unknown";
}

const char* CharsetConverter::status_message(CharsetStatus status)
{
    switch (status) {
        case CharsetStatus::Ok:
            return "ok";
        case CharsetStatus::Unsupported:
            return "charset not supported on this platform";
        case CharsetStatus::InvalidSequence:
            return "input is not a valid sequence in the source charset";
    }
    return "unknown";
}

}  // namespace libmini
