#include "config_facade.h"

#include <cctype>

#include "env.h"
#include "file_utils.h"
#include "ini_config.h"
#include "json_utils.h"
#include "lexical_cast.h"

#ifdef _WIN32
#include "win_utf.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cstdlib>
#endif

namespace libmini {

namespace {

// 键路径与变量名的归一化形式：小写 + 去掉 '.' 与 '_'。
// "server.port" / "SERVER_PORT" / "serverport" 归一后相同。
std::string normalize_key(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '.' || c == '_') {
            continue;
        }
        r.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    }
    return r;
}

// 从 JSON 值递归展平为键路径表（对象继续下钻，标量作为叶子）
void flatten_json(const JsonValue& v, const std::string& prefix,
                  std::map<std::string, std::string>& out)
{
    if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            flatten_json(it.value(),
                         prefix.empty() ? it.key() : prefix + "." + it.key(),
                         out);
        }
        return;
    }
    // 标量叶子：文本化（数字/布尔/字符串统一）
    if (v.is_string()) {
        out[prefix] = v.get<std::string>();
    } else if (v.is_boolean()) {
        out[prefix] = v.get<bool>() ? "true" : "false";
    } else if (!v.is_null()) {
        out[prefix] = v.dump();
    }
}

}  // namespace

ConfigFacade::ConfigFacade() {}

void ConfigFacade::set_default(const std::string& path, const std::string& value)
{
    defaults_[path] = value;
}

bool ConfigFacade::load_file(const std::string& path)
{
    // 扩展名识别（大小写不敏感）
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = path.substr(dot + 1);
    for (std::size_t i = 0; i < ext.size(); ++i) {
        ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
    }

    std::string text = read_file(path);
    if (!file_exists(path)) {
        return false;
    }

    if (ext == "json") {
        load_text(text, "json");
        return true;
    }
    if (ext == "ini") {
        load_text(text, "ini");
        return true;
    }
    return false;
}

void ConfigFacade::load_text(const std::string& text, const std::string& format)
{
    if (format == "json") {
        try {
            const JsonValue root = parse_json(text);
            flatten_json(root, "", file_values_);
        } catch (const std::exception&) {
            // 非法 JSON：保持原状（与 IniConfig::parse 的宽容语义一致）
        }
        return;
    }
    if (format == "ini") {
        IniConfig ini;
        ini.parse(text);
        // section.key；全局区键记为 ".key"（与 has/keys 的口径一致）
        const std::vector<std::string> secs = ini.sections();
        for (std::size_t i = 0; i < secs.size(); ++i) {
            const std::vector<std::string>& ks = ini.keys(secs[i]);
            for (std::size_t j = 0; j < ks.size(); ++j) {
                const std::string path =
                    secs[i].empty() ? "." + ks[j] : secs[i] + "." + ks[j];
                file_values_[path] = ini.get(secs[i], ks[j]);
            }
        }
        return;
    }
}

std::vector<std::string> ConfigFacade::file_keys() const
{
    std::vector<std::string> out;
    out.reserve(file_values_.size());
    for (auto it = file_values_.begin(); it != file_values_.end(); ++it) {
        out.push_back(it->first);
    }
    return out;
}

void ConfigFacade::set_env_prefix(const std::string& prefix)
{
    env_prefix_ = prefix;
}

