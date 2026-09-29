#ifndef LIBMINI_MSGPACK_H
#define LIBMINI_MSGPACK_H

#include <string>

#include "json_utils.h"
#include "serialization.h"
#include "export.h"

namespace libmini {

// ============================== MsgPack ==============================
//
// 二进制序列化格式（https://msgpack.org），与官方实现字节级兼容：
//   - nlohmann::json 自带 to_msgpack/from_msgpack（MsgPack 规范的全集：
//     nil/bool/int/float/str/bin/array/map/ext），直接复用，零新增依赖；
//   - 与 JSON 的关系：同为「JsonValue 树 ↔ 线上字节」，能 JSON 序列化的
//     类型这里同样能；数值以二进制定长编码，字符串无需转义，体积通常
//     比 JSON 小 20%~50%，编解码也更快（无文本扫描）。
//
//   std::string text = serialize_to_msgpack(obj);          // 任意可 JSON 化类型
//   T obj = deserialize_from_msgpack<T>(bytes);
//   T obj = deserialize_from_msgpack_or<T>(bytes, fallback); // 不抛异常版
//
// 注意：MsgPack 不保留 JSON 的字段顺序语义之外的信息；ext 类型仅经
// JsonValue 树的往返不可表达（解析为 bin），与官方 C++ 实现的默认行为一致。

// JsonValue 树 → MsgPack 字节流
LIBMINI_API std::string json_to_msgpack(const JsonValue& value);

// MsgPack 字节流 → JsonValue 树；非法输入抛 nlohmann::json::parse_error
LIBMINI_API JsonValue msgpack_to_json(const std::string& bytes);

// 便捷封装：任意类型（标量/容器/带 DEFINE_TYPE 宏的结构体）
template <typename T>
std::string serialize_to_msgpack(const T& obj)
{
    return json_to_msgpack(JsonValue(obj));
}

template <typename T>
T deserialize_from_msgpack(const std::string& bytes)
{
    return msgpack_to_json(bytes).get<T>();
}

// 不抛异常版本：任何失败（非法字节流 / 类型不匹配 / 字段缺失）返回 fallback
template <typename T>
T deserialize_from_msgpack_or(const std::string& bytes, const T& fallback)
{
    try {
        return deserialize_from_msgpack<T>(bytes);
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

}  // namespace libmini

#endif  // LIBMINI_MSGPACK_H
