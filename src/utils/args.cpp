#include "args.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "lexical_cast.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace libmini {

// ============================== 定义选项 ==================================

void Args::add_option(const std::string& long_name, const std::string& short_name,
                      const std::string& help_text,
                      const std::string& default_value)
{
    OptionDef def;
    def.long_name = long_name;
    def.short_name = short_name;
    def.help = help_text;
    def.takes_value = true;
    def.default_value = default_value;
    def.is_int = false;
    def.is_double = false;
    option_defs_.push_back(def);
    if (!default_value.empty()) {
        values_[long_name] = default_value;
    }
}

void Args::add_int(const std::string& long_name, const std::string& short_name,
                   const std::string& help_text, int default_value)
{
    OptionDef def;
    def.long_name = long_name;
    def.short_name = short_name;
    def.help = help_text;
    def.takes_value = true;
    def.default_value = lexical_cast<std::string>(default_value);
    def.is_int = true;
    def.is_double = false;
    option_defs_.push_back(def);
    values_[long_name] = def.default_value;
}

void Args::add_double(const std::string& long_name, const std::string& short_name,
                      const std::string& help_text, double default_value)
{
    OptionDef def;
    def.long_name = long_name;
    def.short_name = short_name;
    def.help = help_text;
    def.takes_value = true;
    def.default_value = lexical_cast<std::string>(default_value);
    def.is_int = false;
    def.is_double = true;
    option_defs_.push_back(def);
    values_[long_name] = def.default_value;
}

void Args::add_flag(const std::string& long_name, const std::string& short_name,
                    const std::string& help_text)
{
    OptionDef def;
    def.long_name = long_name;
    def.short_name = short_name;
    def.help = help_text;
    def.takes_value = false;
    def.is_int = false;
    def.is_double = false;
    option_defs_.push_back(def);
    flags_[long_name] = false;
}

void Args::add_positional(const std::string& name, const std::string& help_text)
{
    positional_defs_.push_back(std::make_pair(name, help_text));
}

// ================================ 解析 ====================================

const Args::OptionDef* Args::find_long(const std::string& name) const
{
    for (std::size_t i = 0; i < option_defs_.size(); ++i) {
        if (option_defs_[i].long_name == name) {
            return &option_defs_[i];
        }
    }
    return nullptr;
}

Args::OptionDef* Args::find_long(const std::string& name)
{
    for (std::size_t i = 0; i < option_defs_.size(); ++i) {
        if (option_defs_[i].long_name == name) {
            return &option_defs_[i];
        }
    }
    return nullptr;
}

const Args::OptionDef* Args::find_short(const std::string& name) const
{
    if (name.empty()) {
        return nullptr;
    }
    for (std::size_t i = 0; i < option_defs_.size(); ++i) {
        if (option_defs_[i].short_name == name) {
            return &option_defs_[i];
        }
    }
    return nullptr;
}

Args::OptionDef* Args::find_short(const std::string& name)
{
    if (name.empty()) {
        return nullptr;
    }
    for (std::size_t i = 0; i < option_defs_.size(); ++i) {
        if (option_defs_[i].short_name == name) {
            return &option_defs_[i];
        }
    }
    return nullptr;
}

