#ifndef LIBMINI_CRC_H
#define LIBMINI_CRC_H

#include <cstdint>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// CRC-32 校验（zlib 实现，ISO-HDLC 标准多项式 0xEDB88320）。
class LIBMINI_API Crc32 {
public:
    Crc32() : value_(0) {}

    // 一次性计算
    static std::uint32_t compute(const std::string& data);
    static std::uint32_t compute(const void* data, std::size_t size);

    // 增量计算：可分多块调用 update()，最后取 value()
    void update(const std::string& data);
    void update(const void* data, std::size_t size);
    std::uint32_t value() const { return value_; }
    void reset() { value_ = 0; }

private:
    std::uint32_t value_;
};

// CRC-16/Modbus（多项式 0x8005 反射，初值 0xFFFF，结果低字节在前）。
// Modbus RTU、各类串口/工控协议的标准校验。
class LIBMINI_API Crc16Modbus {
public:
    Crc16Modbus() : value_(0xFFFF) {}

    static std::uint16_t compute(const std::string& data);
    static std::uint16_t compute(const void* data, std::size_t size);

    void update(const std::string& data);
    void update(const void* data, std::size_t size);
    std::uint16_t value() const { return value_; }
    void reset() { value_ = 0xFFFF; }

private:
    std::uint16_t value_;
};

// CRC-64/XZ（ECMA-182 多项式的反射实现：多项式 0xC96C5795D7870F42，
// 初值 0xFFFF'FFFF'FFFF'FFFF，输出前异或同值）。xz 归档、Go hash/crc64
// 采用的变体，归档校验、大文件指纹场景。
class LIBMINI_API Crc64 {
public:
    Crc64() : value_(0xFFFFFFFFFFFFFFFFULL) {}

    static std::uint64_t compute(const std::string& data);
    static std::uint64_t compute(const void* data, std::size_t size);

    void update(const std::string& data);
    void update(const void* data, std::size_t size);
    std::uint64_t value() const { return value_ ^ 0xFFFFFFFFFFFFFFFFULL; }
    void reset() { value_ = 0xFFFFFFFFFFFFFFFFULL; }

private:
    std::uint64_t value_;
};

// Adler-32（zlib 实现）。RFC 1950 zlib 流内建校验，传输块校验常用。
class LIBMINI_API Adler32 {
public:
    Adler32() : value_(1) {}  // adler32 初值 1

    static std::uint32_t compute(const std::string& data);
    static std::uint32_t compute(const void* data, std::size_t size);

    void update(const std::string& data);
    void update(const void* data, std::size_t size);
    std::uint32_t value() const { return value_; }
    void reset() { value_ = 1; }

private:
    std::uint32_t value_;
};

}  // namespace libmini

#endif  // LIBMINI_CRC_H
