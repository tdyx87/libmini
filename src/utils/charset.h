#ifndef LIBMINI_CHARSET_H
#define LIBMINI_CHARSET_H

#include <string>

#include "libmini.h"

namespace libmini {

// 文本编码。库对外统一以 UTF-8 的 std::string 为口径，其它编码只在边界
//（读文件、对外部程序、写数据库）处转换。
enum class Charset {
    Utf8,     // 变长 1~4 字节，无 BOM
    Utf16Le,  // 2 字节小端，无 BOM（Windows wchar_t / Win32 W 系 API 的内存布局）
    Utf16Be,  // 2 字节大端，无 BOM（网络字节序、部分文件格式）
    Gbk,      // 简体中文传统编码（Windows 代码页 936；GBK ⊂ GB18030）
};

// 转换结果（本模块与库内其它模块一致：不抛异常，用状态码表达失败）
enum class CharsetStatus {
    Ok,
    Unsupported,      // 当前平台/本次构建不支持源编码或目标编码
    InvalidSequence,  // 输入不是合法的源编码（含末尾被截断的多字节序列）
};

// 字符编码转换。
//
//   std::string utf8 = CharsetConverter::to_utf8(gbk_bytes, Charset::Gbk);
//   std::string gbk  = CharsetConverter::from_utf8(text, Charset::Gbk);
//
//   std::string out;
//   CharsetStatus st = CharsetConverter::convert(in, Charset::Gbk,
//                                                Charset::Utf16Le, out);
//
// 实现策略（与 aes_gcm 的「平台原生优先」一致，不新增第三方依赖）：
//   - Windows：MultiByteToWideChar / WideCharToMultiByte，UTF-16 作中枢。
//   - 其它平台：iconv（glibc 内建；macOS 由 libSystem 提供），
//     不依赖任何 Windows API。
//   - 定义 LIBMINI_CHARSET_USE_ICONV 可强制在 Windows 上也走 iconv
//     （需自行链接 libiconv），用于统一依赖或比对两种实现。
//
// 失败语义：
//   - 非法或截断的输入一律 InvalidSequence，不会静默替换成 '?' / U+FFFD ——
//     静默替换会让「编码选错了」这种问题一路潜伏到用户界面。
//     注意 Windows 的代码页 936 出向不支持 WC_ERR_INVALID_CHARS，实现改用
//     lpUsedDefaultChar 检出被替换的字符，语义与 POSIX 侧的 EILSEQ 对齐。
//   - 平台不支持该编码（例如精简 libc 缺 GBK 模块）返回 Unsupported，
//     可用 is_supported() 预判。
//   - from == to 时原样返回输入，不做合法性校验。
//   - 线程安全：无共享可变状态，任意线程可并发调用。
class LIBMINI_API CharsetConverter {
public:
    // 转换成功返回 Ok 并写入 output；失败时 output 不被修改
    static CharsetStatus convert(const std::string& input,
                                 Charset from,
                                 Charset to,
                                 std::string& output);

    // 该编码对在当前平台是否可用（内部真实尝试建立一次转换器）
    static bool is_supported(Charset from, Charset to);

    // 便捷封装：失败返回空串；ok 非空时写入是否成功
    static std::string to_utf8(const std::string& input, Charset from,
                               bool* ok = nullptr);
    static std::string from_utf8(const std::string& utf8, Charset to,
                                 bool* ok = nullptr);

    // 可读名称（"UTF-8" / "UTF-16LE" / "UTF-16BE" / "GBK"），供日志使用；
    // 同时也是 POSIX 侧 iconv 的编码名
    static const char* name(Charset charset);
    static const char* status_message(CharsetStatus status);
};

}  // namespace libmini

#endif  // LIBMINI_CHARSET_H