bool Args::parse_int_strict(const std::string& text, int* out)
{
    try {
        // lexical_cast 为严格模式：拒绝 "12abc"、" 12"、"" 等
        const int value = lexical_cast<int>(text);
        if (out != nullptr) {
            *out = value;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Args::parse_double_strict(const std::string& text, double* out)
{
    try {
        const double value = lexical_cast<double>(text);
        if (out != nullptr) {
            *out = value;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Args::parse(int argc, char** argv)
{
    std::vector<std::string> list;
    list.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        list.push_back(argv[i] != nullptr ? argv[i] : "");
    }
    // argv[0] 是程序名，跳过
    return parse(std::vector<std::string>(list.begin() + (argc > 0 ? 1 : 0),
                                          list.end()));
}

bool Args::parse(const std::vector<std::string>& arguments)
{
    // 每次解析前重置运行期状态（默认值保留，位置/剩余清空）
    flags_.clear();
    for (std::size_t i = 0; i < option_defs_.size(); ++i) {
        if (option_defs_[i].takes_value) {
            if (option_defs_[i].default_value.empty()) {
                values_.erase(option_defs_[i].long_name);
            } else {
                values_[option_defs_[i].long_name] = option_defs_[i].default_value;
            }
        } else {
            flags_[option_defs_[i].long_name] = false;
        }
    }
    positionals_.clear();
    remaining_.clear();
    help_requested_ = false;

    bool options_ended = false;
    std::size_t pos_index = 0;

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& arg = arguments[i];

        if (options_ended) {
            remaining_.push_back(arg);
            continue;
        }

        // -- 终止符
        if (arg == "--") {
            options_ended = true;
            continue;
        }

        // help
        if (arg == "-h" || arg == "--help") {
            help_requested_ = true;
            print_usage();
            return false;
        }

        // 长选项 --name[=value]
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::string name = arg.substr(2);
            std::string inline_value;
            bool has_inline = false;
            const std::size_t eq = name.find('=');
            if (eq != std::string::npos) {
                inline_value = name.substr(eq + 1);
                name = name.substr(0, eq);
                has_inline = true;
            }

            const OptionDef* def = find_long(name);
            if (def == nullptr) {
                std::fprintf(stderr, "error: unknown option --%s\n", name.c_str());
                print_usage();
                return false;
            }

            if (!def->takes_value) {
                if (has_inline) {
                    std::fprintf(stderr, "error: option --%s does not take a value\n",
                                 name.c_str());
                    return false;
                }
                flags_[def->long_name] = true;
                continue;
            }

            std::string value;
            if (has_inline) {
                value = inline_value;
            } else if (i + 1 < arguments.size()) {
                value = arguments[++i];
            } else {
                std::fprintf(stderr, "error: option --%s requires a value\n",
                             name.c_str());
                return false;
            }
            if (def->is_int && !parse_int_strict(value, nullptr)) {
                std::fprintf(stderr, "error: option --%s expects an integer, got '%s'\n",
                             name.c_str(), value.c_str());
                return false;
            }
            if (def->is_double && !parse_double_strict(value, nullptr)) {
                std::fprintf(stderr, "error: option --%s expects a number, got '%s'\n",
                             name.c_str(), value.c_str());
                return false;
            }
            values_[def->long_name] = value;
            continue;
        }

        // 短选项 -k 或 -kvalue/-k value（这里支持 -k value 与 -k=value）
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            std::string name = arg.substr(1, 1);
            const OptionDef* def = find_short(name);
            if (def == nullptr) {
                std::fprintf(stderr, "error: unknown option -%s\n", name.c_str());
                print_usage();
                return false;
            }

            if (!def->takes_value) {
                if (arg.size() > 2 && arg[2] == '=') {
                    std::fprintf(stderr, "error: option -%s does not take a value\n",
                                 name.c_str());
                    return false;
                }
                flags_[def->long_name] = true;
                continue;
            }

            std::string value;
            if (arg.size() > 2) {
                if (arg[2] == '=') {
                    value = arg.substr(3);
                } else {
                    value = arg.substr(2);
                }
            } else if (i + 1 < arguments.size()) {
                value = arguments[++i];
            } else {
                std::fprintf(stderr, "error: option -%s requires a value\n",
                             name.c_str());
                return false;
            }
            if (def->is_int && !parse_int_strict(value, nullptr)) {
                std::fprintf(stderr, "error: option -%s expects an integer, got '%s'\n",
                             name.c_str(), value.c_str());
                return false;
            }
            if (def->is_double && !parse_double_strict(value, nullptr)) {
                std::fprintf(stderr, "error: option -%s expects a number, got '%s'\n",
                             name.c_str(), value.c_str());
                return false;
            }
            values_[def->long_name] = value;
            continue;
        }

        // 位置参数 / 剩余参数
        if (pos_index < positional_defs_.size()) {
            positionals_.push_back(arg);
            ++pos_index;
        } else {
            remaining_.push_back(arg);
        }
    }

    // 必填位置参数校验
    if (positionals_.size() < positional_defs_.size()) {
        const std::string& missing = positional_defs_[positionals_.size()].first;
        std::fprintf(stderr, "error: missing required argument <%s>\n", missing.c_str());
        print_usage();
        return false;
    }

    return true;
}

// ================================ 取值 ====================================

std::string Args::get_string(const std::string& name) const
{
    std::map<std::string, std::string>::const_iterator it = values_.find(name);
    return it == values_.end() ? std::string() : it->second;
}

int Args::get_int(const std::string& name) const
{
    const std::string v = get_string(name);
    int out = 0;
    return parse_int_strict(v, &out) ? out : 0;
}

double Args::get_double(const std::string& name) const
{
    const std::string v = get_string(name);
    double out = 0.0;
    return parse_double_strict(v, &out) ? out : 0.0;
}

bool Args::has_flag(const std::string& name) const
{
    std::map<std::string, bool>::const_iterator it = flags_.find(name);
    return it != flags_.end() && it->second;
}

std::vector<std::string> Args::remaining() const
{
    return remaining_;
}

std::string Args::positional(const std::string& name) const
{
    for (std::size_t i = 0; i < positional_defs_.size(); ++i) {
        if (positional_defs_[i].first == name && i < positionals_.size()) {
            return positionals_[i];
        }
    }
    return std::string();
}

// ================================ usage ===================================

void Args::print_usage() const
{
    std::fprintf(stderr, "usage: %s", program_.c_str());
    for (std::size_t i = 0; i < positional_defs_.size(); ++i) {
        std::fprintf(stderr, " <%s>", positional_defs_[i].first.c_str());
    }
    if (!option_defs_.empty()) {
        std::fprintf(stderr, " [options]");
    }
    std::fprintf(stderr, "\n");

    if (!description_.empty()) {
        std::fprintf(stderr, "%s", description_.c_str());
        if (!version_.empty()) {
            std::fprintf(stderr, " (version %s)", version_.c_str());
        }
        std::fprintf(stderr, "\n");
    }

    if (!option_defs_.empty()) {
        std::fprintf(stderr, "\noptions:\n");
        std::fprintf(stderr, "  -h, --help              show this help\n");
        for (std::size_t i = 0; i < option_defs_.size(); ++i) {
            const OptionDef& d = option_defs_[i];
            std::fprintf(stderr, "  ");
            if (!d.short_name.empty()) {
                std::fprintf(stderr, "-%s, ", d.short_name.c_str());
            } else {
                std::fprintf(stderr, "    ");
            }
            std::fprintf(stderr, "--%s", d.long_name.c_str());
            if (d.takes_value) {
                std::fprintf(stderr, " <%s>", d.long_name.c_str());
            }
            std::fprintf(stderr, "  %s", d.help.c_str());
            if (d.takes_value && !d.default_value.empty()) {
                std::fprintf(stderr, " (default: %s)", d.default_value.c_str());
            }
            std::fprintf(stderr, "\n");
        }
    }
}

// ============================== wmain 转换 =================================
// Windows 专属：wmain 宽字符入口的参数转 UTF-8（中文参数不乱码）。
// POSIX 入口天然是 UTF-8 字节串，无此需求
#ifdef _WIN32

std::vector<std::string> args_from_wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        const wchar_t* w = (argv != nullptr && argv[i] != nullptr) ? argv[i] : L"";
        const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
                                             nullptr, nullptr);
        if (need <= 1) {
            out.push_back(std::string());
            continue;
        }
        std::string utf8(static_cast<std::size_t>(need - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &utf8[0], need, nullptr, nullptr);
        out.push_back(utf8);
    }
    return out;
}

#endif  // _WIN32

}  // namespace libmini
