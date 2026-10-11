#ifndef LIBMINI_UI_PANELS_H
#define LIBMINI_UI_PANELS_H

// 成品窗口层（库模块）：把「一个应用的原生窗口」抽象成两种形状，直接可用。
//
//   服务面板（port > 0）
//     连到某个已经在跑的 HTTP 控制面，按 spec 声明的内容轮询并渲染：
//       - /healthz /readyz 健康徽章
//       - 若干 JSON 端点（原样展示）
//       - /metrics 里按子串过滤的指标行
//       - 若干 JSON 数值路径（画成折线，保留历史）
//       - 若干动作按钮（GET/POST），结果记进动作日志
//
//   运行器（port == 0）
//     拉起一个子进程（一次性任务），实时展示它的输出与退出码，
//     并提供「重跑 / 停止 / 清空」。
//
// 两者都由 run_app_window() 统一驱动：先按需拉起子进程，再开窗口，窗口关闭后
// 回收子进程。调用方只需要描述 spec，不需要碰窗口细节：
//
//   libmini::ui::AppWindowSpec spec;
//   spec.title = "jobsvc 面板";
//   spec.child_args = {"jobsvc.exe", "--serve"};
//   spec.port = 8080;
//   spec.panel.json_views = {"/api/state"};
//   libmini::ui::run_app_window(spec, &error);
//
// 面板里凡是要长期存活的服务进程都用 libmini::ChildProcess 托管。
//
// 非 Windows：没有窗口后端时 run_app_window() 返回 2 并给出原因（自检档位下
// 打印「跳过」并返回 0），调用方可据此退回控制台模式。

#include <memory>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {
namespace ui {

// 面板上的一个动作按钮
struct PanelAction
{
    std::string label;   // 按钮文字
    std::string method;  // "GET" / "POST"
    std::string path;    // 如 "/api/cleanup"
    std::string body;    // POST 体（可为空）
};

// 服务面板的内容声明
struct PanelSpec
{
    std::string title;
    std::string subtitle;
    std::string base_url;                    // http://127.0.0.1:PORT
    std::vector<std::string> json_views;     // 要展示的 JSON 端点
    std::vector<std::string> metric_filters; // /metrics 里保留的行（子串匹配，空 = 全部）
    std::vector<std::string> series_paths;   // 折线的 JSON 数值路径（点分，如 latest.cpu_percent）
    std::vector<PanelAction> actions;
    int poll_ms = 1000;                      // 轮询间隔
};

// 一个应用窗口的完整描述
struct AppWindowSpec
{
    std::string title;
    std::string subtitle;
    // 需要先拉起的子进程（第一个元素是可执行文件，其后是参数）；空 = 不起子进程
    std::vector<std::string> child_args;
    std::string work_dir;
    int port = 0;          // > 0 → 服务面板（连本机该端口）；0 → 运行器视图
    PanelSpec panel;       // port > 0 时使用

    // ---- 自检档位（CI 用；都为 0/空时是普通交互模式） ----
    int frames = 0;            // > 0：跑满这么多帧自动关窗（无头/自动化验证）
    std::string shot_path;     // 非空：关窗前把完整一帧存成 BMP
    bool report = false;       // true：把自检结论打印到 stdout
};

// 阻塞式运行应用窗口；返回 0 正常关闭、2 无窗口后端（原因写入 error）
LIBMINI_API int run_app_window(const AppWindowSpec& spec, std::string* error);

// 后台面板窗口：需要一个进程同时开多个应用窗口时用它（每个窗口一个线程）。
// open() 会立刻返回（无窗口后端时返回空指针并填 error）。
class LIBMINI_API PanelWindow
{
public:
    ~PanelWindow();

    static std::shared_ptr<PanelWindow> open(const PanelSpec& spec, std::string* error);

    // 请求窗口关闭（下一帧生效）
    void request_close();
    // 等窗口线程结束
    void join();
    // 窗口是否还开着
    bool running() const;

private:
    PanelWindow();

    struct Impl;
    Impl* impl_;
};

}  // namespace ui
}  // namespace libmini

#endif  // LIBMINI_UI_PANELS_H
