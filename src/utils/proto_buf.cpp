#include "proto_buf.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace libmini {

namespace {

// ---------------------- 输出缓冲 ----------------------

// varint 编码（每字节低 7 位有效，最高位为 continuation）
void put_varint(std::string& out, std::uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

void put_tag(std::string& out, std::uint64_t field_number, ProtoWireType wt)
{
    put_varint(out, (field_number << 3) | static_cast<std::uint32_t>(wt));
}

// length-delimited 帧
void put_length_prefixed(std::string& out, const std::string& payload)
{
    put_varint(out, payload.size());
    out.append(payload);
}

void put_fixed64(std::string& out, double v)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
    }
}

// ---------------------- 写出端 ----------------------

// int64/int32 → varint。proto3 负数以 64 位二补码编码（10 字节）
void write_scalar(std::string& out, std::uint64_t field_number,
                  std::int64_t v)
{
    put_tag(out, field_number, ProtoWireType::Varint);
    put_varint(out, static_cast<std::uint64_t>(v));
}

void write_value(std::string& out, std::uint64_t field_number,
                 const JsonValue& value)
{
    if (value.is_null()) {
        // 空 message 占位：长度为 0 的 length-delimited（官方对空
        // message 也是 0 字节载荷，读取端跳过即可）
        put_tag(out, field_number, ProtoWireType::LengthDelimited);
        put_varint(out, 0);
        return;
    }
    if (value.is_boolean()) {
        write_scalar(out, field_number, value.get<bool>() ? 1 : 0);
        return;
    }
    // 无符号先行：nlohmann 的 is_number_integer() 对无符号数也返回
    // true，反序会把大 uint64 塞进 int64 路径静默回绕
    if (value.is_number_unsigned()) {
        put_tag(out, field_number, ProtoWireType::Varint);
        put_varint(out, value.get<std::uint64_t>());
        return;
    }
    if (value.is_number_integer()) {
        write_scalar(out, field_number, value.get<std::int64_t>());
        return;
    }
    if (value.is_number_float()) {
        // 统一双精度 fixed64（proto3 的 double；float 需要显式约定）
        put_tag(out, field_number, ProtoWireType::Fixed64);
        put_fixed64(out, value.get<double>());
        return;
    }
    if (value.is_string()) {
        put_tag(out, field_number, ProtoWireType::LengthDelimited);
        put_length_prefixed(out, value.get<std::string>());
        return;
    }
    if (value.is_array()) {
        // repeated：同名号逐元素重复（非 packed；官方读取端两种都收）
        for (const auto& item : value) {
            write_value(out, field_number, item);
        }
        return;
    }
    if (value.is_object()) {
        // 嵌套 message / map entry：先编载荷再挂长度
        std::string nested = json_to_proto(value);
        put_tag(out, field_number, ProtoWireType::LengthDelimited);
        put_length_prefixed(out, nested);
        return;
    }
    if (value.is_binary()) {
        // JsonValue::binary → bytes 字段
        put_tag(out, field_number, ProtoWireType::LengthDelimited);
        const auto& bin = value.get_binary();
        put_varint(out, bin.size());
        out.append(reinterpret_cast<const char*>(bin.data()), bin.size());
        return;
    }
    throw std::runtime_error("json_to_proto: unsupported value type");
}

// ---------------------- 读取端 ----------------------

// 输入游标
struct ProtoReader {
    const char* data;
    std::size_t size;
    std::size_t pos;

    // 显式构造器：C++11 下聚合初始化不支持默认成员初始化器，
    // 全部调用点用 ProtoReader(data, size) 形式
    ProtoReader(const char* d, std::size_t s) : data(d), size(s), pos(0) {}

    bool eof() const { return pos >= size; }

    std::uint8_t read_byte()
    {
        if (pos >= size) {
            throw std::runtime_error("proto_to_json: truncated varint");
        }
        return static_cast<std::uint8_t>(data[pos++]);
    }

