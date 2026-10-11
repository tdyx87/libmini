// launcher —— libmini 桌面控制台（统一启动器，原生窗口）
//
// 一个窗口把六个体量不小的示例应用与测试聚合器管起来：启动/停止/自检/开窗口，
// 并显示每个被托管服务的健康状态、指标摘要与实时输出。它同时是「原生 UI 层」的
// 样板消费者：界面每帧重新描述（立即模式），进程托管走 libmini::ChildProcess（非阻塞）。
//
//   托管内容（可执行文件都在同一个 bin 目录里）
//     jobsvc       服务  :8780   --db <工作目录>/jobsvc.db --port 8780
//     sysmon       服务  :8790   --db <工作目录>/sysmon.db --port 8790 --interval 1s
//     lanshare     服务  :8800   --dir <工作目录>/lanshare --port 8800
//     svcframe     服务  :8832   --port 8831 --http-port 8832 serve
//     apitest      一次性        --demo
//     passvault    一次性        --demo
//     test_runner  一次性        --json <工作目录>/report.json
//
//   用法
//     launcher                打开原生窗口（Windows）
//     launcher --demo         无头自检：托管/输出/退出码/终止都用自造子进程验证
//     launcher --ui-selftest  窗口自检：建窗、跑固定帧数、合成点击、截图存 BMP
//
// 非 Windows：窗口后端缺失时如实报告「无后端」，但托管逻辑（fork/exec）照样自检，
// 因此三平台 CI 跑的是同一条用例。

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>  // readlink：定位自身可执行文件
#endif

#include "app_common.h"
#include "libmini.h"
#include "utils/child_process.h"
#include "utils/native_ui.h"
#include "utils/ui_panels.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW（WIN32_LEAN_AND_MEAN 不含它，且必须排在 windows.h 之后）

#include "utils/win_utf.h"

#endif

using namespace libmini;

namespace {

using app::DemoReport;

// 前置声明：界面上把 /metrics 样本拆行展示（定义在文件后部）
std::vector<std::string> split_lines(const std::string& text);

// ---------------- 可执行文件位置 ----------------

std::string module_dir(const char* argv0)
{
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD got = GetModuleFileNameW(0, buffer, MAX_PATH);
    if (got > 0 && got < MAX_PATH) {
        std::string full = libmini::internal::wide_to_utf8(buffer, static_cast<std::size_t>(got));
        const std::size_t slash = full.find_last_of("\\/");
        if (slash != std::string::npos) {
            return full.substr(0, slash);
        }
    }
#else
    char buffer[4096];
    const ssize_t got = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (got > 0) {
        buffer[got] = '\0';
        std::string full(buffer);
        const std::size_t slash = full.find_last_of('/');
        if (slash != std::string::npos) {
            return full.substr(0, slash);
        }
    }
#endif
    // 退路：用 argv[0] 所在目录（相对路径则拼上当前目录）
    const std::string arg = argv0 != 0 ? std::string(argv0) : std::string();
    const std::size_t slash = arg.find_last_of("\\/");
    if (slash != std::string::npos) {
        return arg.substr(0, slash);
    }
    return std::string(".");  // 相对路径或找不到：按当前目录处理
}

// ---------------- 托管项 ----------------

struct AppSpec
{
    std::string name;
    std::string kind;                      // "服务" / "一次性"
    std::vector<std::string> serve_args;   // 常驻服务参数
    std::vector<std::string> run_args;     // 一次性任务参数
    int port = 0;                          // 控制面端口（0 = 无）
    std::string note;
};

struct Managed
{
    AppSpec spec;
    ChildProcess service;
    ChildProcess oneshot;
    int last_exit = ChildProcess::kNotExited;
    bool last_failed = false;
    std::string last_what;   // "自检" / "测试"

    // 服务健康采样（启动器自己发的 HTTP 请求）
    int http_status = 0;
    double rtt_ms = 0.0;
    std::size_t metric_lines = 0;
    std::string metric_sample;
    std::string ready_text;
    std::string probe_error;
    std::vector<double> rtt_history;
    std::int64_t last_poll_ms = 0;
};

// ---------------- 启动器 ----------------

class Launcher
{
public:
    Launcher(const std::string& exe_dir, const std::string& work_dir)
        : exe_dir_(exe_dir), work_dir_(work_dir), selected_(0), selftest_(false)
    {
        build_catalog();
    }

    void enable_ui_selftest() { selftest_ = true; }
    void set_capture_path(const std::string& path) { capture_path_ = path; }
    bool synthetic_click_seen() const { return synthetic_click_seen_; }
    // 截图请求是否已登记（真正写文件由 UI 层在帧结束后完成，结果用 Ui::capture_ok() 取）
    bool capture_requested() const { return capture_requested_; }
    const std::string& capture_path() const { return capture_path_; }

