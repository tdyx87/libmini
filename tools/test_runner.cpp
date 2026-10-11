// libmini 测试聚合器
//
// 把 test/ 下的各 gtest 可执行文件集成到一个程序里跑：默认在终端给出实时进度与
// 汇总，加 --serve 则同时起一个本地 HTTP 服务，用网页仪表板看同一份进度。
//
// 用法：
//   test_runner                      跑全部套件（终端）
//   test_runner --suite libmini,rpc  只跑指定套件
//   test_runner --filter 'Sqlite*'   透传 gtest 过滤器
//   test_runner --list               列出套件与用例数
//   test_runner --serve --open       起浏览器仪表板并自动打开
//   test_runner --serve --no-run     只起仪表板，等网页上点运行
//   test_runner --ui                 开原生窗口（运行器视图）跑聚合器
//
// 本程序只依赖 libmini 本体（run_process / HttpServer / Args / JSON，以及原生 UI
// 模块 ui_panels/child_process），三平台一致；测试二进制路径由构建系统注入
// （见 tools/CMakeLists.txt）。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// windows.h 要在库头之前引入（下面用到控制台 API），因此必须先按库内约定
// 定义 WIN32_LEAN_AND_MEAN / NOMINMAX：否则 min/max 宏会破坏库头里的
// std::min / std::max（见 object_pool.h、retry.h）
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "libmini.h"
#include "tr_dashboard.h"
#include "tr_model.h"

using namespace libmini;
using namespace tr;

namespace {

bool g_color = false;

std::string paint(const char* code, const std::string& text)
{
    if (!g_color) {
        return text;
    }
    return std::string("\x1b[") + code + "m" + text + "\x1b[0m";
}

std::string bold(const std::string& s) { return paint("1", s); }
std::string dim(const std::string& s) { return paint("2", s); }
std::string red(const std::string& s) { return paint("31", s); }
std::string green(const std::string& s) { return paint("32", s); }
std::string yellow(const std::string& s) { return paint("33", s); }
std::string cyan(const std::string& s) { return paint("36", s); }

bool stdout_is_tty()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

void enable_ansi_colors()
{
#if defined(_WIN32)
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) {
        return;
    }
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING：Win10 起的 ANSI 转义支持
    SetConsoleMode(handle, mode | 0x0004);
#endif
}

std::string fmt_seconds(double seconds)
{
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(2);
    ss << seconds << "s";
    return ss.str();
}

std::string state_text(const SuiteResult& result)
{
    switch (result.state) {
    case SuiteState::Passed:
        return green("通过");
    case SuiteState::Failed:
        return red("失败");
    case SuiteState::Crashed:
        return yellow("崩溃");
    case SuiteState::NotRun:
        return dim("未运行");
    case SuiteState::Running:
        return cyan("运行中");
    case SuiteState::Pending:
        break;
    }
    return dim("待运行");
}

// 缩进多行文本（失败详情用）
std::string indent(const std::string& text, const std::string& prefix)
{
    std::string out;
    std::vector<std::string> lines = split(text, '\n');
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        out += prefix;
        out += line;
        out += "\n";
    }
    return out;
}

bool snapshot_all_good(const RunSnapshot& snapshot)
{
    if (snapshot.suites_total == 0) {
        return false;
    }
    return snapshot.suites_finished == snapshot.suites_total && snapshot.suites_failed == 0 &&
           snapshot.suites_crashed == 0 && snapshot.failed == 0;
}

// ---------------- 终端渲染 ----------------

// 实时进度 + 结束后汇总。运行期间独占 stdout 的「活动行」，套件跑完时先清掉
// 活动行再打一行结果，避免交错。
class TerminalRenderer
{
public:
    TerminalRenderer(RunState* state, bool live_progress, bool verbose)
        : state_(state), live_progress_(live_progress), verbose_(verbose), live_len_(0)
    {
    }

