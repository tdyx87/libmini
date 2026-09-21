#ifndef LIBMINI_ARGS_H
#define LIBMINI_ARGS_H

#include <map>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// 命令行参数解析。先定义后解析的 builder 风格：
//
//   int main(int argc, char** argv)
//   {
//       libmini::Args args("demo", "1.0", "示例程序");
//       args.add_option("host", "h", "服务器地址", true,  std::string("127.0.0.1"));
//       args.add_option("port", "p", "端口", true, 8080);
//       args.add_flag("verbose", "v", "输出详细日志");
//       args.add_positional("input", "输入文件");
//       if (!args.parse(argc, argv)) return 1;   // 已打印错误或 help
//
//       std::string host = args.get_string("host");
//       int port = args.get_int("port");
//       bool verbose = args.has_flag("verbose");
//   }
//
// 支持形式：
//   --key=value / --key value / -k value / --flag / -f
//   --            终止符，其后全部作为位置参数（不解析选项）
//   -h / --help   内置，打印 usage 并返回 false（parse 结束，不算错误）
class LIBMINI_API Args {
public:
    Args() : help_requested_(false) {}
    Args(const std::string& program, const std::string& version,
         const std::string& description)
        : program_(program), version_(version), description_(description),
          help_requested_(false)
    {
    }

    // ------------------------- 定义选项 -------------------------
    // 长名不得为空；短名可为空。带值选项的 default_value 用字符串形式给出。
    void add_option(const std::string& long_name, const std::string& short_name,
                    const std::string& help_text,
                    const std::string& default_value = std::string());
    void add_int(const std::string& long_name, const std::string& short_name,
                 const std::string& help_text, int default_value);
    void add_double(const std::string& long_name, const std::string& short_name,
                    const std::string& help_text, double default_value);
    // 开关：出现即 true，不出现即 false，不取值
    void add_flag(const std::string& long_name, const std::string& short_name,
                  const std::string& help_text);
    // 位置参数（按定义顺序填充；余下的归入剩余参数）
    void add_positional(const std::string& name, const std::string& help_text);

    // ------------------------- 解析 -------------------------
    // 成功返回 true；解析失败（未知选项/缺值/数值非法/缺位置参数）打印错误到
    // stderr 并返回 false；请求 help 打印 usage 并返回 false（不算错误，
    // 用 help_requested() 区分）。
    bool parse(int argc, char** argv);
    bool parse(const std::vector<std::string>& arguments);

    // ------------------------- 取值 -------------------------
    std::string get_string(const std::string& name) const;
    int get_int(const std::string& name) const;
    double get_double(const std::string& name) const;
    bool has_flag(const std::string& name) const;

    // 除选项与已命名位置参数外剩余的参数（如待处理文件列表）
    std::vector<std::string> remaining() const;
    // 命名位置参数的值（未提供返回空串）
    std::string positional(const std::string& name) const;

    bool help_requested() const { return help_requested_; }

    // 打印 usage 到 stderr（parse 失败/help 时自动调用）
    void print_usage() const;

private:
    struct OptionDef {
        std::string long_name;
        std::string short_name;
        std::string help;
        bool takes_value;
        std::string default_value;
        bool is_int;
        bool is_double;
    };

    const OptionDef* find_long(const std::string& name) const;
    const OptionDef* find_short(const std::string& name) const;
    OptionDef* find_long(const std::string& name);
    OptionDef* find_short(const std::string& name);
    static bool parse_int_strict(const std::string& text, int* out);
    static bool parse_double_strict(const std::string& text, double* out);

    std::vector<OptionDef> option_defs_;
    std::vector<std::pair<std::string, std::string> > positional_defs_;  // name, help
    std::map<std::string, std::string> values_;      // 长名 → 值
    std::map<std::string, bool> flags_;              // 长名 → 是否出现
    std::vector<std::string> positionals_;
    std::vector<std::string> remaining_;
    std::string program_;
    std::string version_;
    std::string description_;
    bool help_requested_;
};

// Windows 宽字符入口转换：wmain 的 argv（UTF-16）转 UTF-8，中文参数不乱码。
//   int wmain(int argc, wchar_t** argv)
//   {
//       std::vector<std::string> a = libmini::args_from_wmain(argc, argv);
//       libmini::Args args;
//       args.parse(a);    // 或 args.parse(static_cast<int>(a.size()), ...
//   }
std::vector<std::string> LIBMINI_API args_from_wmain(int argc, wchar_t** argv);

}  // namespace libmini

#endif  // LIBMINI_ARGS_H
