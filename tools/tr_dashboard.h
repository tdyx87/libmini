#ifndef LIBMINI_TOOLS_TR_DASHBOARD_H
#define LIBMINI_TOOLS_TR_DASHBOARD_H

#include <string>

// 浏览器仪表板：复用库本体的 HttpServer，把 RunState 的进度/结果渲染成网页，
// 并把「运行 / 重跑失败 / 停止」暴露成 JSON 接口。终端模式与网页模式共用同一
// 个 RunState，所以两边看到的是同一份状态。

namespace tr {

class RunState;

// 启动本地 HTTP 服务并注册路由。port=0 时由系统分配端口。
// 成功返回可访问的 URL（如 "http://127.0.0.1:8123/"）；失败返回空串并写 error。
std::string start_dashboard(RunState* state, int port, std::string* error);

// 停止并回收上一次启动的服务（幂等；未启动时无害）
void stop_dashboard();

// 用系统默认程序打开 URL（尽力而为，失败返回 false）
bool open_in_browser(const std::string& url);

}  // namespace tr

#endif  // LIBMINI_TOOLS_TR_DASHBOARD_H
