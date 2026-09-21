#include "crc.h"

#include <zlib.h>

namespace libmini {

std::uint32_t Crc32::compute(const void* data, std::size_t size)
{
    // zlib 的 crc32 要求非 const 指针，这里做无修改的转换
    return static_cast<std::uint32_t>(::crc32(
        0, static_cast<const Bytef*>(data), static_cast<uInt>(size)));
}

std::uint32_t Crc32::compute(const std::string& data)
{
    return compute(data.data(), data.size());
}

void Crc32::update(const void* data, std::size_t size)
{
    value_ = static_cast<std::uint32_t>(::crc32(
        value_, static_cast<const Bytef*>(data), static_cast<uInt>(size)));
}

void Crc32::update(const std::string& data)
{
    update(data.data(), data.size());
}

// ---------------- CRC-16/Modbus ----------------
// 多项式 0x8005（反射 0xA001），初值 0xFFFF，无最终异或。

std::uint16_t crc16_update(std::uint16_t crc, const unsigned char* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1) {
                crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

std::uint16_t Crc16Modbus::compute(const void* data, std::size_t size)
{
    return crc16_update(0xFFFF, static_cast<const unsigned char*>(data), size);
}

std::uint16_t Crc16Modbus::compute(const std::string& data)
{
    return compute(data.data(), data.size());
}

void Crc16Modbus::update(const void* data, std::size_t size)
{
    value_ = crc16_update(value_, static_cast<const unsigned char*>(data), size);
}

void Crc16Modbus::update(const std::string& data)
{
    update(data.data(), data.size());
}

// ---------------- CRC-64/XZ ----------------
// ECMA-182 多项式反射：0xC96C5795D7870F42，初值全 F，输出异或全 F。

std::uint64_t crc64_update(std::uint64_t crc, const unsigned char* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xC96C5795D7870F42ULL;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

std::uint64_t Crc64::compute(const void* data, std::size_t size)
{
    return crc64_update(0xFFFFFFFFFFFFFFFFULL, static_cast<const unsigned char*>(data), size) ^
           0xFFFFFFFFFFFFFFFFULL;
}

std::uint64_t Crc64::compute(const std::string& data)
{
    return compute(data.data(), data.size());
}

void Crc64::update(const void* data, std::size_t size)
{
    value_ = crc64_update(value_, static_cast<const unsigned char*>(data), size);
}
// value() 已做最终异或（见头文件）

void Crc64::update(const std::string& data)
{
    update(data.data(), data.size());
}

// ---------------- Adler-32 ----------------
// zlib 实现（RFC 1950：A=1 起始，B 累加 A，结果 (B<<16)|A）。

std::uint32_t Adler32::compute(const void* data, std::size_t size)
{
    return static_cast<std::uint32_t>(::adler32(
        1, static_cast<const Bytef*>(data), static_cast<uInt>(size)));
}

std::uint32_t Adler32::compute(const std::string& data)
{
    return compute(data.data(), data.size());
}

void Adler32::update(const void* data, std::size_t size)
{
    value_ = static_cast<std::uint32_t>(::adler32(
        value_, static_cast<const Bytef*>(data), static_cast<uInt>(size)));
}

void Adler32::update(const std::string& data)
{
    update(data.data(), data.size());
}

}  // namespace libmini
