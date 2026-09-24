#ifndef LIBMINI_CONFIG_FACADE_H
#define LIBMINI_CONFIG_FACADE_H

#include <map>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// 分层配置门面：默认值 → 配置文件（JSON/INI，按扩展名识别）→ 环境变量。
// 高层覆盖低层；文件格式只看扩展名（.json/.ini），不猜内容。
//
//   ConfigFacade cfg;
//   cfg.set_default("server.port", 8080);                    // 默认层
//   cfg.load_file("config.json");                            // 文件层
//   cfg.set_env_prefix("MYAPP_");                            // MYAPP_server.port
//   const int port = cfg.get_int("server.port");             // 依次查三层
//
// 环境变量映射规则：env 前缀 + 路径，'.' 与 '_' 归一化后匹配
// （"server.port" ←→ MYAPP_SERVER_PORT / myapp_serverport 均可）。
// 匹配在加载文件时完成快照，之后修改环境变量不再影响本次实例。
//
// 线程模型：load/set_default 后再并发读是安全的（读操作共享只读数据）；
// 并发 load 与 get 混用需外部同步。
class LIBMINI_API ConfigFacade {
public:
    ConfigFacade();

    // ---------------- 默认值层 ----------------

    // 设置默认值（字符串形式存储；取值时按目标类型转换）
    void set_default(const std::string& path, const std::string& value);

    // ---------------- 文件层 ----------------

    // 从文件加载并合并（同路径覆盖）。按扩展名识别格式：
    //   .json → JSON 对象；支持嵌套，键路径以 '.' 连接
    //           （{"server":{"port":8080}} → "server.port"）
    //   .ini  → IniConfig；键路径为 "section.key"（全局区为 ".key"）
    //   其他扩展名 → 返回 false（不支持）
    // 文件不存在返回 false（不抛异常）。
    bool load_file(const std::string& path);

    // 直接解析文本（显式指定格式："json" / "ini"）
    void load_text(const std::string& text, const std::string& format);

    // 当前文件层的键路径列表（含 default 与 env 不提供的）
    std::vector<std::string> file_keys() const;

    // ---------------- 环境变量层 ----------------

    // 设置环境变量前缀（如 "MYAPP_"）；空串表示匹配所有环境变量。
    // 修改前缀不会自动重扫环境，需调用 refresh_env()。
    void set_env_prefix(const std::string& prefix);

    // 重新扫描环境变量快照（set_env_prefix 或运行中改环境后调用）
    void refresh_env();

    // ---------------- 取值（三层合并） ----------------

    // 是否有任何一层提供该键
    bool has(const std::string& path) const;

    // 原始字符串值（未提供返回 default_value）
    std::string get(const std::string& path,
                    const std::string& default_value = std::string()) const;

    // 类型化读取；值不可转换时返回 default_value（不抛异常）
    int get_int(const std::string& path, int default_value = 0) const;
    std::int64_t get_int64(const std::string& path,
                           std::int64_t default_value = 0) const;
    double get_double(const std::string& path, double default_value = 0.0) const;
    bool get_bool(const std::string& path, bool default_value = false) const;

    // 三层来源（"default"/"file"/"env"）；未提供返回空串
    std::string source_of(const std::string& path) const;

    // 全部键路径（三层并集，字典序；诊断/导出用）
    std::vector<std::string> keys() const;

    void clear();

private:
    // 环境变量名 → 值 的归一化查找表（refresh_env 时构建）
    struct EnvEntry {
        std::string raw_name;
        std::string value;
    };
    // norm → entries（同名可能对应多个原始变量，取字典序最小者，行为确定）
    std::map<std::string, EnvEntry> env_lookup_;

    std::string env_prefix_;
    std::map<std::string, std::string> defaults_;
    std::map<std::string, std::string> file_values_;
};

}  // namespace libmini

#endif  // LIBMINI_CONFIG_FACADE_H
