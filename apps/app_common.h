#ifndef LIBMINI_APPS_APP_COMMON_H
#define LIBMINI_APPS_APP_COMMON_H

// 示例应用共用的极小工具层。刻意保持很薄：应用本身该直接用 libmini 的模块，
// 这里只放「六个应用都会重复写的几行」——工作目录、JSON 响应、demo 自检统计。

#include <cstdint>
#include <string>

#include "libmini.h"

namespace app {

using libmini::JsonValue;

// ---------------- 工作目录 ----------------

// 在系统临时目录下创建本次运行专属的目录（`<app>_<随机>`）。
// 传 dir 非空时用 dir 作为父目录。失败返回空串。
std::string make_workspace(const std::string& app_name,
                           const std::string& dir = std::string());

// ---------------- 自身位置 ----------------

// 当前可执行文件所在目录（优先用系统接口拿真实路径，拿不到再退回 argv[0]）
std::string module_dir(const std::string& argv0);

// 当前可执行文件的完整路径（名字取自 argv[0] 的文件名部分，目录用 module_dir）
std::string self_exe(const std::string& argv0);

// 拼出同目录下另一个可执行文件（自动补平台后缀）
std::string sibling_exe(const std::string& argv0, const std::string& name);

// 目录下的文件路径（path_join 的便捷形式）
std::string workspace_file(const std::string& dir, const std::string& name);

// ---------------- 时间 / 数值 ----------------

// 当前 Unix 毫秒时间戳
std::int64_t now_ms();

// 毫秒 → "1.23s" / "12ms"（对齐库内 format_duration_ms 的用法，便于统一输出）
std::string ms_text(std::int64_t ms);

// ---------------- 解析辅助 ----------------

// 严格整数解析（"42" 成功，"4x"/"" 失败）
bool to_int(const std::string& text, int* value);

// 时长解析：支持 "500ms" / "30s" / "5m" / "2h" / "1d"，无单位按秒
bool parse_duration_ms(const std::string& text, int* out_ms);

// JSON 取值（缺失或类型不符时返回默认值）
std::string jstr(const JsonValue& node, const char* key,
                 const std::string& fallback = std::string());
std::int64_t jint(const JsonValue& node, const char* key, std::int64_t fallback = 0);
bool jbool(const JsonValue& node, const char* key, bool fallback = false);

// ---------------- HTTP 响应 ----------------

// 统一的 JSON 响应构造
libmini::HttpReply json_ok(const JsonValue& body, int status = 200);
libmini::HttpReply json_ok();
libmini::HttpReply json_error(int status, const std::string& message);

// ---------------- demo 自检 ----------------

// 应用自带的 --demo 模式用它累积「检查项 → 通过/失败」，最后打印小结并返回 0/1。
// demo 一律绑 0 端口、用临时目录，因此可在 CI 上重复/并行执行。
class DemoReport
{
public:
    explicit DemoReport(const std::string& title);

    // 记录一条检查结果（ok=false 也继续跑，保证一次看完所有问题）
    void check(bool ok, const std::string& what);
    // 只打印信息，不计入通过/失败
    void info(const std::string& text) const;

    int passed() const { return passed_; }
    int failed() const { return failed_; }

    // 打印小结并返回进程退出码（全通过 0，否则 1）
    int finish() const;

private:
    std::string title_;
    int passed_;
    int failed_;
};

// 打印带分隔线的标题（应用启动时的一致性输出）
void print_banner(const std::string& title, const std::string& detail);

}  // namespace app

#endif  // LIBMINI_APPS_APP_COMMON_H