    std::uint64_t read_varint()
    {
        std::uint64_t result = 0;
        // proto3 varint 上限 10 字节（64 位）；第 10 字节只可能贡献
        // bit63，掩掉高位避免移位出界（0x7F << 63 是 UB）
        for (int i = 0; i < 10; ++i) {
            const std::uint8_t b = read_byte();
            const std::uint64_t chunk =
                (i == 9) ? static_cast<std::uint64_t>(b & 0x01)
                         : static_cast<std::uint64_t>(b & 0x7F);
            result |= chunk << (7 * i);
            if ((b & 0x80) == 0) {
                return result;
            }
        }
        throw std::runtime_error("proto_to_json: varint too long");
    }

    std::string read_bytes(std::uint64_t len)
    {
        if (len > size - pos) {
            throw std::runtime_error("proto_to_json: truncated payload");
        }
        std::string r(data + pos, static_cast<std::size_t>(len));
        pos += static_cast<std::size_t>(len);
        return r;
    }

    double read_fixed64()
    {
        if (size - pos < 8) {
            throw std::runtime_error("proto_to_json: truncated fixed64");
        }
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits |= static_cast<std::uint64_t>(
                        static_cast<std::uint8_t>(data[pos + i]))
                    << (8 * i);
        }
        pos += 8;
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    float read_fixed32()
    {
        if (size - pos < 4) {
            throw std::runtime_error("proto_to_json: truncated fixed32");
        }
        std::uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) {
            bits |= static_cast<std::uint32_t>(
                        static_cast<std::uint8_t>(data[pos + i]))
                    << (8 * i);
        }
        pos += 4;
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
};

// varint 的符号语义：proto3 没有 ZigZag 时，负 int32/int64 都按 64 位
// 二补码走 varint——读取端无法区分「64 位大数」与「负 int32」。选择
// 与官方 JSON 映射一致：值 >= 2^63 按无符号数表示，其余按有符号。
JsonValue varint_to_json(std::uint64_t v)
{
    if (v & (1ULL << 63)) {
        return JsonValue(v);  // 无符号路径（含负数二补码，见头文件注释）
    }
    return JsonValue(static_cast<std::int64_t>(v));
}

void append_to_field(JsonValue& obj, const std::string& key, JsonValue&& item)
{
    auto it = obj.find(key);
    if (it == obj.end()) {
        obj[key] = std::move(item);
        return;
    }
    if (!it->is_array()) {
        // 同字段号第二次出现：转数组（repeated / packed 展开的载体）
        JsonValue arr = JsonValue::array();
        arr.push_back(*it);
        arr.push_back(std::move(item));
        obj[key] = std::move(arr);
        return;
    }
    it->push_back(std::move(item));
}

// packed repeated 载荷展开：把整段 length-delimited 载荷按对应 wire
// type 切成数组（官方 packed 编码）；供 unfold_packed 使用
JsonValue unpack_packed(const std::string& payload, ProtoWireType wire)
{
    ProtoReader inner(payload.data(), payload.size());
    JsonValue arr = JsonValue::array();
    while (!inner.eof()) {
        switch (wire) {
            case ProtoWireType::Varint:
                arr.push_back(varint_to_json(inner.read_varint()));
                break;
            case ProtoWireType::Fixed64:
                arr.push_back(inner.read_fixed64());
                break;
            case ProtoWireType::Fixed32:
                arr.push_back(static_cast<double>(inner.read_fixed32()));
                break;
            default:
                throw std::runtime_error(
                    "unfold_packed: wire type is not packed-compatible");
        }
    }
    return arr;
}

