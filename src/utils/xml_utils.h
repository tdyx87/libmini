#ifndef LIBMINI_XML_UTILS_H
#define LIBMINI_XML_UTILS_H

#include <string>
#include <map>
#include <vector>
#include <pugixml.hpp>

namespace libmini {

// XML 文档类型
using XmlDocument = pugi::xml_document;

// XML 节点类型
using XmlNode = pugi::xml_node;

// 简单 XML 节点（保持向后兼容）
struct SimpleXmlNode {
    std::string name;
    std::string value;
    std::map<std::string, std::string> attributes;
    std::vector<SimpleXmlNode> children;
};

// XML 解析
XmlDocument parse_xml(const std::string& xml_str);

// XML 序列化
std::string to_xml_string(const XmlDocument& doc);

// 简单 XML 解析（基本实现，保持向后兼容）
SimpleXmlNode parse_xml_simple(const std::string& xml_str);

// 简单 XML 序列化（保持向后兼容）
std::string to_xml_string_simple(const SimpleXmlNode& node);

}  // namespace libmini

#endif  // LIBMINI_XML_UTILS_H