    const std::vector<Managed>& apps() const { return apps_; }
    void set_toast(const std::string& text) { toast_ = text; toast_ms_ = app::now_ms(); }
    // 收尾：关闭由本进程开出的应用窗口并回收托管进程（主窗口关闭时由调用方执行）
    void close_app_windows();

    // 跑一个子进程并等它结束（自检与「自检」按钮共用）
    int run_and_wait(const std::string& program, const std::vector<std::string>& args,
                     std::string* output, int timeout_ms)
    {
        ChildProcess child;
        std::string error;
        if (!child.start(program, args, work_dir_, &error)) {
            if (output != 0) {
                *output = "启动失败: " + error;
            }
            return -1;
        }
        if (!child.wait(timeout_ms)) {
            child.kill();
            if (output != 0) {
                *output = child.all_output() + "\n[超时被终止]";
            }
            return -2;
        }
        if (output != 0) {
            *output = child.all_output();
        }
        return child.exit_code();
    }

    void frame(ui::Ui& u);

private:
    void build_catalog();
    std::string exe_path(const std::string& name) const;
    void poll_all();
    void poll(Managed& m);
    void start_selected(Managed& m);
    void stop_selected(Managed& m);
    void kill_children();
    std::string kind_badge_text(const Managed& m) const;
    void open_in_browser(const std::string& url);
    void open_app_window(Managed& m);
    static ui::PanelSpec panel_spec_for(const AppSpec& spec);

    std::vector<Managed> apps_;
    // 由启动器进程内开出来的应用面板窗口（每个一个线程）；关窗时统一回收
    std::vector<std::shared_ptr<ui::PanelWindow> > panels_;
    // 由应用自己开的窗口（<app> --ui 子进程）：句柄留着，否则局部析构会把它杀掉
    std::vector<std::shared_ptr<ChildProcess> > ui_children_;
    std::string exe_dir_;
    std::string work_dir_;
    std::size_t selected_;
    std::string toast_;
    std::int64_t toast_ms_ = 0;

