#ifndef LIBMINI_JSON_UTILS_H
#define LIBMINI_JSON_UTILS_H

#include <string>
#include <nlohmann/json.hpp>

namespace libmini {

// JSON 对象类型
using JsonValue = nlohmann::json;

// 解析 JSON 文本；非法输入抛 nlohmann::json::parse_error
JsonValue parse_json(const std::string& json_str);

// 序列化为 JSON 文本（紧凑格式）
std::string to_json_string(const JsonValue& value);

// 解析 JSON 文本为紧凑格式字符串。
//   {"name":"test","value":123}  →  {"name":"test","value":123}
//   { "a" : 1 }                  →  {"a":1}
// 非法输入返回空串（与 parse_json 不同，不抛异常）。
std::string parse_json_simple(const std::string& json_str);

// 将字符串编码为 JSON 字符串字面量（含首尾引号）。
//   to_json_string_simple("say \"hi\"")  →  "say \"hi\""（合法 JSON 文本）
//   to_json_string_simple("a\b")         →  "a\\b"
// 控制字符按 \uXXXX 转义，中文等非 ASCII 字符原样保留（UTF-8）。
std::string to_json_string_simple(const std::string& value);

// 转义字符串内容的 JSON 转义文本（不含首尾引号）。
// 用于手工拼接 JSON 时转义动态值：
//   "{\"msg\":\"" + escape_json_string(user_input) + "\"}"
std::string escape_json_string(const std::string& value);

}  // namespace libmini

#endif  // LIBMINI_JSON_UTILS_H