    void run_until_done()
    {
        const std::vector<SuiteSpec>& specs = state_->specs();
        std::vector<char> reported(specs.size(), 0);
        std::size_t finished = 0;
        for (;;) {
            RunSnapshot snapshot = state_->snapshot();
            for (std::size_t i = 0; i < snapshot.suites.size() && i < reported.size(); ++i) {
                const SuiteResult& result = snapshot.suites[i];
                if (!result.selected || reported[i]) {
                    continue;
                }
                if (result.state == SuiteState::Passed || result.state == SuiteState::Failed ||
                    result.state == SuiteState::Crashed || result.state == SuiteState::NotRun) {
                    reported[i] = 1;
                    ++finished;
                    clear_live();
                    print_suite_line(result, finished, snapshot.suites_total);
                }
            }
            if (!snapshot.running) {
                break;
            }
            draw_live(snapshot);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        clear_live();
    }

    void print_summary()
    {
        const RunSnapshot snapshot = state_->snapshot();
        std::cout << "\n" << bold("=== 汇总 ===") << "\n";
        std::cout << "套件   " << green("通过 " + lexical_cast<std::string>(snapshot.suites_passed))
                  << "   " << red("失败 " + lexical_cast<std::string>(snapshot.suites_failed))
                  << "   " << yellow("崩溃 " + lexical_cast<std::string>(snapshot.suites_crashed))
                  << "   共 " << lexical_cast<std::string>(snapshot.suites_total) << " 个\n";
        std::cout << "用例   " << green("通过 " + lexical_cast<std::string>(snapshot.passed))
                  << "   " << red("失败 " + lexical_cast<std::string>(snapshot.failed))
                  << "   " << dim("跳过 " + lexical_cast<std::string>(snapshot.skipped))
                  << "   共 " << lexical_cast<std::string>(snapshot.total) << " 条\n";
        std::cout << "用时   " << fmt_seconds(snapshot.elapsed_seconds) << "\n";
        if (snapshot_all_good(snapshot)) {
            std::cout << "结果   " << green(bold("全部通过")) << "\n";
        } else {
            std::cout << "结果   " << red(bold("存在失败")) << "\n";
            print_failures(snapshot);
        }
    }

private:
    void print_failures(const RunSnapshot& snapshot)
    {
        for (std::size_t i = 0; i < snapshot.suites.size(); ++i) {
            const SuiteResult& suite = snapshot.suites[i];
            if (!suite.selected) {
                continue;
            }
            if (suite.state != SuiteState::Failed && suite.state != SuiteState::Crashed) {
                continue;
            }
            std::cout << "\n" << red("[" + suite.spec.id + "] ") << dim(suite.spec.label) << "\n";
            if (!suite.note.empty()) {
                std::cout << indent(suite.note, "    ");
            }
            for (std::size_t c = 0; c < suite.cases.size(); ++c) {
                const CaseResult& item = suite.cases[c];
                if (item.status != CaseStatus::Failed) {
                    continue;
                }
                std::cout << "  " << red("✗ ") << item.full_name << " " << dim(fmt_seconds(item.seconds))
                          << "\n";
                for (std::size_t f = 0; f < item.failures.size(); ++f) {
                    std::cout << indent(item.failures[f], "      ");
                }
            }
            if (verbose_ && !suite.output_tail.empty()) {
                std::cout << dim("    --- 子进程输出尾部 ---") << "\n";
                std::cout << indent(suite.output_tail, "    ");
            }
        }
    }

    void print_suite_line(const SuiteResult& result, std::size_t index, int total)
    {
        std::ostringstream head;
        head << "[" << index << "/" << total << "] ";
        std::cout << dim(head.str()) << std::left << std::setw(10) << result.spec.id
                  << std::right << " " << state_text(result) << "  ";
        if (result.state == SuiteState::Passed || result.state == SuiteState::Failed) {
            std::cout << lexical_cast<std::string>(result.passed) << "/"
                      << lexical_cast<std::string>(result.total) << " 用例";
            if (result.failed > 0) {
                std::cout << "  " << red(lexical_cast<std::string>(result.failed) + " 失败");
            }
            if (!result.note.empty()) {
                std::cout << "  " << dim(result.note);
            }
        } else if (result.state == SuiteState::Crashed) {
            std::cout << result.note;
        } else if (result.state == SuiteState::NotRun && !result.note.empty()) {
            std::cout << dim(result.note);
        }
        if (result.state != SuiteState::NotRun) {
            std::cout << "  " << dim(fmt_seconds(result.seconds));
        }
        std::cout << "\n" << std::flush;
    }

    void draw_live(const RunSnapshot& snapshot)
    {
        if (!live_progress_) {
            return;
        }
        std::string running;
        int running_count = 0;
        double running_seconds = 0.0;
        const long long now = current_timestamp_ms();
        for (std::size_t i = 0; i < snapshot.suites.size(); ++i) {
            const SuiteResult& suite = snapshot.suites[i];
            if (!suite.selected || suite.state != SuiteState::Running) {
                continue;
            }
            if (running_count == 0) {
                running = suite.spec.id;
                running_seconds = static_cast<double>(now - suite.started_at_ms) / 1000.0;
            }
            ++running_count;
        }
        std::size_t finished = 0;
        for (std::size_t i = 0; i < snapshot.suites.size(); ++i) {
            const SuiteResult& suite = snapshot.suites[i];
            if (!suite.selected) {
                continue;
            }
            if (suite.state == SuiteState::Passed || suite.state == SuiteState::Failed ||
                suite.state == SuiteState::Crashed || suite.state == SuiteState::NotRun) {
                ++finished;
            }
        }
        std::ostringstream line;
        line << "  [" << finished << "/" << snapshot.suites_total << "] ";
        if (running_count > 0) {
            line << "运行中 " << running;
            if (running_count > 1) {
                line << " 等 " << running_count << " 个";
            }
            line << " (" << fmt_seconds(running_seconds) << ")";
        } else {
            line << "准备中…";
        }
        line << "   用例 " << snapshot.passed << " 通过";
        if (snapshot.failed > 0) {
            line << " / " << snapshot.failed << " 失败";
        }
        line << "   总用时 " << fmt_seconds(snapshot.elapsed_seconds);
        write_live(line.str());
    }

    void write_live(const std::string& line)
    {
        std::string out = "\r" + line;
        if (out.size() < live_len_) {
            out.append(live_len_ - out.size(), ' ');
        }
        live_len_ = out.size();
        std::cout << out << std::flush;
    }

    void clear_live()
    {
        if (live_len_ == 0) {
            return;
        }
        std::cout << "\r" << std::string(live_len_, ' ') << "\r" << std::flush;
        live_len_ = 0;
    }

    RunState* state_;
    bool live_progress_;
    bool verbose_;
    std::size_t live_len_;
};

// ---------------- 套件表处理 ----------------

std::vector<std::string> parse_suite_ids(const std::string& text)
{
    std::vector<std::string> out;
    std::vector<std::string> parts = split(text, ',');
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string id = trim(parts[i]);
        if (!id.empty()) {
            out.push_back(id);
        }
    }
    return out;
}