JsonValue read_message(ProtoReader& r, std::uint64_t limit)
{
    const std::size_t end = static_cast<std::size_t>(limit);
    JsonValue obj = JsonValue::object();

    while (r.pos < end) {
        const std::uint64_t tag = r.read_varint();
        const std::uint32_t wire = static_cast<std::uint32_t>(tag & 0x7);
        const std::uint64_t field = tag >> 3;
        if (field == 0) {
            throw std::runtime_error("proto_to_json: field number 0");
        }
        const std::string key = std::to_string(field);

        switch (wire) {
            case static_cast<std::uint32_t>(ProtoWireType::Varint): {
                append_to_field(obj, key, varint_to_json(r.read_varint()));
                break;
            }
            case static_cast<std::uint32_t>(ProtoWireType::Fixed64): {
                append_to_field(obj, key, r.read_fixed64());
                break;
            }
            case static_cast<std::uint32_t>(ProtoWireType::LengthDelimited): {
                const std::uint64_t len = r.read_varint();
                const std::string payload = r.read_bytes(len);
                // 空载荷：空 message 与空 string/bytes 线上同为 0 字节，
                // 无 schema 不可区分——取 null（与写出端 null→空载荷
                // 的编码约定互逆，自有数据往返无损）
                if (payload.empty()) {
                    append_to_field(obj, key, JsonValue(nullptr));
                    break;
                }
                // 嵌套 message 探测优先：载荷能完整解析为合法 message
                //（字段号/wire type/长度全部自洽）时按对象返回，否则按
                // 字符串原样保留。packed repeated 会被误读为字符串，
                // 已知 schema 时用 unfold_packed() 显式展开
                try {
                    ProtoReader probe(payload.data(), payload.size());
                    JsonValue trial = read_message(probe, payload.size());
                    if (probe.pos == payload.size()) {
                        append_to_field(obj, key, std::move(trial));
                        break;
                    }
                } catch (const std::runtime_error&) {
                }
                append_to_field(obj, key, JsonValue(payload));
                break;
            }
            case static_cast<std::uint32_t>(ProtoWireType::Fixed32): {
                append_to_field(obj, key,
                                static_cast<double>(r.read_fixed32()));
                break;
            }
            case 3:  // group（proto2 遗留）：proto3 不允许，报错
            case 4:
                throw std::runtime_error(
                    "proto_to_json: wire type group not supported");
            default:
                throw std::runtime_error("proto_to_json: unknown wire type");
        }
    }
    return obj;
}

}  // namespace

LIBMINI_API std::string json_to_proto(const JsonValue& value)
{
    if (!value.is_object()) {
        throw std::runtime_error(
            "json_to_proto: top-level value must be an object (message)");
    }
    std::string out;
    for (auto it = value.begin(); it != value.end(); ++it) {
        // 键 = 字段号（十进制）；非数字键非法——proto3 线上格式只认号
        const std::string& key = it.key();
        std::uint64_t field = 0;
        try {
            std::size_t consumed = 0;
            field = std::stoull(key, &consumed);
            if (consumed != key.size() || field == 0 ||
                field > (1ULL << 29) - 1) {
                throw std::invalid_argument("range");
            }
        } catch (const std::exception&) {
            throw std::runtime_error(
                "json_to_proto: object keys must be field numbers 1..2^29-1");
        }
        write_value(out, field, it.value());
    }
    return out;
}

LIBMINI_API JsonValue proto_to_json(const std::string& bytes)
{
    ProtoReader r{bytes.data(), bytes.size()};
    return read_message(r, bytes.size());
}

LIBMINI_API JsonValue proto_to_json_or(const std::string& bytes,
                                       const JsonValue& fallback)
{
    try {
        return proto_to_json(bytes);
    } catch (const std::runtime_error&) {
        return fallback;
    }
}

LIBMINI_API void unfold_packed(JsonValue& tree, const std::string& key,
                              ProtoWireType wire)
{
    auto it = tree.find(key);
    if (it == tree.end()) {
        return;  // 无该字段：原样返回
    }
    if (!it->is_string()) {
        throw std::runtime_error(
            "unfold_packed: field was not decoded as raw bytes");
    }
    *it = unpack_packed(it->get<std::string>(), wire);
}

}  // namespace libmini
