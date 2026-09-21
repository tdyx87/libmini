#include "ini_config.h"

#include <cctype>

#include "file_utils.h"
#include "lexical_cast.h"
#include "string_algo.h"
#include "string_utils.h"

namespace libmini {

namespace {

// 按行拆分（\n 或 \r\n，孤立 \r 当普通字符处理）
std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (text[i] != '\r') {
            cur += text[i];
        }
    }
    lines.push_back(cur);
    return lines;
}

// 清洗值：截断行内注释（仅当注释符前是空白或行首）、去引号
std::string clean_value(const std::string& raw)
{
    std::string v = raw;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if ((c == ';' || c == '#') && (i == 0 || std::isspace(static_cast<unsigned char>(v[i - 1])))) {
            v = v.substr(0, i);
            break;
        }
    }
    v = trim(v);
    if (v.size() >= 2) {
        const char first = v.front();
        const char last = v.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            v = v.substr(1, v.size() - 2);
        }
    }
    return v;
}

}  // namespace

bool IniConfig::load(const std::string& path)
{
    if (!file_exists(path)) {
        return false;
    }
    parse(read_file(path));
    return true;
}

void IniConfig::parse(const std::string& text)
{
    data_.clear();
    std::string current;  // 空串 = 全局区
    const std::vector<std::string> lines = split_lines(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        if (t.empty() || t[0] == ';' || t[0] == '#') {
            continue;
        }
        if (t[0] == '[') {
            const std::size_t close = t.find(']');
            if (close != std::string::npos) {
                current = trim(t.substr(1, close - 1));
            }
            continue;
        }
        const std::size_t eq = t.find_first_of("=:");
        if (eq == std::string::npos) {
            continue;  // 非法行忽略
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string value = clean_value(t.substr(eq + 1));
        if (!key.empty()) {
            data_[current][key] = value;
        }
    }
}

std::string IniConfig::save() const
{
    std::string out;
    bool first_section = true;
    for (std::map<std::string, std::map<std::string, std::string> >::const_iterator it = data_.begin();
         it != data_.end(); ++it) {
        if (it->second.empty()) {
            continue;
        }
        if (!first_section) {
            out += "\r\n";
        }
        first_section = false;
        if (!it->first.empty()) {
            out += "[" + it->first + "]\r\n";
        }
        for (std::map<std::string, std::string>::const_iterator kv = it->second.begin();
             kv != it->second.end(); ++kv) {
            out += kv->first + "=" + kv->second + "\r\n";
        }
    }
    return out;
}

bool IniConfig::save_to(const std::string& path) const
{
    return write_file(path, save());
}

std::string IniConfig::get(const std::string& section, const std::string& key,
                           const std::string& default_value) const
{
    std::map<std::string, std::map<std::string, std::string> >::const_iterator s =
        data_.find(section);
    if (s == data_.end()) {
        return default_value;
    }
    std::map<std::string, std::string>::const_iterator k = s->second.find(key);
    return (k == s->second.end()) ? default_value : k->second;
}

int IniConfig::get_int(const std::string& section, const std::string& key,
                       int default_value) const
{
    return lexical_cast_or(get(section, key), default_value);
}

double IniConfig::get_double(const std::string& section, const std::string& key,
                             double default_value) const
{
    return lexical_cast_or(get(section, key), default_value);
}

bool IniConfig::get_bool(const std::string& section, const std::string& key,
                         bool default_value) const
{
    const std::string v = get(section, key);
    if (v.empty()) {
        return default_value;
    }
    if (iequals(v, "1") || iequals(v, "true") || iequals(v, "yes") ||
        iequals(v, "on")) {
        return true;
    }
    if (iequals(v, "0") || iequals(v, "false") || iequals(v, "no") ||
        iequals(v, "off")) {
        return false;
    }
    return default_value;
}

void IniConfig::set(const std::string& section, const std::string& key,
                    const std::string& value)
{
    data_[section][key] = value;
}

void IniConfig::remove(const std::string& section, const std::string& key)
{
    std::map<std::string, std::map<std::string, std::string> >::iterator s =
        data_.find(section);
    if (s != data_.end()) {
        s->second.erase(key);
    }
}

void IniConfig::remove_section(const std::string& section)
{
    data_.erase(section);
}

bool IniConfig::has_section(const std::string& section) const
{
    std::map<std::string, std::map<std::string, std::string> >::const_iterator s =
        data_.find(section);
    return s != data_.end() && !s->second.empty();
}

bool IniConfig::has_key(const std::string& section, const std::string& key) const
{
    std::map<std::string, std::map<std::string, std::string> >::const_iterator s =
        data_.find(section);
    return s != data_.end() && s->second.find(key) != s->second.end();
}

std::vector<std::string> IniConfig::sections() const
{
    std::vector<std::string> out;
    for (std::map<std::string, std::map<std::string, std::string> >::const_iterator it =
             data_.begin();
         it != data_.end(); ++it) {
        if (!it->first.empty() && !it->second.empty()) {
            out.push_back(it->first);
        }
    }
    return out;
}

std::vector<std::string> IniConfig::keys(const std::string& section) const
{
    std::vector<std::string> out;
    std::map<std::string, std::map<std::string, std::string> >::const_iterator s =
        data_.find(section);
    if (s != data_.end()) {
        for (std::map<std::string, std::string>::const_iterator it = s->second.begin();
             it != s->second.end(); ++it) {
            out.push_back(it->first);
        }
    }
    return out;
}

void IniConfig::clear()
{
    data_.clear();
}

}  // namespace libmini
