#ifndef LIBMINI_PROTO_BUF_H
#define LIBMINI_PROTO_BUF_H

#include <cstdint>
#include <string>

#include "json_utils.h"
#include "serialization.h"
#include "export.h"

namespace libmini {

// ============================== ProtoBuf（proto3 wire format）==============
//
// 与官方 protobuf 线上字节格式完全兼容的编解码器（wire types：
// varint 0 / fixed64 1 / length-delimited 2 / fixed32 5）。设计动机：
//   - 官方 protobuf 运行时 + protoc 生成代码在 MinGW/MSVC 静态构建下
//     依赖过重，与 libmini「自包含、少第三方」的定位不符；
//   - 大多数使用场景只需要把一颗「字段号 → 值」的树编进线上格式，
//     本模块以 JsonValue 树 + 字段号约定表达同样的线上字节。
//
// 表示法：对象 = message，键为十进制字段号字符串（proto3 里字段号就是
// 线上身份，无名字）：
//   JsonValue msg;
//   msg["1"] = 150;              // int32/int64 → varint（负数 10 字节）
//   msg["2"] = 1.5;              // double → fixed64；float → fixed32
//   msg["3"] = "testing";        // string/bytes → length-delimited
//   msg["4"] = JsonValue::array({...});   // repeated 字段：同名号出现多次
//   msg["5"] = JsonValue::object();       // 嵌套 message / map 条目
//   msg["6"] = true;             // bool → varint 0/1
//   msg["7"] = JsonValue(nullptr);        // 表示 proto3 的空 message 占位
//
// 与官方互操作：std::message.SerializeToString() 的输出可直接用
// proto_to_json() 解析（字段名按官方生成的二进制序号形式），反之亦然。
//
//   std::string bytes = json_to_proto(msg);
//   JsonValue back = proto_to_json(bytes);                    // 抛异常版
//   JsonValue back = proto_to_json_or(bytes, JsonValue());    // 不抛异常版
//
// 序列化模板（与 JSON/MsgPack 同风格，值为树/标量均可）：
//   std::string bytes = serialize_to_proto(obj);
//   T obj = deserialize_from_proto<T>(bytes);
//   T obj = deserialize_from_proto_or<T>(bytes, fallback);
//
// 注意：
//   - unknown 字段（解析方未定义的号）保留为原样值树，往返不丢；
//   - repeated 数组写出为逐元素重复 tag（非 packed，规范允许）；
//   - packed repeated 与嵌套 message 线上同为 length-delimited，官方也要
//     靠 schema 区分——本模块以嵌套 message 探测优先，packed 场景请用
//     unfold_packed() 显式展开（见下）。

// wire types（proto3 规范）
enum class ProtoWireType : std::uint32_t {
    Varint          = 0,
    Fixed64         = 1,
    LengthDelimited = 2,
    Fixed32         = 5,
};

// packed repeated 展开工具：proto_to_json 只能按「嵌套 message 探测
// 优先」解读 length-delimited 字段——若字段号在 schema 里是 packed
// repeated 数值（官方默认），探测会把整段载荷误判成字符串/对象。已知
// schema 时用此工具显式展开：
//   JsonValue v = proto_to_json(bytes);
//   unfold_packed(v, "5", ProtoWireType::Varint);   // 字段 5 是 packed int32
// 无该字段的树原样返回；字段值非 length-delimited 时抛 std::runtime_error。
LIBMINI_API void unfold_packed(JsonValue& tree, const std::string& key,
                              ProtoWireType wire);

// JsonValue 树 → proto3 线上字节流
LIBMINI_API std::string json_to_proto(const JsonValue& value);

// proto3 线上字节流 → JsonValue 树。非法 wire type / 截断流抛
// std::runtime_error；字符串字段按 UTF-8 原样保留（bytes 语义）。
// packed repeated 数值字段自动展开为 JSON 数组。
LIBMINI_API JsonValue proto_to_json(const std::string& bytes);

// 不抛异常版本：非法输入返回 fallback
LIBMINI_API JsonValue proto_to_json_or(const std::string& bytes,
                                       const JsonValue& fallback);

// ---------------------- 类型化便捷封装 ----------------------

template <typename T>
std::string serialize_to_proto(const T& obj)
{
    return json_to_proto(JsonValue(obj));
}

template <typename T>
T deserialize_from_proto(const std::string& bytes)
{
    return proto_to_json(bytes).get<T>();
}

template <typename T>
T deserialize_from_proto_or(const std::string& bytes, const T& fallback)
{
    try {
        return deserialize_from_proto<T>(bytes);
    } catch (const std::runtime_error&) {
        return fallback;
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

}  // namespace libmini

#endif  // LIBMINI_PROTO_BUF_H