void ConfigFacade::refresh_env()
{
    env_lookup_.clear();
    // 跨平台枚举：Windows 用 GetEnvironmentStringsW（W 版，中文不乱码），
    // POSIX 用 environ
    std::map<std::string, std::string> raw;
#ifdef _WIN32
    {
        wchar_t* strings = ::GetEnvironmentStringsW();
        if (strings != nullptr) {
            wchar_t* p = strings;
            while (*p != L'\0') {
                const std::string entry = libmini::internal::wide_to_utf8(p);
                const std::size_t eq = entry.find('=');
                if (eq != std::string::npos && eq > 0) {
                    raw[entry.substr(0, eq)] = entry.substr(eq + 1);
                }
                p += std::wcslen(p) + 1;
            }
            ::FreeEnvironmentStringsW(strings);
        }
    }
#else
    {
        extern char** environ;
        for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
            const std::string entry(*e);
            const std::size_t eq = entry.find('=');
            if (eq != std::string::npos && eq > 0) {
                raw[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
        }
    }
#endif

    const std::string prefix_norm = normalize_key(env_prefix_);
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        const std::string norm = normalize_key(it->first);
        if (norm.empty()) {
            continue;
        }
        if (!prefix_norm.empty() && norm.compare(0, prefix_norm.size(), prefix_norm) != 0) {
            continue;
        }
        // 剥掉前缀（按归一化长度），余下部分作为键路径
        const std::string rest = norm.substr(prefix_norm.size());
        if (rest.empty()) {
            continue;
        }
        // 同一归一化名多个原始变量：取字典序最小的变量名（行为确定）
        auto ex = env_lookup_.find(rest);
        if (ex == env_lookup_.end() || it->first < ex->second.raw_name) {
            env_lookup_[rest] = EnvEntry{it->first, it->second};
        }
    }
}

bool ConfigFacade::has(const std::string& path) const
{
    return !source_of(path).empty();
}

std::string ConfigFacade::get(const std::string& path,
                              const std::string& default_value) const
{
    // 1) 环境变量（最高）
    if (!env_lookup_.empty()) {
        const auto e = env_lookup_.find(normalize_key(path));
        if (e != env_lookup_.end()) {
            return e->second.value;
        }
    }
    // 2) 文件层
    {
        const auto f = file_values_.find(path);
        if (f != file_values_.end()) {
            return f->second;
        }
    }
    // 3) 默认层
    {
        const auto d = defaults_.find(path);
        if (d != defaults_.end()) {
            return d->second;
        }
    }
    return default_value;
}

int ConfigFacade::get_int(const std::string& path, int default_value) const
{
    return lexical_cast_or(get(path), default_value);
}

std::int64_t ConfigFacade::get_int64(const std::string& path, std::int64_t default_value) const
{
    return lexical_cast_or(get(path), default_value);
}

double ConfigFacade::get_double(const std::string& path, double default_value) const
{
    return lexical_cast_or(get(path), default_value);
}

bool ConfigFacade::get_bool(const std::string& path, bool default_value) const
{
    // 与 IniConfig::get_bool 同口径：1/true/yes/on 与 0/false/no/off（大小写不敏感）
    const std::string v = get(path);
    if (v.empty()) {
        return default_value;
    }
    std::string low;
    low.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        low.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(v[i]))));
    }
    if (low == "1" || low == "true" || low == "yes" || low == "on") {
        return true;
    }
    if (low == "0" || low == "false" || low == "no" || low == "off") {
        return false;
    }
    return default_value;
}

std::string ConfigFacade::source_of(const std::string& path) const
{
    if (!env_lookup_.empty()) {
        const auto e = env_lookup_.find(normalize_key(path));
        if (e != env_lookup_.end()) {
            return "env";
        }
    }
    if (file_values_.find(path) != file_values_.end()) {
        return "file";
    }
    if (defaults_.find(path) != defaults_.end()) {
        return "default";
    }
    return std::string();
}

std::vector<std::string> ConfigFacade::keys() const
{
    // 三层并集（file ∪ default；env 的原始变量名与路径可能差异大，不并入）
    std::map<std::string, char> seen;
    for (auto it = file_values_.begin(); it != file_values_.end(); ++it) {
        seen[it->first] = 'f';
    }
    for (auto it = defaults_.begin(); it != defaults_.end(); ++it) {
        seen[it->first] = 'f';  // 占位即可，键集合才是目的
    }
    std::vector<std::string> out;
    out.reserve(seen.size());
    for (auto it = seen.begin(); it != seen.end(); ++it) {
        out.push_back(it->first);
    }
    return out;
}

void ConfigFacade::clear()
{
    defaults_.clear();
    file_values_.clear();
    // env 快照保留（与实例生命周期一致）
}

}  // namespace libmini
