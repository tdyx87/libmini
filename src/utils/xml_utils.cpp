#include "xml_utils.h"

#include <functional>
#include <sstream>
#include <stdexcept>

namespace libmini {

// pugixml 的解析错误信息，附加到异常文本中
namespace {

struct XmlParseError : std::runtime_error {
    explicit XmlParseError(const std::string& msg) : std::runtime_error(msg) {}
};

std::string describe(const pugi::xml_parse_result& result)
{
    std::ostringstream ss;
    ss << "xml parse error: " << result.description()
       << " at offset " << result.offset;
    return ss.str();
}

}  // namespace

XmlDocument parse_xml(const std::string& xml_str)
{
    XmlDocument doc;
    // 默认 flag 不保留原始空白节点，输出更干净
    const pugi::xml_parse_result result = doc.load_string(
        xml_str.c_str(), pugi::parse_default | pugi::parse_trim_pcdata);
    if (!result) {
        throw XmlParseError(describe(result));
    }
    return doc;
}

std::string to_xml_string(const XmlDocument& doc)
{
    std::ostringstream ss;
    doc.save(ss, "", pugi::format_default | pugi::format_no_declaration);
    return ss.str();
}

SimpleXmlNode parse_xml_simple(const std::string& xml_str)
{
    // 之前是恒返回空节点的占位实现；现在用 pugixml 真解析，
    // 只提取标签/文本/属性结构（非法输入返回空节点，保持"不抛异常"约定）
    SimpleXmlNode root;
    try {
        const XmlDocument doc = parse_xml(xml_str);
        std::function<void(const XmlNode&, SimpleXmlNode&)> convert =
            [&](const XmlNode& src, SimpleXmlNode& dst) {
                dst.name = src.name();
                dst.value = src.text().as_string();
                for (pugi::xml_attribute attr : src.attributes()) {
                    dst.attributes[attr.name()] = attr.value();
                }
                for (XmlNode child : src.children()) {
                    if (child.type() == pugi::node_element) {
                        SimpleXmlNode c;
                        convert(child, c);
                        dst.children.push_back(c);
                    }
                }
            };
        convert(doc.root().first_child(), root);
    } catch (const std::exception&) {
        return SimpleXmlNode();
    }
    return root;
}

std::string to_xml_string_simple(const SimpleXmlNode& node)
{
    std::stringstream ss;
    ss << "<" << node.name;
    for (const auto& attr : node.attributes) {
        ss << " " << attr.first << "=\"" << attr.second << "\"";
    }
    ss << ">" << node.value;
    for (const auto& child : node.children) {
        ss << to_xml_string_simple(child);
    }
    ss << "</" << node.name << ">";
    return ss.str();
}

}  // namespace libmini