    // 窗口自检状态
    bool selftest_;
    bool synthetic_click_seen_ = false;
    bool capture_requested_ = false;
    std::string capture_path_;
    ui::Rect refresh_button_rect_;
    int frames_seen_ = 0;
};

void Launcher::build_catalog()
{
    AppSpec jobsvc;
    jobsvc.name = "jobsvc";
    jobsvc.kind = "服务";
    jobsvc.port = 8780;
    jobsvc.serve_args.push_back("--db");
    jobsvc.serve_args.push_back(app::workspace_file(work_dir_, "jobsvc.db"));
    jobsvc.serve_args.push_back("--port");
    jobsvc.serve_args.push_back("8780");
    jobsvc.run_args.push_back("--demo");
    jobsvc.note = "任务调度：cron 计划 + 子进程/HTTP 执行器";
    apps_.push_back(Managed());
    apps_.back().spec = jobsvc;

    AppSpec sysmon;
    sysmon.name = "sysmon";
    sysmon.kind = "服务";
    sysmon.port = 8790;
    sysmon.serve_args.push_back("--db");
    sysmon.serve_args.push_back(app::workspace_file(work_dir_, "sysmon.db"));
    sysmon.serve_args.push_back("--port");
    sysmon.serve_args.push_back("8790");
    sysmon.serve_args.push_back("--interval");
    sysmon.serve_args.push_back("1s");
    sysmon.run_args.push_back("--demo");
    sysmon.note = "观测台：采集 + Prometheus + 看板 + 告警";
    apps_.push_back(Managed());
    apps_.back().spec = sysmon;

    AppSpec lanshare;
    lanshare.name = "lanshare";
    lanshare.kind = "服务";
    lanshare.port = 8800;
    lanshare.serve_args.push_back("--dir");
    lanshare.serve_args.push_back(app::workspace_file(work_dir_, "lanshare"));
    lanshare.serve_args.push_back("--port");
    lanshare.serve_args.push_back("8800");
    lanshare.run_args.push_back("--demo");
    lanshare.note = "文件中转：分块续传 + sha256 + 打包下载";
    apps_.push_back(Managed());
    apps_.back().spec = lanshare;

    AppSpec svcframe;
    svcframe.name = "svcframe";
    svcframe.kind = "服务";
    svcframe.port = 8832;
    svcframe.serve_args.push_back("--port");
    svcframe.serve_args.push_back("8831");
    svcframe.serve_args.push_back("--http-port");
    svcframe.serve_args.push_back("8832");
    svcframe.serve_args.push_back("serve");
    svcframe.run_args.push_back("--demo");
    svcframe.note = "RPC 骨架：分层配置 + 热加载 + 指标 + 追踪";
    apps_.push_back(Managed());
    apps_.back().spec = svcframe;

    AppSpec apitest;
    apitest.name = "apitest";
    apitest.kind = "一次性";
    apitest.run_args.push_back("--demo");
    apitest.note = "API 回归与压测：断言 + 分位 + 报告";
    apps_.push_back(Managed());
    apps_.back().spec = apitest;

    AppSpec passvault;
    passvault.name = "passvault";
    passvault.kind = "一次性";
    passvault.run_args.push_back("--demo");
    passvault.note = "加密保管库：PBKDF2 + AES-256-GCM";
    apps_.push_back(Managed());
    apps_.back().spec = passvault;

    AppSpec runner;
    runner.name = "test_runner";
    runner.kind = "一次性";
    runner.run_args.push_back("--json");
    runner.run_args.push_back(app::workspace_file(work_dir_, "report.json"));
    runner.note = "测试聚合器：跑全部 gtest 套件并出报告";
    apps_.push_back(Managed());
    apps_.back().spec = runner;
}

std::string Launcher::exe_path(const std::string& name) const
{
    std::string file = name;
#if defined(_WIN32)
    file += ".exe";
#endif
    return app::workspace_file(exe_dir_, file);
}

void Launcher::start_selected(Managed& m)
{
    const std::string exe = exe_path(m.spec.name);
    if (!file_exists(exe)) {
        set_toast(m.spec.name + ": 找不到可执行文件 " + exe);
        return;
    }
    if (m.spec.kind == "服务") {
        if (m.service.started() && m.service.running()) {
            set_toast(m.spec.name + ": 已在运行（PID " +
                      lexical_cast<std::string>(m.service.pid()) + "）");
            return;
        }
        m.service = ChildProcess();
        std::string error;
        if (!m.service.start(exe, m.spec.serve_args, work_dir_, &error)) {
            set_toast(m.spec.name + ": 启动失败 " + error);
            return;
        }
        set_toast(m.spec.name + ": 已启动（PID " +
                  lexical_cast<std::string>(m.service.pid()) + "）");
        return;
    }
    // 一次性任务：起了就不等（界面上看输出与退出码）
    if (m.oneshot.started() && m.oneshot.running()) {
        set_toast(m.spec.name + ": 上一轮还在跑");
        return;
    }
    m.oneshot = ChildProcess();
    std::string error;
    if (!m.oneshot.start(exe, m.spec.run_args, work_dir_, &error)) {
        set_toast(m.spec.name + ": 启动失败 " + error);
        return;
    }
    m.last_what = "运行";
    set_toast(m.spec.name + ": 已启动一次性任务（PID " +
              lexical_cast<std::string>(m.oneshot.pid()) + "）");
}

void Launcher::stop_selected(Managed& m)
{
    bool did = false;
    if (m.service.started() && m.service.running()) {
        m.service.kill();
        did = true;
    }
    if (m.oneshot.started() && m.oneshot.running()) {
        m.oneshot.kill();
        did = true;
    }
    set_toast(did ? (m.spec.name + ": 已停止") : (m.spec.name + ": 本来就没在跑"));
}

void Launcher::kill_children()
{
    for (std::size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i].service.started() && apps_[i].service.running()) {
            apps_[i].service.kill();
        }
        if (apps_[i].oneshot.started() && apps_[i].oneshot.running()) {
            apps_[i].oneshot.kill();
        }
    }
}

