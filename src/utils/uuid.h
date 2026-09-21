#ifndef LIBMINI_UUID_H
#define LIBMINI_UUID_H

#include <cstdint>
#include <string>

#include "libmini.h"

namespace libmini {

// RFC 4122 v4（随机）UUID，对应 boost::uuids::uuid。
struct LIBMINI_API Uuid {
    std::uint8_t bytes[16];

    // 生成一个随机 UUID（v4，变体位按 RFC 4122 设置）
    static Uuid generate();

    // 标准格式解析："xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"（大小写均可，
    // 也接受不带连字符的 32 位十六进制形式）。失败返回 false。
    static bool parse(const std::string& str, Uuid& out);

    // 标准格式输出（小写、带连字符）
    std::string to_string() const;

    // 不带连字符的 32 位十六进制形式（小写）
    std::string to_hex_string() const;

    bool operator==(const Uuid& other) const;
    bool operator!=(const Uuid& other) const;
    bool operator<(const Uuid& other) const;  // 用于 std::map 键

    // 是否为全零 UUID
    bool is_nil() const;

    // 全零 UUID
    static Uuid nil();
};

}  // namespace libmini

#endif  // LIBMINI_UUID_H
