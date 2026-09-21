#include "json_utils.h"
#include <nlohmann/json.hpp>

namespace libmini {

JsonValue parse_json(const std::string& json_str)
{
    return nlohmann::json::parse(json_str);
}

std::string to_json_string(const JsonValue& value)
{
    return value.dump();
}

std::string parse_json_simple(const std::string& json_str)
{
    try {
        // 解析后以紧凑格式 dump：能吃进带空白的 JSON，输出规范化文本；
        // 非法输入抛 parse_error，捕获后返回空串（不抛异常约定）
        return nlohmann::json::parse(json_str).dump();
    } catch (const nlohmann::json::exception&) {
        return std::string();
    }
}

std::string to_json_string_simple(const std::string& value)
{
    // 走 nlohmann 的序列化器：正确转义引号、反斜杠、控制字符，
    // 输出带首尾引号的合法 JSON 字符串字面量
    return nlohmann::json(value).dump();
}

std::string escape_json_string(const std::string& value)
{
    // dump() 结果去掉首尾引号即为纯转义文本
    const std::string dumped = nlohmann::json(value).dump();
    return dumped.substr(1, dumped.size() - 2);
}

}  // namespace libmini