void Launcher::poll(Managed& m)
{
    m.last_poll_ms = app::now_ms();
    if (m.spec.port == 0 || !m.service.started() || !m.service.running()) {
        m.http_status = 0;
        m.rtt_ms = 0.0;
        m.metric_lines = 0;
        m.metric_sample.clear();
        m.ready_text.clear();
        m.probe_error.clear();
        return;
    }
    const std::string base = "http://127.0.0.1:" + lexical_cast<std::string>(m.spec.port);
    HttpClient http(base);

    const std::int64_t started = app::now_ms();
    Result<HttpResponse> health = http.try_get("/healthz");
    m.rtt_ms = static_cast<double>(app::now_ms() - started);
    if (!health.ok()) {
        m.http_status = 0;
        m.probe_error = health.status().to_string();
        return;
    }
    m.http_status = health.value().status;
    m.probe_error.clear();
    m.rtt_history.push_back(m.rtt_ms);
    if (m.rtt_history.size() > 60) {
        m.rtt_history.erase(m.rtt_history.begin());
    }

    Result<HttpResponse> ready = http.try_get("/readyz");
    if (ready.ok()) {
        m.ready_text = ready.value().status == 200 ? std::string("就绪")
                                                   : ("未就绪(" +
                                                      lexical_cast<std::string>(
                                                          ready.value().status) +
                                                      ")");
    } else {
        m.ready_text = "就绪探测失败";
    }

    Result<HttpResponse> metrics = http.try_get("/metrics");
    if (metrics.ok()) {
        const std::string& body = metrics.value().body;
        m.metric_lines = static_cast<std::size_t>(
            std::count(body.begin(), body.end(), '\n'));
        // 取前几行做样本（跳过注释行，界面更干净）
        m.metric_sample.clear();
        std::size_t begin = 0;
        int taken = 0;
        while (begin < body.size() && taken < 6) {
            const std::size_t end = body.find('\n', begin);
            const std::string line = body.substr(begin, end == std::string::npos
                                                            ? std::string::npos
                                                            : end - begin);
            if (!line.empty() && line[0] != '#') {
                m.metric_sample += line + "\n";
                ++taken;
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    } else {
        m.metric_lines = 0;
        m.metric_sample = "/metrics 不可用: " + metrics.status().to_string();
    }
}

void Launcher::poll_all()
{
    // 应用自己开的窗口子进程：退出了就把句柄收掉（避免越积越多）
    std::size_t keep = 0;
    for (std::size_t i = 0; i < ui_children_.size(); ++i) {
        const bool finished = ui_children_[i]->started() && ui_children_[i]->try_finish(0);
        if (!finished) {
            ui_children_[keep++] = ui_children_[i];
        }
    }
    ui_children_.resize(keep);

    for (std::size_t i = 0; i < apps_.size(); ++i) {
        Managed& m = apps_[i];
        // 一次性任务：收尾退出码
        int code = 0;
        if (m.oneshot.started() && m.oneshot.try_finish(&code)) {
            m.last_exit = code;
            m.last_failed = code != 0;
        }
        if (m.spec.kind == "服务") {
            if (app::now_ms() - m.last_poll_ms >= 1500) {
                poll(m);
            }
            int service_code = 0;
            if (m.service.started() && m.service.try_finish(&service_code)) {
                // 服务意外退出：记下退出码，面板会显示
                m.last_exit = service_code;
                m.last_failed = service_code != 0;
                m.http_status = 0;
            }
        }
    }
}

// 每个应用的面板声明：控制面端点、要突出的指标、可点的动作。
// 这里刻意只声明「看什么」，轮询与渲染交给 ui::PanelWindow。
ui::PanelSpec Launcher::panel_spec_for(const AppSpec& spec)
{
    ui::PanelSpec panel;
    panel.title = spec.name + " 面板";
    panel.subtitle = spec.note;
    panel.base_url = "http://127.0.0.1:" + lexical_cast<std::string>(spec.port);

    if (spec.name == "sysmon") {
        panel.json_views.push_back("/api/state");
        panel.json_views.push_back("/api/alerts");
        panel.metric_filters.push_back("sysmon_cpu_usage_percent");
        panel.metric_filters.push_back("sysmon_memory_used_bytes");
        panel.metric_filters.push_back("sysmon_disk_free_bytes");
        panel.series_paths.push_back("latest.cpu_percent");
        panel.series_paths.push_back("latest.memory_percent");
    } else if (spec.name == "jobsvc") {
        panel.json_views.push_back("/api/state");
        panel.json_views.push_back("/api/jobs");
        panel.json_views.push_back("/api/runs");
        panel.metric_filters.push_back("jobsvc");
    } else if (spec.name == "lanshare") {
        panel.json_views.push_back("/api/files");
        panel.metric_filters.push_back("lanshare");
        ui::PanelAction cleanup;
        cleanup.label = "清理过期文件";
        cleanup.method = "POST";
        cleanup.path = "/api/cleanup";
        cleanup.body = "{}";
        panel.actions.push_back(cleanup);
    } else if (spec.name == "svcframe") {
        panel.json_views.push_back("/api/config");
        panel.json_views.push_back("/api/traces");
        panel.metric_filters.push_back("svcframe_rpc_calls_total");
        panel.metric_filters.push_back("svcframe_rpc_errors_total");
        panel.metric_filters.push_back("svcframe_rpc_in_flight");

        ui::PanelAction reload;
        reload.label = "热加载配置";
        reload.method = "POST";
        reload.path = "/api/reload";
        reload.body = "{}";
        panel.actions.push_back(reload);

        ui::PanelAction ping;
        ping.label = "RPC ping";
        ping.method = "POST";
        ping.path = "/api/rpc";
        ping.body = "{\"method\":\"ping\",\"params\":\"null\"}";
        panel.actions.push_back(ping);

        ui::PanelAction add;
        add.label = "RPC add 2+3";
        add.method = "POST";
        add.path = "/api/rpc";
        add.body =
            "{\"method\":\"add\",\"params\":\"{\\\"a\\\":2,\\\"b\\\":3}\"}";
        panel.actions.push_back(add);
    }
    return panel;
}

void Launcher::open_app_window(Managed& m)
{
    if (m.spec.kind == "服务") {
        // 服务得先跑起来，面板才有东西可看
        if (!(m.service.started() && m.service.running())) {
            start_selected(m);
        }
        if (!(m.service.started() && m.service.running())) {
            set_toast(m.spec.name + ": 服务没起来，先看输出尾部");
            return;
        }
        std::string error;
        std::shared_ptr<ui::PanelWindow> panel =
            ui::PanelWindow::open(panel_spec_for(m.spec), &error);
        if (!panel) {
            set_toast(m.spec.name + ": 开面板失败 " + error);
            return;
        }
        panels_.push_back(panel);
        set_toast(m.spec.name + ": 已开面板窗口（当前 " +
                  lexical_cast<std::string>(panels_.size()) + " 个）");
        return;
    }

    // 一次性任务（含测试聚合器）：交给应用自己的 --ui 窗口，句柄留着以免被杀掉
    const std::string exe = exe_path(m.spec.name);
    std::shared_ptr<ChildProcess> child(new ChildProcess());
    std::string error;
    const std::vector<std::string> args(1, "--ui");
    if (!child->start(exe, args, work_dir_, &error)) {
        set_toast(m.spec.name + ": 开窗口失败 " + error);
        return;
    }
    ui_children_.push_back(child);
    set_toast(m.spec.name + ": 已请求应用开窗口（PID " +
              lexical_cast<std::string>(child->pid()) + "）");
}

void Launcher::close_app_windows()
{
    for (std::size_t i = 0; i < panels_.size(); ++i) {
        panels_[i]->request_close();
    }
    for (std::size_t i = 0; i < panels_.size(); ++i) {
        panels_[i]->join();  // 面板每帧检查关闭请求，很快返回
    }
    const std::size_t closed = panels_.size();
    panels_.clear();
    for (std::size_t i = 0; i < ui_children_.size(); ++i) {
        if (ui_children_[i]->started() && ui_children_[i]->running()) {
            ui_children_[i]->kill();
        }
    }
    ui_children_.clear();
    set_toast("已关闭应用窗口（面板 " + lexical_cast<std::string>(closed) + " 个）");
}

void Launcher::open_in_browser(const std::string& url)
{
#if defined(_WIN32)
    const std::wstring wide = libmini::internal::utf8_to_wide(url);
    const HINSTANCE result =
        ShellExecuteW(0, L"open", wide.c_str(), 0, 0, SW_SHOWNORMAL);
    set_toast(reinterpret_cast<std::intptr_t>(result) > 32 ? ("已打开 " + url)
                                                           : ("打开失败 " + url));
#else
    set_toast("无桌面后端，无法打开浏览器: " + url);
#endif
}

void Launcher::frame(ui::Ui& u)
{
    ++frames_seen_;
    if (u.every(800)) {
        poll_all();
    }

    u.title("libmini 桌面控制台", std::string("原生 UI 后端 ") + ui::Ui::backend() +
                                       " · 工作目录 " + work_dir_);

    // 顶部按钮行：最后一个按钮（刷新面板）留作窗口自检的合成点击目标
    // 最后一个按钮（刷新面板）留作窗口自检的合成点击目标
    const int top = u.button_row(std::vector<std::string>{"全部启动", "全部停止", "全部自检",
                                                          "关闭应用窗口", "刷新面板"});
    refresh_button_rect_ = u.last_rect();
    if (top == 0) {
        for (std::size_t i = 0; i < apps_.size(); ++i) {
            if (apps_[i].spec.kind == "服务") {
                start_selected(apps_[i]);
            }
        }
        set_toast("已启动全部服务");
    } else if (top == 1) {
        kill_children();
        set_toast("已停止全部托管进程");
    } else if (top == 2) {
        for (std::size_t i = 0; i < apps_.size(); ++i) {
            if (!(apps_[i].oneshot.started() && apps_[i].oneshot.running())) {
                start_selected(apps_[i]);
            }
        }
        set_toast("已开始全部自检");
    } else if (top == 3) {
        close_app_windows();
    } else if (top == 4) {
        poll_all();
        set_toast("面板已刷新");
        if (selftest_) {
            synthetic_click_seen_ = true;  // 合成点击确实落到了按钮上
        }
    }

    // 汇总
    int running_count = 0;
    int exited_count = 0;
    int failed_count = 0;
    for (std::size_t i = 0; i < apps_.size(); ++i) {
        Managed& m = apps_[i];
        if ((m.service.started() && m.service.running()) ||
            (m.oneshot.started() && m.oneshot.running())) {
            ++running_count;
        }
        if (m.last_exit != ChildProcess::kNotExited) {
            ++exited_count;
            if (m.last_failed) {
                ++failed_count;
            }
        }
    }
    u.gap(4);
    u.badge("运行中 " + lexical_cast<std::string>(running_count), ui::theme::ok);
    u.badge("已结束 " + lexical_cast<std::string>(exited_count), ui::theme::dim);
    u.badge(failed_count > 0 ? ("失败 " + lexical_cast<std::string>(failed_count))
                             : std::string("无失败"),
            failed_count > 0 ? ui::theme::err : ui::theme::ok);
    u.newline();

    // ---- 托管项表格 ----
    u.section("托管项（点一行选中）");
    std::vector<std::vector<std::string> > rows;
    for (std::size_t i = 0; i < apps_.size(); ++i) {
        const Managed& m = apps_[i];
        std::vector<std::string> row;
        row.push_back(m.spec.name);
        row.push_back(m.spec.kind);
        row.push_back(m.spec.port > 0 ? (":" + lexical_cast<std::string>(m.spec.port))
                                      : std::string("-"));
        std::string state = "空闲";
        if (m.service.started() && m.service.running()) {
            state = "服务运行中";
        } else if (m.oneshot.started() && m.oneshot.running()) {
            state = "任务运行中";
        } else if (m.last_exit != ChildProcess::kNotExited) {
            state = m.last_failed ? ("已结束(失败 " + lexical_cast<std::string>(m.last_exit) + ")")
                                  : ("已结束(0)");
        }
        row.push_back(state);
        row.push_back(m.spec.port > 0 && m.http_status != 0
                          ? ("HTTP " + lexical_cast<std::string>(m.http_status))
                          : std::string("-"));
        row.push_back(m.spec.note);
        rows.push_back(row);
    }
    const int clicked_row = u.table(
        std::vector<std::string>{"名称", "类型", "端口", "状态", "健康", "说明"}, rows,
        std::vector<int>{70, 50, 50, 150, 60, 260}, static_cast<int>(selected_));
    if (clicked_row >= 0) {
        selected_ = static_cast<std::size_t>(clicked_row);
    }

    Managed& sel = apps_[selected_];
    u.gap(2);
    const int action = u.button_row(std::vector<std::string>{"启动", "停止", "自检", "窗口"});
    if (action == 0) {
        start_selected(sel);
    } else if (action == 1) {
        stop_selected(sel);
    } else if (action == 2) {
        // 服务与一次性任务都有 --demo，自检统一走它
        const std::vector<std::string>& args = sel.spec.run_args;
        const std::string exe = exe_path(sel.spec.name);
        if (sel.oneshot.started() && sel.oneshot.running()) {
            set_toast(sel.spec.name + ": 上一轮还在跑");
        } else {
            sel.oneshot = ChildProcess();
            std::string error;
            if (!sel.oneshot.start(exe, args, work_dir_, &error)) {
                set_toast(sel.spec.name + ": 自检启动失败 " + error);
            } else {
                sel.last_what = "自检";
                set_toast(sel.spec.name + ": 自检进行中");
            }
        }
    } else if (action == 3) {
        open_app_window(sel);
    }

    // ---- 选中项详情 ----
    u.section("选中项：" + sel.spec.name + "（" + sel.spec.kind + "）");
    u.kv("说明", sel.spec.note);
    if (sel.spec.port > 0) {
        u.kv("控制面", "http://127.0.0.1:" + lexical_cast<std::string>(sel.spec.port));
        u.kv("健康", sel.http_status != 0
                         ? ("HTTP " + lexical_cast<std::string>(sel.http_status) + " · " +
                            sel.ready_text + " · 往返 " +
                            lexical_cast<std::string>(
                                static_cast<int>(sel.rtt_ms * 10) / 10.0) +
                            "ms")
                         : (sel.probe_error.empty() ? std::string("未探测")
                                                    : ("探测失败: " + sel.probe_error)));
        if (sel.spec.port > 0) {
            if (u.inline_button("打开网页控制台") && sel.http_status != 0) {
                open_in_browser("http://127.0.0.1:" +
                                lexical_cast<std::string>(sel.spec.port) + "/");
            }
            u.newline();
        }
        u.sparkline(sel.rtt_history, "健康探测往返耗时（毫秒）/ 最近 60 次");
    }
    if (sel.service.started()) {
        u.kv("服务进程", "PID " + lexical_cast<std::string>(sel.service.pid()) + " · " +
                             (sel.service.running() ? "运行中" : "已退出"));
        u.kv("命令行", sel.service.command_line());
    }
    if (sel.oneshot.started()) {
        u.kv("任务进程", (sel.oneshot.running() ? "运行中 · PID " +
                                                      lexical_cast<std::string>(sel.oneshot.pid())
                                                : std::string("已退出 · 码 ") +
                                                      lexical_cast<std::string>(
                                                          sel.oneshot.exit_code())));
    }
    if (sel.metric_lines > 0) {
        u.kv("指标", lexical_cast<std::string>(sel.metric_lines) + " 行（/metrics）");
        u.log_view(split_lines(sel.metric_sample), 96);
    }

    // ---- 输出尾部 ----
    u.section("输出尾部");
    const std::vector<std::string> lines =
        (sel.oneshot.started() ? sel.oneshot.lines(200) : sel.service.lines(200));
    u.log_view(lines, 180);

    // ---- 底部状态 ----
    std::string status = "选中 " + sel.spec.name;
    if (!toast_.empty() && app::now_ms() - toast_ms_ < 8000) {
        status = toast_;
    }
    u.status(status, app::now_ms() - toast_ms_ < 8000 ? ui::theme::accent : ui::theme::dim);

    // ---- 窗口自检：合成点击 + 截图 ----
    if (selftest_) {
        if (frames_seen_ == 5 && refresh_button_rect_.w > 0 &&
            !synthetic_click_seen_) {
            u.simulate_click(refresh_button_rect_.center_x(), refresh_button_rect_.center_y());
        }
        if (frames_seen_ == 12 && !capture_requested_) {
            std::string error;
            capture_requested_ = u.capture_bmp(capture_path_, &error);
        }
    }
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            out.push_back(current);
            current.clear();
        } else if (text[i] != '\r') {
            current.push_back(text[i]);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

// ---------------- 自造子进程（自检用） ----------------

int child_echo(int count, int delay_ms)
{
    for (int i = 0; i < count; ++i) {
        std::cout << "child line " << i << "\n";
        std::cout.flush();
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    std::cerr << "child stderr tail\n";  // 顺便验证 stderr 也被合并进来
    std::cerr.flush();
    return 0;
}

int child_sleep_forever()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return 0;
}

// ---------------- 无头自检 ----------------

int run_demo(const std::string& self_exe, const std::string& exe_dir,
             const std::string& work_dir)
{
    DemoReport report("launcher 自检（原生 UI 后端 / 子进程托管 / 托管目录）");
    const std::string self = self_exe.empty() ? std::string("launcher") : self_exe;

    report.check(!work_dir.empty(), "工作目录: " + work_dir);
    report.info(std::string("原生窗口后端: ") + ui::Ui::backend() +
                (ui::Ui::available() ? "（可用，窗口自检见 --ui-selftest）"
                                     : "（本平台无窗口后端，托管逻辑照常可用）"));

    // 1) 输出捕获（stdout + stderr 合并，边跑边取）
    {
        ChildProcess child;
        std::string error;
        const std::vector<std::string> args = {"--child-echo", "4"};
        const bool started = child.start(self, args, work_dir, &error);
        report.check(started, "启动子进程（--child-echo 4）" + (started ? "" : " " + error));
        if (started) {
            std::string incremental;
            for (int i = 0; i < 200 && child.running(); ++i) {
                incremental += child.take_output();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            incremental += child.take_output();
            child.wait(3000);
            const std::string all = child.all_output();
            report.check(all.find("child line 0") != std::string::npos &&
                             all.find("child line 3") != std::string::npos,
                         "捕获全部 4 行标准输出");
            report.check(all.find("child stderr tail") != std::string::npos,
                         "标准错误与标准输出合并到同一条流");
            report.check(!incremental.empty() && incremental.size() <= all.size(),
                         "增量读取拿到过数据（" +
                             lexical_cast<std::string>(incremental.size()) + " 字节）");
            report.check(child.exit_code() == 0, "正常退出，退出码 0");
            const std::vector<std::string> lines = child.lines(10);
            report.check(lines.size() >= 5, "按行切分得到 " +
                                                lexical_cast<std::string>(lines.size()) + " 行");
        }
    }

    // 2) 退出码透传
    {
        ChildProcess child;
        std::string error;
        const std::vector<std::string> args = {"--exit-with", "7"};
        if (child.start(self, args, work_dir, &error)) {
            child.wait(3000);
            report.check(child.exit_code() == 7, "退出码原样透传（--exit-with 7）");
        } else {
            report.check(false, "启动子进程（--exit-with 7）: " + error);
        }
    }

    // 3) 停止常驻子进程
    {
        ChildProcess child;
        std::string error;
        const std::vector<std::string> args = {"--sleep-forever"};
        if (child.start(self, args, work_dir, &error)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const bool was_running = child.running();
            const bool killed = child.kill();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            report.check(was_running && killed && !child.running(),
                         "常驻子进程可被终止并回收（PID " +
                             lexical_cast<std::string>(child.pid()) + "）");
        } else {
            report.check(false, "启动常驻子进程: " + error);
        }
    }

    // 4) 托管目录：可执行文件都在同一目录，端口互不冲突
    {
        const char* names[] = {"jobsvc", "sysmon", "lanshare", "svcframe", "apitest",
                               "passvault", "test_runner"};
        int found = 0;
        for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            std::string file = names[i];
#if defined(_WIN32)
            file += ".exe";
#endif
            if (file_exists(app::workspace_file(exe_dir, file))) {
                ++found;
            }
        }
        report.check(found >= 5, "托管目录里找到 " + lexical_cast<std::string>(found) +
                                     "/7 个被托管可执行文件");
        report.info("托管目录: " + exe_dir);
    }

    // 5) 目录/端口不冲突（纯静态自检，避免把服务真的拉起来）
    {
        Launcher probe(exe_dir, work_dir);
        const std::vector<Managed>& apps = probe.apps();
        bool unique_ports = true;
        for (std::size_t i = 0; i < apps.size(); ++i) {
            for (std::size_t j = i + 1; j < apps.size(); ++j) {
                if (apps[i].spec.port != 0 && apps[i].spec.port == apps[j].spec.port) {
                    unique_ports = false;
                }
            }
        }
        report.check(apps.size() == 7, "托管项共 " +
                                           lexical_cast<std::string>(apps.size()) + " 个");
        report.check(unique_ports, "托管服务的控制面端口互不冲突");
    }

    return report.finish();
}

// ---------------- 窗口自检 ----------------

int run_ui_selftest(const std::string& exe_dir, const std::string& work_dir,
                    const std::string& shot_path)
{
    DemoReport report("launcher 窗口自检（原生窗口 / 合成点击 / 截图）");
    report.info(std::string("后端: ") + ui::Ui::backend());

    ui::Ui ui;
    std::string error;
    if (!ui.open("launcher 窗口自检", 1040, 720, &error)) {
        // 无窗口后端不是失败：如实记录并按「跳过」处理，三平台跑同一条用例
        report.info("跳过窗口自检: " + error);
        return 0;
    }
    Launcher launcher(exe_dir, work_dir);
    launcher.enable_ui_selftest();
    launcher.set_capture_path(shot_path);
    launcher.set_toast("窗口自检中");

    ui.set_frame_interval_ms(15);
    ui.set_frame_limit(30);
    const int frames = ui.run([&launcher](ui::Ui& u) { launcher.frame(u); });

    report.check(frames >= 30, "窗口渲染 " + lexical_cast<std::string>(frames) + " 帧");
    report.check(launcher.synthetic_click_seen(),
                 "合成点击命中「刷新面板」按钮并触发了处理逻辑");
    report.check(launcher.capture_requested() && ui.capture_ok(),
                 ui.capture_ok() ? ("客户区完整一帧已写出: " + launcher.capture_path())
                                 : ("截图失败: " + ui.capture_error()));
    if (ui.capture_ok()) {
        const std::size_t size = file_size(launcher.capture_path());
        report.check(size > 100 * 1024, "BMP 体积 " + lexical_cast<std::string>(size) +
                                            " 字节（含真实像素）");
    }
    (void)shot_path;
    return report.finish();
}

int run_window(const std::string& exe_dir, const std::string& work_dir)
{
    ui::Ui ui;
    std::string error;
    if (!ui.open("libmini 桌面控制台", 1180, 780, &error)) {
        std::cerr << "无法打开原生窗口: " << error << "\n";
        std::cerr << "提示：本层基于 Win32/GDI，非 Windows 平台请用 --demo 无头自检。\n";
        return 2;
    }
    Launcher launcher(exe_dir, work_dir);
    launcher.set_toast("已就绪：选一行，然后点「启动」或「自检」");
    ui.run([&launcher](ui::Ui& u) { launcher.frame(u); });
    launcher.close_app_windows();  // 主窗口关闭：所有面板窗口与托管进程一起收尾
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    // 自造子进程模式：只有自检会用，放在参数解析之前，避免被 Args 的校验挡下
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--child-echo") {
            const int count = i + 1 < argc ? atoi(argv[i + 1]) : 3;
            return child_echo(count, 5);
        }
        if (arg == "--exit-with") {
            const int code = i + 1 < argc ? atoi(argv[i + 1]) : 0;
            return code;
        }
        if (arg == "--sleep-forever") {
            return child_sleep_forever();
        }
    }

    Args args("launcher", "1.0",
              "libmini 桌面控制台：统一托管六个示例应用与测试聚合器（原生窗口）");
    args.add_flag("demo", "", "无头自检：托管/输出/退出码/终止全部自验证");
    args.add_flag("ui-selftest", "", "窗口自检：建窗 + 合成点击 + 截图（无后端则跳过）");
    args.add_option("work-dir", "", "托管进程的工作目录（默认系统临时目录）", std::string(""));
    args.add_option("shot", "", "窗口自检的截图输出路径", std::string(""));
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }

    const std::string exe_dir = module_dir(argc > 0 ? argv[0] : 0);
    std::string work_dir = trim(args.get_string("work-dir"));
    if (work_dir.empty()) {
        work_dir = app::make_workspace("launcher");
    }
    std::string shot = trim(args.get_string("shot"));
    if (shot.empty()) {
        shot = app::workspace_file(work_dir, "launcher_window.bmp");
    }

    std::string self_exe = app::workspace_file(exe_dir, "launcher");
#if defined(_WIN32)
    self_exe += ".exe";
#endif

    if (args.has_flag("demo")) {
        return run_demo(self_exe, exe_dir, work_dir);
    }
    if (args.has_flag("ui-selftest")) {
        return run_ui_selftest(exe_dir, work_dir, shot);
    }
    return run_window(exe_dir, work_dir);
}
