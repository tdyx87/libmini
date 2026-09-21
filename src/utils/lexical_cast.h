#ifndef LIBMINI_LEXICAL_CAST_H
#define LIBMINI_LEXICAL_CAST_H

#include <sstream>
#include <string>
#include <typeinfo>

#include "libmini.h"

namespace libmini {

// 转换失败异常（对应 boost::bad_lexical_cast）
class bad_lexical_cast : public std::bad_cast {
public:
    const char* what() const noexcept override
    {
        return "libmini::bad_lexical_cast: source value could not be interpreted as target";
    }
};

// 任意类型 <-> 字符串的转换（对应 boost::lexical_cast）。
// 用法：
//   int n = lexical_cast<int>("42");
//   std::string s = lexical_cast<std::string>(3.14);
// 失败时抛出 bad_lexical_cast。
template <typename Target, typename Source>
Target lexical_cast(const Source& value)
{
    std::stringstream ss;
    if (!(ss << value)) {
        throw bad_lexical_cast();
    }
    // 严格模式（与 boost::lexical_cast 一致）：不接受前后空白，
    // "  42"、"42 "、"42x" 都视为转换失败
    ss >> std::noskipws;
    Target result;
    if (!(ss >> result) || !(ss >> std::ws).eof()) {
        throw bad_lexical_cast();
    }
    return result;
}

// 字符串到字符串：原样返回
template <>
inline std::string lexical_cast<std::string, std::string>(const std::string& value)
{
    return value;
}

// 不抛异常版本：失败时返回默认值（多数解析场景比异常更方便）
template <typename Target>
Target lexical_cast_or(const std::string& value, Target default_value)
{
    try {
        return lexical_cast<Target>(value);
    } catch (const std::exception&) {
        return default_value;
    }
}

}  // namespace libmini

#endif  // LIBMINI_LEXICAL_CAST_H
