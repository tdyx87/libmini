#include "serialization.h"

#include "xml_utils.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace libmini {

namespace {

// 元素的子元素节点里是否含 XML 元素（跳过 PCDATA 等非元素节点）
bool has_element_child(const pugi::xml_node& node)
{
    for (pugi::xml_node child = node.first_child(); child;
         child = child.next_sibling()) {
        if (child.type() == pugi::node_element) {
            return true;
        }
    }
    return false;
}

// 标量文本 → JSON 值：true/false → bool，其余尝试整数/浮点（要求整段
// 可解析），失败按字符串。写入端为字符串值记录了 type="string"，
// 因此我们自己生成的 XML 不会发生误判。
JsonValue scalar_to_json(const char* type_attr, const std::string& text)
{
    if (std::strcmp(type_attr, "string") == 0) {
        return JsonValue(text);
    }
    if (std::strcmp(type_attr, "null") == 0 || text == "null") {
        return JsonValue(nullptr);
    }
    if (text == "true") {
        return JsonValue(true);
    }
    if (text == "false") {
        return JsonValue(false);
    }
    if (!text.empty()) {
        // 无符号先行（避免 >int64max 时 strtoll 溢出成 LONG_MAX）
        if (text[0] >= '0' && text[0] <= '9') {
            char* end = NULL;
            const unsigned long long u = std::strtoull(text.c_str(), &end, 10);
            if (end != text.c_str() && *end == '\0') {
                return JsonValue(static_cast<std::uint64_t>(u));
            }
        } else {
            char* end = NULL;
            const long long i = std::strtoll(text.c_str(), &end, 10);
            if (end != text.c_str() && *end == '\0') {
                return JsonValue(static_cast<std::int64_t>(i));
            }
        }
        char* dend = NULL;
        const double d = std::strtod(text.c_str(), &dend);
        if (dend != text.c_str() && *dend == '\0') {
            return JsonValue(d);
        }
    }
    return JsonValue(text);
}

// 元素 → JSON：
//   type="object|array"        → 按标注恢复
//   含子元素（未标注）         → 对象（兼容手写 XML，如 <config><timeout/>）
//   否则                       → 标量推断
JsonValue json_from_element(const pugi::xml_node& node)
{
    const char* type_attr = node.attribute("type").value();

    if (std::strcmp(type_attr, "array") == 0) {
        JsonValue arr = JsonValue::array();
        for (pugi::xml_node child = node.first_child(); child;
             child = child.next_sibling()) {
            if (child.type() != pugi::node_element) {
                continue;
            }
            arr.push_back(json_from_element(child));
        }
        return arr;
    }

    if (std::strcmp(type_attr, "object") == 0 || has_element_child(node)) {
        JsonValue obj = JsonValue::object();
        for (pugi::xml_node child = node.first_child(); child;
             child = child.next_sibling()) {
            if (child.type() != pugi::node_element) {
                continue;
            }
            obj[child.name()] = json_from_element(child);
        }
        return obj;
    }

    return scalar_to_json(type_attr, node.text().as_string());
}

// JSON 值 → XML 元素：
//   对象/数组 → type 属性标注 + 子元素（数组子元素名固定 item）
//   字符串    → type="string" + 文本（pugixml 自动实体转义）
//   其余标量  → 文本（读取时按内容推断）
void write_json_value(pugi::xml_node node, const JsonValue& value)
{
    if (value.is_object()) {
        node.append_attribute("type") = "object";
        for (JsonValue::const_iterator it = value.begin(); it != value.end();
             ++it) {
            write_json_value(node.append_child(it.key().c_str()), it.value());
        }
        return;
    }
    if (value.is_array()) {
        node.append_attribute("type") = "array";
        for (JsonValue::const_iterator it = value.begin(); it != value.end();
             ++it) {
            write_json_value(node.append_child("item"), it.value());
        }
        return;
    }
    if (value.is_string()) {
        node.append_attribute("type") = "string";
        node.text().set(value.get<std::string>().c_str());
        return;
    }
    if (value.is_null()) {
        node.append_attribute("type") = "null";
        return;
    }
    // 数字 / 布尔
    node.text().set(value.dump().c_str());
}

}  // namespace

std::string json_to_xml(const JsonValue& value)
{
    pugi::xml_document doc;
    write_json_value(doc.append_child("value"), value);

    std::ostringstream ss;
    doc.save(ss, "", pugi::format_default | pugi::format_no_declaration);
    // pugixml save 会在文档尾部追加换行，去掉以保持与其他序列化 API 一致
    std::string out = ss.str();
    while (!out.empty() && (out[out.size() - 1] == '\n' ||
                            out[out.size() - 1] == '\r')) {
        out.erase(out.size() - 1);
    }
    return out;
}

JsonValue xml_to_json(const std::string& xml_str)
{
    const XmlDocument doc = parse_xml(xml_str);
    const pugi::xml_node root = doc.root().first_child();
    if (!root) {
        throw std::runtime_error("xml_to_json: document has no root element");
    }
    return json_from_element(root);
}

// ------------------------- std::string XML 特化 -------------------------
// 与通用路径语义一致（写入端本就带 type="string"），显式特化仅为
// 语义自文档化并阻止用户对字符串再包一层推导。

template <>
std::string serialize_to_xml<std::string>(const std::string& obj)
{
    return json_to_xml(JsonValue(obj));
}

template <>
std::string deserialize_from_xml<std::string>(const std::string& xml_str)
{
    return xml_to_json(xml_str).get<std::string>();
}

}  // namespace libmini
