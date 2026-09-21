#ifndef LIBMINI_INI_CONFIG_H
#define LIBMINI_INI_CONFIG_H

#include <map>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// INI 配置文件读写。特性：
//   - [section] 分区，区外键属于全局区（空 section 名）
//   - 键值分隔符支持 '=' 和 ':'
//   - 行注释 ';' 与 '#'（整行或行内，行内注释前需有空白）
//   - 值可带单/双引号（读取时剥掉；保存时原样写出）
//   - 区名与键名大小写敏感
//   - set 后 save 可往返（重新排序为字典序，但内容不丢）
class LIBMINI_API IniConfig {
public:
    // 从文件加载；文件不存在返回 false
    bool load(const std::string& path);

    // 从内存文本解析（覆盖当前内容）
    void parse(const std::string& text);

    // 序列化为 INI 文本（\r\n 行尾）
    std::string save() const;
    bool save_to(const std::string& path) const;

    // 读取（不存在时返回 default_value）
    std::string get(const std::string& section, const std::string& key,
                    const std::string& default_value = std::string()) const;
    int get_int(const std::string& section, const std::string& key,
                int default_value = 0) const;
    double get_double(const std::string& section, const std::string& key,
                      double default_value = 0.0) const;
    // 布尔值接受 1/true/yes/on 与 0/false/no/off（大小写不敏感）
    bool get_bool(const std::string& section, const std::string& key,
                  bool default_value = false) const;

    // 写入（section/key 不存在时自动创建）
    void set(const std::string& section, const std::string& key,
             const std::string& value);

    // 删除单个键 / 整个区
    void remove(const std::string& section, const std::string& key);
    void remove_section(const std::string& section);

    bool has_section(const std::string& section) const;
    bool has_key(const std::string& section, const std::string& key) const;

    // 区名列表 / 某区的键名列表（均按字典序）
    std::vector<std::string> sections() const;
    std::vector<std::string> keys(const std::string& section) const;

    void clear();

private:
    // "" 为全局区
    std::map<std::string, std::map<std::string, std::string> > data_;
};

}  // namespace libmini

#endif  // LIBMINI_INI_CONFIG_H