std::string suite_id_list(const std::vector<SuiteSpec>& specs)
{
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        ids.push_back(specs[i].id);
    }
    return join(ids, ", ");
}

int do_list(const std::vector<SuiteSpec>& specs, bool verbose)
{
    std::cout << bold("套件") << "  " << dim("（--suite 用左侧短名）") << "\n";
    int failures = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        std::vector<std::string> cases;
        std::string error;
        const bool ok = list_tests(specs[i], &cases, &error);
        std::cout << "  " << std::left << std::setw(10) << specs[i].id << std::right;
        if (ok) {
            std::cout << lexical_cast<std::string>(cases.size()) << " 条用例";
        } else {
            std::cout << red("不可用");
            ++failures;
        }
        std::cout << "  " << dim(specs[i].label) << "\n";
        std::cout << "    " << dim(specs[i].exe) << "\n";
        if (!ok && verbose) {
            std::cout << indent(error, "    ");
        }
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args("test_runner", "1.0",
              "libmini 测试聚合器：集成运行各测试套件，支持终端进度与浏览器仪表板");
    args.add_option("suite", "s", "只运行指定套件（逗号分隔；见 --list）", std::string());
    args.add_option("filter", "f", "透传给 gtest 的 --gtest_filter", std::string());
    args.add_int("jobs", "j", "并发运行的套件数（默认串行）", 1);
    args.add_int("timeout-ms", "", "单个套件的超时毫秒数（0 = 不限）", 900000);
    args.add_option("bin-dir", "", "覆盖测试可执行文件所在目录", std::string());
    args.add_option("json", "", "把聚合结果写成 JSON 报告到该路径", std::string());
    args.add_flag("list", "", "列出套件与用例数后退出");
    args.add_flag("fail-fast", "", "某套件失败后不再运行其余套件");
    args.add_flag("verbose", "v", "打印崩溃套件的子进程输出");
    args.add_flag("serve", "", "启动浏览器仪表板（本地 HTTP 服务）");
    args.add_int("port", "p", "仪表板监听端口（0 = 自动分配）", 0);
    args.add_flag("open", "", "仪表板就绪后用默认浏览器打开");
    args.add_flag("no-run", "", "配合 --serve：只起服务，不自动跑一轮");
    args.add_flag("no-progress", "", "关闭实时进度行");
    args.add_flag("no-color", "", "关闭 ANSI 颜色");
    args.add_flag("ui", "", "开原生窗口跑聚合器（运行器视图；仅 Windows 有窗口后端）");

    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }

    // ---- 原生窗口模式：窗口里的运行器视图托管「跑一轮聚合器」的自己 ----
    // 与 apps/ 下各应用的 --ui 同一种形状（启动器点「窗口」时也是这么调的）：
    // 先起子进程，再把它的实时输出与退出码画在窗口里，关窗时回收。
    if (args.has_flag("ui")) {
        const std::string report_path =
            unique_temp_path("libmini_tr_report_") + ".json";
        ui::AppWindowSpec spec;
        spec.title = "test_runner";
        spec.subtitle = "libmini 测试聚合器";
        spec.child_args.push_back(executable_path());
        spec.child_args.push_back("--json");
        spec.child_args.push_back(report_path);
        // --suite/--filter 透传给窗口里的那一轮，便于窗口里只跑关心的套件
        if (!args.get_string("suite").empty()) {
            spec.child_args.push_back("--suite");
            spec.child_args.push_back(args.get_string("suite"));
        }
        if (!trim(args.get_string("filter")).empty()) {
            spec.child_args.push_back("--filter");
            spec.child_args.push_back(args.get_string("filter"));
        }

        std::string window_error;
        const int code = ui::run_app_window(spec, &window_error);
        if (code == 2) {
            std::cerr << "错误：" << window_error << "\n";
        }
        remove_file(report_path);
        return code;
    }

    std::vector<SuiteSpec> specs = builtin_suites();
    if (specs.empty()) {
        std::cerr << "错误：没有可用的测试套件（构建时未注入 LIBMINI_TR_SUITE_*）\n";
        return 2;
    }

    const std::string bin_dir = trim(args.get_string("bin-dir"));
    if (!bin_dir.empty()) {
        for (std::size_t i = 0; i < specs.size(); ++i) {
            specs[i].exe = path_join(bin_dir, basename(specs[i].exe));
        }
    }

    const bool tty = stdout_is_tty();
    const bool no_color_env = env_has("NO_COLOR");
    g_color = tty && !args.has_flag("no-color") && !no_color_env;
    if (g_color) {
        enable_ansi_colors();
    }
    const bool live_progress = tty && !args.has_flag("no-progress");
    const bool verbose = args.has_flag("verbose");

    if (args.has_flag("list")) {
        return do_list(specs, verbose);
    }

    RunOptions options;
    options.suites = parse_suite_ids(args.get_string("suite"));
    options.gtest_filter = trim(args.get_string("filter"));
    options.jobs = args.get_int("jobs");
    options.timeout_ms = args.get_int("timeout-ms");
    options.keep_going = !args.has_flag("fail-fast");

    // 校验套件名，避免打错字时静默变成「全部没跑」
    if (!options.suites.empty()) {
        for (std::size_t i = 0; i < options.suites.size(); ++i) {
            bool found = false;
            for (std::size_t j = 0; j < specs.size(); ++j) {
                if (specs[j].id == options.suites[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "错误：未知套件 '" << options.suites[i] << "'；可用：" << suite_id_list(specs)
                          << "\n";
                return 2;
            }
        }
    }

    RunState state(specs);

    std::cout << bold("libmini 测试聚合器") << "  " << dim("(" + lexical_cast<std::string>(specs.size()) +
                                                          " 个套件)")
              << "\n";
    std::cout << dim("套件: " + suite_id_list(specs)) << "\n";

    // ---- 浏览器仪表板模式 ----
    if (args.has_flag("serve")) {
        std::string error;
        const std::string url = start_dashboard(&state, args.get_int("port"), &error);
        if (url.empty()) {
            std::cerr << "错误：仪表板启动失败：" << error << "\n";
            return 1;
        }
        std::cout << "仪表板: " << cyan(url) << "\n";
        std::cout << dim("提示：服务监听 0.0.0.0（同网段可访问），本机调试请留意")
                  << "\n" << std::flush;
        if (args.has_flag("open")) {
            if (!open_in_browser(url)) {
                std::cout << dim("（自动打开浏览器失败，请手动访问上面的地址）") << "\n";
            }
        }
        if (args.has_flag("no-run")) {
            std::cout << dim("已按 --no-run 只起服务；在网页上点「运行选中」即可。按 Ctrl+C 退出。")
                      << "\n";
        } else {
            std::string start_error;
            if (!state.start(options, &start_error)) {
                std::cerr << "错误：" << start_error << "\n";
            } else {
                TerminalRenderer renderer(&state, live_progress, verbose);
                renderer.run_until_done();
                renderer.print_summary();
            }
            std::cout << "\n" << dim("仪表板仍在 " + url + " 运行，按 Ctrl+C 退出。") << "\n"
                      << std::flush;
        }
        ConsoleExit exit_signal;
        exit_signal.wait();
        stop_dashboard();
        return 0;
    }

    // ---- 纯终端模式 ----
    std::string start_error;
    if (!state.start(options, &start_error)) {
        std::cerr << "错误：" << start_error << "\n";
        return 2;
    }
    TerminalRenderer renderer(&state, live_progress, verbose);
    renderer.run_until_done();
    renderer.print_summary();

    const std::string json_path = trim(args.get_string("json"));
    if (!json_path.empty()) {
        const std::string report = snapshot_to_json(state.snapshot());
        if (write_file(json_path, report)) {
            std::cout << "报告: " << json_path << "\n";
        } else {
            std::cerr << "错误：写入 JSON 报告失败：" << json_path << "\n";
        }
    }

    return snapshot_all_good(state.snapshot()) ? 0 : 1;
}
