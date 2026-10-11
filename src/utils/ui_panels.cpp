// 成品窗口层实现（见 ui_panels.h 的说明）。库模块：内部只用本库的模块
// （native_ui / child_process / http_client / json_utils），无第三方 UI 依赖。

#include "ui_panels.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

#include "child_process.h"
#include "libmini.h"
#include "native_ui.h"

namespace libmini {
namespace ui {

namespace {

// ---------------- JSON 点分路径取值 ----------------

// 支持对象键与数组下标（"series.0.cpu"）；数值与可转数值的字符串都接受
bool json_number(const JsonValue& root, const std::string& path, double* out)
{
    const JsonValue* node = &root;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t dot = path.find('.', begin);
        const std::string key =
            dot == std::string::npos ? path.substr(begin) : path.substr(begin, dot - begin);
        if (!key.empty()) {
            if (node->is_object()) {
                if (!node->contains(key)) {
                    return false;
                }
                node = &(*node)[key];
            } else if (node->is_array()) {
                const int index = lexical_cast_or<int>(key, -1);
                if (index < 0 || static_cast<std::size_t>(index) >= node->size()) {
                    return false;
                }
                node = &(*node)[static_cast<std::size_t>(index)];
            } else {
                return false;
            }
        }
        if (dot == std::string::npos) {
            break;
        }
        begin = dot + 1;
    }
    if (node->is_number()) {
        *out = node->get<double>();
        return true;
    }
    if (node->is_string()) {
        const std::string text = node->get<std::string>();
        *out = lexical_cast_or<double>(text, 0.0);
        return true;
    }
    return false;
}

// 把长文本截成最多 max_lines 行（界面不展示巨量内容）
std::vector<std::string> head_lines(const std::string& text, std::size_t max_lines)
{
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n') {
            out.push_back(current);
            current.clear();
            if (out.size() >= max_lines) {
                out.push_back("…（已截断）");
                return out;
            }
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

// ---------------- 服务面板状态 ----------------

struct PanelState
{
    PanelSpec spec;
    int http_status = 0;
    int ready_status = 0;
    std::string error;
    std::int64_t last_poll_ms = 0;
    std::size_t poll_count = 0;

    std::vector<std::string> metric_lines;                 // 过滤后的指标行
    std::vector<std::vector<std::string> > view_lines;     // 每个 json_view 的展示行
    JsonValue first_view;                                  // 第一个 JSON 视图的解析结果（折线取样用）
    std::vector<std::vector<double> > series;              // 每条折线的历史
    std::vector<std::string> action_log;                   // 动作结果日志
};

void poll_panel(PanelState* state)
{
    state->last_poll_ms = current_timestamp_ms();
    ++state->poll_count;
    state->error.clear();
    HttpClient http(state->spec.base_url);

    Result<HttpResponse> health = http.try_get("/healthz");
    if (!health.ok()) {
        state->http_status = 0;
        state->ready_status = 0;
        state->error = health.status().to_string();
        return;
    }
    state->http_status = health.value().status;
    Result<HttpResponse> ready = http.try_get("/readyz");
    state->ready_status = ready.ok() ? ready.value().status : 0;

    // 指标（按子串过滤）
    state->metric_lines.clear();
    Result<HttpResponse> metrics = http.try_get("/metrics");
    if (metrics.ok()) {
        const std::string& body = metrics.value().body;
        std::size_t begin = 0;
        while (begin < body.size()) {
            const std::size_t end = body.find('\n', begin);
            const std::string line =
                body.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!line.empty() && line[0] != '#') {
                bool keep = state->spec.metric_filters.empty();
                for (std::size_t i = 0; !keep && i < state->spec.metric_filters.size(); ++i) {
                    keep = line.find(state->spec.metric_filters[i]) != std::string::npos;
                }
                if (keep) {
                    state->metric_lines.push_back(line);
                }
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        if (state->metric_lines.size() > 200) {
            state->metric_lines.resize(200);
            state->metric_lines.push_back("…（已截断）");
        }
    }

    // JSON 视图 + 折线取样
    state->view_lines.assign(state->spec.json_views.size(), std::vector<std::string>());
    state->first_view = JsonValue();
    for (std::size_t i = 0; i < state->spec.json_views.size(); ++i) {
        Result<HttpResponse> replyhttp = http.try_get(state->spec.json_views[i]);
        if (!replyhttp.ok()) {
            state->view_lines[i].push_back("请求失败: " + replyhttp.status().to_string());
            continue;
        }
        const std::string& body = replyhttp.value().body;
        try {
            const JsonValue doc = parse_json(body);
            if (i == 0) {
                state->first_view = doc;
            }
            state->view_lines[i] = head_lines(to_json_string(doc), 24);
        } catch (const std::exception&) {
            state->view_lines[i] = head_lines(body, 24);
        }
    }
    if (state->series.size() != state->spec.series_paths.size()) {
        state->series.assign(state->spec.series_paths.size(), std::vector<double>());
    }
    // 折线取样点落在第一个 JSON 视图上（约定：series 从第一个端点里取）
    for (std::size_t i = 0; i < state->spec.series_paths.size() && state->first_view.is_object();
         ++i) {
        double value = 0.0;
        if (json_number(state->first_view, state->spec.series_paths[i], &value)) {
            state->series[i].push_back(value);
            if (state->series[i].size() > 120) {
                state->series[i].erase(state->series[i].begin());
            }
        }
    }
}

void render_panel(Ui& u, PanelState* state)
{
    PanelSpec& spec = state->spec;
    if (u.every(spec.poll_ms)) {
        poll_panel(state);
    }

    u.title(spec.title, spec.subtitle + " · " + spec.base_url);
    u.badge(state->http_status == 200 ? "存活" : "未连接",
            state->http_status == 200 ? theme::ok : theme::err);
    u.badge(state->ready_status == 200 ? "就绪" : "未就绪",
            state->ready_status == 200 ? theme::ok : theme::warn);
    u.badge("轮询 " + lexical_cast<std::string>(static_cast<long long>(state->poll_count)) + " 次",
            theme::dim);
    u.badge("刷新", theme::dim);
    u.newline();

    // 动作按钮
    if (!spec.actions.empty()) {
        u.section("动作");
        std::vector<std::string> labels;
        for (std::size_t i = 0; i < spec.actions.size(); ++i) {
            labels.push_back(spec.actions[i].label);
        }
        const int hit = u.button_row(labels);
        if (hit >= 0) {
            const PanelAction& action = spec.actions[static_cast<std::size_t>(hit)];
            HttpClient http(spec.base_url);
            Result<HttpResponse> reply =
                action.method == "POST"
                    ? http.try_post_json(action.path, action.body.empty() ? std::string("{}")
                                                                         : action.body)
                    : http.try_get(action.path);
            std::string line = action.label + " → " +
                               (reply.ok() ? ("HTTP " + lexical_cast<std::string>(
                                                            reply.value().status))
                                           : reply.status().to_string());
            if (reply.ok() && !reply.value().body.empty()) {
                line += " · " + head_lines(reply.value().body, 1)[0];
            }
            state->action_log.push_back(line);
            poll_panel(state);  // 动作之后立刻刷新，界面才跟得上
        }
    }

    // 折线
    if (!spec.series_paths.empty()) {
        u.section("趋势");
        for (std::size_t i = 0; i < spec.series_paths.size(); ++i) {
            const std::vector<double>& data = state->series[i];
            std::string caption = spec.series_paths[i];
            if (!data.empty()) {
                caption += " = " + lexical_cast<std::string>(data[data.size() - 1]);
            }
            u.sparkline(data, caption);
        }
    }

    // 概要
    u.section("概要");
    u.kv("控制面", spec.base_url);
    u.kv("最近轮询", state->last_poll_ms > 0
                         ? (lexical_cast<std::string>(static_cast<long long>(
                                (current_timestamp_ms() - state->last_poll_ms) / 1000)) +
                            " 秒前")
                         : std::string("尚未轮询"));
    if (!state->error.empty()) {
        u.kv("上次错误", state->error, theme::err);
    }

    // JSON 视图
    for (std::size_t i = 0; i < spec.json_views.size(); ++i) {
        u.section(spec.json_views[i]);
        u.log_view(state->view_lines[i], 160);
    }

    // 指标
    u.section("指标（/metrics 过滤后 " +
              lexical_cast<std::string>(state->metric_lines.size()) + " 行）");
    u.log_view(state->metric_lines, 150);

    if (!state->action_log.empty()) {
        u.section("动作记录");
        u.log_view(state->action_log, 90);
    }

    u.status(state->http_status == 200 ? ("已连接 " + spec.base_url) : ("未连接: " + state->error),
             state->http_status == 200 ? theme::ok : theme::err);
}

// ---------------- 运行器视图（一次性任务） ----------------

struct RunnerState
{
    AppWindowSpec spec;
    ChildProcess child;
    std::string last_error;
    std::size_t run_count = 0;
};

void start_child(RunnerState* state)
{
    if (state->spec.child_args.empty()) {
        return;
    }
    if (state->child.started() && state->child.running()) {
        return;
    }
    state->child = ChildProcess();
    const std::string program = state->spec.child_args[0];
    std::vector<std::string> args(state->spec.child_args.begin() + 1,
                                 state->spec.child_args.end());
    std::string error;
    if (!state->child.start(program, args, state->spec.work_dir, &error)) {
        state->last_error = error;
        return;
    }
    state->last_error.clear();
    ++state->run_count;
}

void render_runner(Ui& u, RunnerState* state)
{
    const bool alive = state->child.started() && state->child.running();
    u.title(state->spec.title, state->spec.subtitle);

    int code = 0;
    const bool finished = state->child.started() && state->child.try_finish(&code);
    u.badge(alive ? "运行中" : (finished ? "已结束" : "空闲"),
            alive ? theme::ok : (finished && code == 0 ? theme::dim : theme::err));
    if (finished) {
        u.badge("退出码 " + lexical_cast<std::string>(code),
                code == 0 ? theme::ok : theme::err);
    }
    u.badge("已跑 " + lexical_cast<std::string>(static_cast<long long>(state->run_count)) + " 轮",
            theme::dim);
    u.newline();

    const int hit = u.button_row(std::vector<std::string>{"重跑", "停止", "清空输出"});
    if (hit == 0) {
        start_child(state);
    } else if (hit == 1) {
        if (alive) {
            state->child.kill();
        }
    } else if (hit == 2) {
        state->child.clear_output();
        state->last_error.clear();
    }

    if (!state->spec.child_args.empty()) {
        u.kv("命令行", state->child.command_line());
    }
    if (!state->last_error.empty()) {
        u.kv("提示", state->last_error, theme::warn);
    }
    u.section("实时输出");
    u.log_view(state->child.lines(400), 420);
    u.status(alive ? "子进程运行中" : "子进程已结束", alive ? theme::ok : theme::dim);
}

}  // namespace

int run_app_window(const AppWindowSpec& spec, std::string* error)
{
    if (!Ui::available()) {
        // 自检档位下「没有窗口后端」是平台事实而非失败：如实跳过，让三平台 CI 跑同一条用例
        if (spec.report) {
            std::cout << "[跳过] 本平台没有原生窗口后端，窗口自检不在本平台执行\n";
            return 0;
        }
        if (error != 0) {
            *error = "当前平台没有原生窗口后端（Win32/GDI 专属；其它平台请用 --demo）";
        }
        return 2;
    }

    // 服务面板要先确保服务在跑；运行器视图也要先把子进程拉起来
    ChildProcess service;
    RunnerState runner;
    runner.spec = spec;

    if (spec.port > 0 && !spec.child_args.empty()) {
        const std::string program = spec.child_args[0];
        std::vector<std::string> args(spec.child_args.begin() + 1, spec.child_args.end());
        std::string start_error;
        if (!service.start(program, args, spec.work_dir, &start_error)) {
            if (error != 0) {
                *error = "启动后台服务失败: " + start_error;
            }
            return 1;
        }
    }

    // 服务面板：等被托管服务把 /healthz 答上再开窗（窗口一开就有数据，而不是先空一片）
    if (spec.port > 0 && service.started()) {
        HttpClient probe("http://127.0.0.1:" + lexical_cast<std::string>(spec.port));
        const std::int64_t deadline = current_timestamp_ms() + 10000;
        bool ready = false;
        while (current_timestamp_ms() < deadline) {
            Result<HttpResponse> reply = probe.try_get("/healthz");
            if (reply.ok() && reply.value().status == 200) {
                ready = true;
                break;
            }
            if (service.started() && service.try_finish(0)) {
                break;  // 服务先挂了，不白等
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        if (spec.report) {
            std::cout << (ready ? "[通过] " : "[失败] ") << "后台服务就绪（端口 "
                      << spec.port << "）\n";
        }
    }

    Ui window;
    std::string open_error;
    if (!window.open(spec.title, 980, 720, &open_error)) {
        if (service.started()) {
            service.kill();
        }
        if (spec.report) {
            // 有后端却建不出窗口（例如无交互桌面）也算平台限制：跳过而不误报失败
            std::cout << "[跳过] 无法建立窗口：" << open_error << "\n";
            return 0;
        }
        if (error != 0) {
            *error = open_error;
        }
        return 2;
    }

    PanelState panel;
    panel.spec = spec.panel;
    if (panel.spec.base_url.empty() && spec.port > 0) {
        panel.spec.base_url = "http://127.0.0.1:" + lexical_cast<std::string>(spec.port);
    }
    if (panel.spec.title.empty()) {
        panel.spec.title = spec.title;
        panel.spec.subtitle = spec.subtitle;
    }

    // 自检档位：跑满指定帧数后自动关窗（帧结束前请求截图，确保拿到完整一帧）
    if (spec.frames > 0) {
        window.set_frame_limit(spec.frames);
        window.set_frame_interval_ms(15);
    }
    bool capture_requested = false;
    const int frames_limit = spec.frames;
    const std::string shot = spec.shot_path;
    const std::function<void(Ui&)> hooks = [&](Ui& u) {
        if (!shot.empty() && !capture_requested && frames_limit > 0 &&
            u.frames() + 4 >= frames_limit) {
            std::string ignored;
            capture_requested = u.capture_bmp(shot, &ignored);
        }
    };

    if (spec.port > 0) {
        window.run([&panel, &hooks](Ui& u) {
            hooks(u);
            render_panel(u, &panel);
        });
    } else {
        start_child(&runner);
        window.run([&runner, &hooks](Ui& u) {
            hooks(u);
            render_runner(u, &runner);
        });
    }

    if (runner.child.started() && runner.child.running()) {
        runner.child.kill();
    }
    if (service.started() && service.running()) {
        service.kill();
    }

    // 自检结论：窗口真的渲染了、截图真的写出了、面板真的连上了被托管的服务
    int result = 0;
    if (frames_limit > 0 && spec.report) {
        std::cout << (window.frames() >= frames_limit ? "[通过] " : "[失败] ")
                  << "窗口渲染 " << window.frames() << " 帧\n";
        if (window.frames() < frames_limit) {
            result = 1;
        }
    }
    if (!shot.empty() && spec.report) {
        std::cout << (window.capture_ok() ? "[通过] " : "[失败] ") << "窗口截图: " << shot << "\n";
        if (!window.capture_ok()) {
            std::cout << "        原因: " << window.capture_error() << "\n";
            result = 1;
        }
    }
    if (spec.port > 0 && frames_limit > 0 && spec.report) {
        const bool connected = panel.http_status == 200;
        std::cout << (connected ? "[通过] " : "[失败] ")
                  << "窗口期间连上控制面（/healthz 返回 HTTP " << panel.http_status << "）\n";
        if (!connected) {
            std::cout << "        控制面: " << panel.spec.base_url << "\n";
            result = 1;
        }
    }
    if (spec.port == 0 && frames_limit > 0 && spec.report) {
        const bool ran = runner.child.started();
        std::cout << (ran ? "[通过] " : "[失败] ")
                  << "窗口期间拉起并展示了子进程输出" << (ran ? "" : "（子进程没起来）") << "\n";
        if (!ran) {
            result = 1;
        }
    }
    return result;
}

// ---------------- 后台面板窗口（启动器用） ----------------

struct PanelWindow::Impl
{
    std::atomic<bool> stop_requested;
    std::atomic<bool> alive;
    std::thread thread;

    Impl() : stop_requested(false), alive(false) {}
};

PanelWindow::PanelWindow() : impl_(new Impl())
{
}

PanelWindow::~PanelWindow()
{
    request_close();
    join();
    delete impl_;
}

std::shared_ptr<PanelWindow> PanelWindow::open(const PanelSpec& spec, std::string* error)
{
    if (!Ui::available()) {
        if (error != 0) {
            *error = "当前平台没有原生窗口后端（Win32/GDI 专属）";
        }
        return std::shared_ptr<PanelWindow>();
    }
    std::shared_ptr<PanelWindow> window(new PanelWindow());
    PanelSpec copy = spec;
    if (copy.title.empty()) {
        copy.title = "应用面板";
    }
    window->impl_->thread = std::thread([window, copy]() {
        Ui ui;
        std::string open_error;
        if (!ui.open(copy.title, 940, 700, &open_error)) {
            window->impl_->alive.store(false);
            return;
        }
        window->impl_->alive.store(true);
        PanelState state;
        state.spec = copy;
        ui.run([window, &state](Ui& u) {
            if (window->impl_->stop_requested.load()) {
                u.close();
            }
            render_panel(u, &state);
        });
        window->impl_->alive.store(false);
    });
    return window;
}

void PanelWindow::request_close()
{
    impl_->stop_requested.store(true);
}

void PanelWindow::join()
{
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

bool PanelWindow::running() const
{
    return impl_->alive.load();
}

}  // namespace ui
}  // namespace libmini
