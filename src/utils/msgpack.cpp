#include "msgpack.h"

#include <vector>

namespace libmini {

// nlohmann::json 内置 MsgPack 全集编解码（to_msgpack/from_msgpack），
// 输入非法时抛 nlohmann::json::parse_error，与 JSON 路径的错误形态一致，
// 调用方可以用同一套 try/catch 或 _or 兜底处理三种格式。
//
// 注意 JsonValue(nlohmann::json) 的 binary 类型：to_msgpack 原样输出 bin
// 家族格式；from_msgpack 解出的 bin 在 JsonValue 里也是 binary 类型，
// get<std::string>() 会抛类型错误——二进制载荷建议用
// JsonValue::binary(value, subtype) 显式构造，读取时 is_binary() 分支处理。

LIBMINI_API std::string json_to_msgpack(const JsonValue& value)
{
    // nlohmann 的 msgpack 编解码是静态成员：to_msgpack 返回字节容器
    const std::vector<std::uint8_t> bytes = JsonValue::to_msgpack(value);
    return std::string(bytes.begin(), bytes.end());
}

LIBMINI_API JsonValue msgpack_to_json(const std::string& bytes)
{
    // strict=true：整段输入必须恰好是一个 MsgPack 值，尾随垃圾字节报错，
    // 与 JSON parse 的严格语义一致（截断的流式帧不会静默吞掉尾部）
    return JsonValue::from_msgpack(bytes, true);
}

}  // namespace libmini
