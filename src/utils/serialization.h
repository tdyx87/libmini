#ifndef LIBMINI_SERIALIZATION_H
#define LIBMINI_SERIALIZATION_H

#include <string>

#include "json_utils.h"
#include "export.h"

namespace libmini {

// ============================== JSON 序列化 ==============================
//
// 序列化：serialize_to_json(obj) 把对象编码为 JSON 文本。
// 支持（全部由 nlohmann::json 的转换规则提供）：
//   - 标量：int/long long/double/bool/字符串等
//   - STL 容器：std::vector/std::map/std::unordered_map 等
//   - 自定义结构体：声明 NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Type, 字段...)
//     或 NLOHMANN_DEFINE_TYPE_INTRUSIVE(Type, 字段...) 后即可用
// 反序列化：deserialize_from_json<T>(text)，非法 JSON 或字段缺失时抛
// nlohmann::json::exception；不想处理异常用 deserialize_from_json_or<T>。

template <typename T>
std::string serialize_to_json(const T& obj)
{
    return JsonValue(obj).dump();
}

template <typename T>
T deserialize_from_json(const std::string& json_str)
{
    return JsonValue::parse(json_str).get<T>();
}

// 不抛异常版本：任何失败（非法 JSON / 类型不匹配 / 字段缺失）返回 fallback
template <typename T>
T deserialize_from_json_or(const std::string& json_str, const T& fallback)
{
    try {
        return deserialize_from_json<T>(json_str);
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

// ============================== XML 序列化 ==============================
//
// 采用"JSON 树 ↔ XML 元素"的通用映射，因此 JSON 能序列化的类型这里同样能：
//   - 对象        → 子元素           {"port":8080}       → <port>8080</port>
//   - 数组        → 重复的 <item>    [1,2]               → <item>1</item><item>2</item>
//   - 标量        → 元素文本         true                → <value>true</value>
//   - 空对象/数组 → <value type="object|array"/>
//
// 类型恢复按元素内容推断（true/false → bool、整数/浮点 → 数值、其余 → 字符串）；
// std::string 特化为始终读作原文，不做数字/布尔猜测，往返无损。
//
// 注意：XML 表达有损于 JSON 的部分（字段顺序不保留、键作标签名受 XML 命名
// 约束），适用于结构规整的数据。

// ---------------------- JSON 树 ↔ XML 文本 互转 ----------------------
// 序列化实现内部使用，也公开出来方便与 json_utils/XML 工具组合。
// 声明必须在下方模板之前：GCC 的两阶段查找要求依赖名在模板定义点可见
//（MSVC permissive 模式会放行，不可依赖）

// JSON 树 → XML 文本（根元素 <value>，无声明头）
LIBMINI_API std::string json_to_xml(const JsonValue& value);

// XML 文本 → JSON 树；非法 XML 抛 std::runtime_error（带 pugixml 错误描述）
LIBMINI_API JsonValue xml_to_json(const std::string& xml_str);

template <typename T>
std::string serialize_to_xml(const T& obj)
{
    // 根元素名固定为 <value>；单元素文档无需声明头
    return json_to_xml(JsonValue(obj));
}

template <typename T>
T deserialize_from_xml(const std::string& xml_str)
{
    return xml_to_json(xml_str).get<T>();
}

// 不抛异常版本：非法 XML（含 xml_to_json 的 runtime_error）/ 类型不匹配返回 fallback
template <typename T>
T deserialize_from_xml_or(const std::string& xml_str, const T& fallback)
{
    try {
        return deserialize_from_xml<T>(xml_str);
    } catch (const nlohmann::json::exception&) {
        return fallback;
    } catch (const std::runtime_error&) {
        return fallback;
    }
}

// std::string 的 XML 特化（声明在此、定义于 serialization.cpp）：
// 字符串始终按原文读写，不做数字/布尔推断，"0089" 等内容往返不变形
template <>
LIBMINI_API std::string serialize_to_xml<std::string>(const std::string& obj);

template <>
LIBMINI_API std::string deserialize_from_xml<std::string>(const std::string& xml_str);

// json_to_xml / xml_to_json 的声明见上方模板段之前（两阶段查找要求）

}  // namespace libmini

#endif  // LIBMINI_SERIALIZATION_H
