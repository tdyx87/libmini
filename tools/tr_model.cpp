// 测试聚合器的数据模型实现（见 tr_model.h 顶部说明）。
#include "tr_model.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <sstream>

#include "libmini.h"
#include "utils/process.h"

using namespace libmini;

namespace tr {

namespace {

// 内置套件表的编译期注入点。tools/CMakeLists.txt 用生成器表达式把各测试
// 目标的绝对路径塞进这些宏；缺失时（例如手工编译本工具）该套件不出现。
void add_suite(std::vector<SuiteSpec>* out, const char* id, const char* label,
               const char* exe)
{
    if (exe == nullptr || exe[0] == '\0') {
        return;
    }
    SuiteSpec spec;
    spec.id = id;
    spec.label = label;
    spec.exe = exe;
    out->push_back(spec);
}

// 保留文本尾部（诊断输出只关心结尾的报错）
std::string tail_of(const std::string& text, std::size_t max_len)
{
    if (text.size() <= max_len) {
        return text;
    }
    return std::string("...[已截断前段]...\n") + text.substr(text.size() - max_len);
}

std::string describe_options(const RunOptions& opts)
{
    std::ostringstream ss;
    ss << "套件=" << (opts.suites.empty() ? std::string("全部") : join(opts.suites, ","))
       << "  并发=" << (opts.jobs < 1 ? 1 : opts.jobs)
       << "  超时=" << (opts.timeout_ms / 1000) << "s"
       << "  失败即停=" << (opts.keep_going ? "否" : "是");
    if (!opts.gtest_filter.empty()) {
        ss << "  过滤=" << opts.gtest_filter;
    }
    return ss.str();
}

bool suite_selected(const RunOptions& opts, const SuiteSpec& spec)
{
    if (opts.suites.empty()) {
        return true;
    }
    for (std::size_t i = 0; i < opts.suites.size(); ++i) {
        if (opts.suites[i] == spec.id) {
            return true;
        }
    }
    return false;
}

// gtest 的 time 字段是 "0.003s" 这样的文本
std::string node_string(const JsonValue& node, const char* key);

double parse_time_field(const JsonValue& node)
{
    const std::string text = node_string(node, "time");
    if (text.empty()) {
        return 0.0;
    }
    const std::size_t pos = text.find('s');
    const std::string number = (pos == std::string::npos) ? text : text.substr(0, pos);
    std::istringstream ss(number);
    double value = 0.0;
    ss >> value;
    return ss.fail() ? 0.0 : value;
}

std::string node_string(const JsonValue& node, const char* key)
{
    if (!node.is_object()) {
        return std::string();
    }
    JsonValue::const_iterator it = node.find(key);
    if (it == node.end() || it->is_null()) {
        return std::string();
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return it->dump();
}

}  // namespace

std::vector<SuiteSpec> builtin_suites()
{
    std::vector<SuiteSpec> out;
#ifdef LIBMINI_TR_SUITE_libmini
    add_suite(&out, "libmini", "libmini 基础模块", LIBMINI_TR_SUITE_libmini);
#endif
#ifdef LIBMINI_TR_SUITE_rpc
    add_suite(&out, "rpc", "RPC / 传输", LIBMINI_TR_SUITE_rpc);
#endif
#ifdef LIBMINI_TR_SUITE_utils
    add_suite(&out, "utils", "通用工具", LIBMINI_TR_SUITE_utils);
#endif
#ifdef LIBMINI_TR_SUITE_common
    add_suite(&out, "common", "常用模块", LIBMINI_TR_SUITE_common);
#endif
#ifdef LIBMINI_TR_SUITE_args
    add_suite(&out, "args", "命令行解析", LIBMINI_TR_SUITE_args);
#endif
    return out;
}

const char* suite_state_name(SuiteState state)
{
    switch (state) {
    case SuiteState::Pending:
        return "pending";
    case SuiteState::Running:
        return "running";
    case SuiteState::Passed:
        return "passed";
    case SuiteState::Failed:
        return "failed";
    case SuiteState::Crashed:
        return "crashed";
    case SuiteState::NotRun:
        return "not_run";
    }
    return "unknown";
}

const char* case_status_name(CaseStatus status)
{
    switch (status) {
    case CaseStatus::Passed:
        return "passed";
    case CaseStatus::Failed:
        return "failed";
    case CaseStatus::Skipped:
        return "skipped";
    }
    return "unknown";
}

bool list_tests(const SuiteSpec& spec, std::vector<std::string>* out,
                std::string* error)
{
    out->clear();
    if (!file_exists(spec.exe)) {
        *error = "可执行文件不存在: " + spec.exe;
        return false;
    }
    ProcessResult pr = run_process(spec.exe, std::vector<std::string>(1, "--gtest_list_tests"),
                                   60000);
    if (pr.timed_out) {
        *error = "列出用例超时";
        return false;
    }
    if (pr.exit_code != 0) {
        *error = "退出码 " + lexical_cast<std::string>(pr.exit_code) + ": " +
                 tail_of(pr.stderr_text, 400);
        return false;
    }
    // 输出形态：
    //   ResultTest.
    //     StatusClassifiesAndFormats
    //     HoldsValueOrError
    std::string current;
    std::vector<std::string> lines = split(pr.stdout_text, '\n');
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.empty()) {
            continue;
        }
        if (line[0] != ' ' && line[0] != '\t') {
            current = line;
            continue;
        }
        std::string name = trim(line);
        const std::size_t comment = name.find('#');
        if (comment != std::string::npos) {
            name = trim(name.substr(0, comment));
        }
        if (name.empty()) {
            continue;
        }
        if (current.empty()) {
            out->push_back(name);
        } else {
            out->push_back(trim(current) + name);
        }
    }
    return true;
}

bool parse_gtest_json(const std::string& text, SuiteResult* out,
                      std::string* error)
{
    JsonValue doc;
    try {
        doc = parse_json(text);
    } catch (const std::exception& e) {
        *error = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        *error = "JSON 根节点不是对象";
        return false;
    }
    out->cases.clear();
    out->total = out->passed = out->failed = out->skipped = 0;
    const JsonValue suites_node =
        doc.find("testsuites") == doc.end() ? JsonValue::array() : doc["testsuites"];
    if (!suites_node.is_array()) {
        *error = "testsuites 不是数组";
        return false;
    }
    for (JsonValue::const_iterator sit = suites_node.begin(); sit != suites_node.end(); ++sit) {
        const JsonValue& group = *sit;
        const JsonValue cases_node =
            group.find("testsuite") == group.end() ? JsonValue::array() : group["testsuite"];
        if (!cases_node.is_array()) {
            continue;
        }
        for (JsonValue::const_iterator cit = cases_node.begin(); cit != cases_node.end(); ++cit) {
            const JsonValue& node = *cit;
            CaseResult c;
            c.name = node_string(node, "name");
            c.suite_name = node_string(node, "classname");
            if (c.suite_name.empty()) {
                c.suite_name = node_string(group, "name");
            }
            c.full_name = c.suite_name.empty() ? c.name : (c.suite_name + "." + c.name);
            c.seconds = parse_time_field(node);

            const std::string result = node_string(node, "result");
            const JsonValue failures_node =
                node.find("failures") == node.end() ? JsonValue::array() : node["failures"];
            if (failures_node.is_array()) {
                for (JsonValue::const_iterator fit = failures_node.begin();
                     fit != failures_node.end(); ++fit) {
                    std::string message = node_string(*fit, "failure");
                    if (message.empty()) {
                        message = node_string(*fit, "message");
                    }
                    if (message.empty()) {
                        message = fit->is_string() ? fit->get<std::string>() : fit->dump();
                    }
                    c.failures.push_back(message);
                }
            }
            if (result == "SKIPPED") {
                c.status = CaseStatus::Skipped;
                ++out->skipped;
            } else if (!c.failures.empty()) {
                c.status = CaseStatus::Failed;
                ++out->failed;
            } else {
                c.status = CaseStatus::Passed;
                ++out->passed;
            }
            ++out->total;
            out->cases.push_back(c);
        }
    }
    // 报告里的汇总字段优先（用例是被过滤掉时两者一致，取报告值更省心）
    if (doc.find("tests") != doc.end() && doc["tests"].is_number_integer()) {
        out->total = doc["tests"].get<int>();
    }
    if (doc.find("failures") != doc.end() && doc["failures"].is_number_integer()) {
        out->failed = doc["failures"].get<int>();
    }
    if (doc.find("disabled") != doc.end() && doc["disabled"].is_number_integer()) {
        out->skipped = doc["disabled"].get<int>();
    }
    out->passed = out->total - out->failed - out->skipped;
    if (out->passed < 0) {
        out->passed = 0;
    }
    return true;
}

SuiteResult run_suite(const SuiteSpec& spec, const RunOptions& opts)
{
    SuiteResult r;
    r.spec = spec;
    r.selected = true;

    if (!file_exists(spec.exe)) {
        r.state = SuiteState::Crashed;
        r.note = "可执行文件不存在: " + spec.exe;
        return r;
    }

    // 报告写到系统临时目录，避免污染源码树/工作目录
    const std::string json_path = unique_temp_path("libmini_tr_");
    std::vector<std::string> args;
    args.push_back("--gtest_color=no");
    args.push_back("--gtest_output=json:" + json_path);
    if (!opts.gtest_filter.empty()) {
        args.push_back("--gtest_filter=" + opts.gtest_filter);
    }

    Stopwatch sw;
    ProcessResult pr = run_process(spec.exe, args, opts.timeout_ms);
    r.seconds = sw.elapsed_seconds();
    r.exit_code = pr.exit_code;
    r.timed_out = pr.timed_out;

    std::string json_text;
    if (file_exists(json_path)) {
        json_text = read_file(json_path);
        remove_file(json_path);
    }

    std::string parse_error;
    if (json_text.empty()) {
        r.state = SuiteState::Crashed;
        if (pr.timed_out) {
            const int ms = opts.timeout_ms;
            const std::string amount =
                (ms >= 1000 && ms % 1000 == 0)
                    ? lexical_cast<std::string>(ms / 1000) + "s"
                    : lexical_cast<std::string>(ms) + "ms";
            r.note = "超时（" + amount + "）被终止，未产出报告";
        } else if (pr.exit_code != 0) {
            r.note = "进程异常退出（退出码 " + lexical_cast<std::string>(pr.exit_code) +
                     "），未产出报告";
        } else {
            r.note = "未产出 gtest JSON 报告";
        }
    } else if (!parse_gtest_json(json_text, &r, &parse_error)) {
        r.state = SuiteState::Crashed;
        r.note = parse_error;
    } else if (r.failed > 0 || pr.exit_code != 0) {
        r.state = SuiteState::Failed;
        if (r.failed == 0) {
            r.note = "无失败用例但退出码为 " +
                     lexical_cast<std::string>(pr.exit_code);
        }
    } else {
        r.state = SuiteState::Passed;
        if (r.total == 0 && !opts.gtest_filter.empty()) {
            r.note = "过滤器未匹配到用例";
        }
    }

    if (r.state == SuiteState::Crashed) {
        const std::string combined = pr.stdout_text + "\n" + pr.stderr_text;
        r.output_tail = tail_of(combined, 4000);
    }
    return r;
}

// ---------------- RunState ----------------

RunState::RunState() : specs_(builtin_suites()), running_(0), stop_(false), next_(0),
                       started_ms_(0), finished_ms_(0)
{
    results_.resize(specs_.size());
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        results_[i].spec = specs_[i];
    }
}

RunState::RunState(const std::vector<SuiteSpec>& suites)
    : specs_(suites), running_(0), stop_(false), next_(0), started_ms_(0),
      finished_ms_(0)
{
    results_.resize(specs_.size());
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        results_[i].spec = specs_[i];
    }
}

RunState::~RunState()
{
    request_stop();
    wait();
}

bool RunState::start(const RunOptions& opts, std::string* error)
{
    if (running_.load() != 0) {
        if (error != nullptr) {
            *error = "已有一轮测试在运行";
        }
        return false;
    }
    std::vector<std::size_t> chosen;
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        if (suite_selected(opts, specs_[i])) {
            chosen.push_back(i);
        }
    }
    if (chosen.empty()) {
        if (error != nullptr) {
            *error = "没有匹配的套件（检查 --suite 的取值）";
        }
        return false;
    }
    if (supervisor_.joinable()) {
        supervisor_.join();  // 上一轮已结束，回收线程
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.assign(specs_.size(), SuiteResult());
        for (std::size_t i = 0; i < specs_.size(); ++i) {
            results_[i].spec = specs_[i];
        }
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            results_[chosen[i]].selected = true;
        }
        opts_ = opts;
        started_at_ = current_time_string();
        options_desc_ = describe_options(opts);
        started_ms_ = current_timestamp_ms();
        finished_ms_ = 0;
    }
    stop_.store(false);
    next_.store(0);
    running_.store(1);

    int jobs = opts.jobs < 1 ? 1 : opts.jobs;
    if (static_cast<std::size_t>(jobs) > chosen.size()) {
        jobs = static_cast<int>(chosen.size());
    }
    supervisor_ = std::thread([this, jobs]() {
        std::vector<std::thread> workers;
        for (int i = 0; i < jobs; ++i) {
            workers.push_back(std::thread([this]() { worker_loop(); }));
        }
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
        {
            // 冻结总用时：跑完后 elapsed 不再随时间增长
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ms_ = current_timestamp_ms();
            running_.store(0);
        }
    });
    return true;
}

void RunState::worker_loop()
{
    for (;;) {
        const std::size_t index = next_.fetch_add(1);
        if (index >= specs_.size()) {
            return;
        }
        bool selected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            selected = results_[index].selected;
        }
        if (!selected) {
            continue;
        }
        if (stop_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            results_[index].state = SuiteState::NotRun;
            results_[index].note = "已请求停止，未执行";
            continue;
        }
        SuiteSpec spec;
        RunOptions opts;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_[index].state = SuiteState::Running;
            results_[index].started_at_ms = current_timestamp_ms();
            spec = specs_[index];
            opts = opts_;
        }
        const SuiteResult result = run_suite(spec, opts);
        std::function<void(const SuiteResult&)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_[index] = result;
            callback = on_finished_;
        }
        if (result.state == SuiteState::Failed || result.state == SuiteState::Crashed) {
            if (!opts.keep_going) {
                stop_.store(true);
            }
        }
        if (callback) {
            callback(result);
        }
    }
}

void RunState::request_stop()
{
    stop_.store(true);
}

void RunState::wait()
{
    if (supervisor_.joinable()) {
        supervisor_.join();
    }
}

void RunState::set_on_suite_finished(std::function<void(const SuiteResult&)> cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_finished_ = cb;
}

RunSnapshot RunState::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    RunSnapshot snapshot;
    snapshot.started_at = started_at_;
    snapshot.options_desc = options_desc_;
    snapshot.running = running_.load() != 0;
    snapshot.stop_requested = stop_.load();
    const bool live = running_.load() != 0;
    const long long end_ms = live ? current_timestamp_ms() : finished_ms_;
    snapshot.elapsed_seconds =
        (started_ms_ == 0 || end_ms == 0)
            ? 0.0
            : static_cast<double>(end_ms - started_ms_) / 1000.0;
    snapshot.suites = results_;
    for (std::size_t i = 0; i < results_.size(); ++i) {
        const SuiteResult& r = results_[i];
        if (!r.selected) {
            continue;
        }
        ++snapshot.suites_total;
        if (r.state == SuiteState::Passed || r.state == SuiteState::Failed ||
            r.state == SuiteState::Crashed) {
            ++snapshot.suites_finished;
        }
        if (r.state == SuiteState::Passed) {
            ++snapshot.suites_passed;
        } else if (r.state == SuiteState::Failed) {
            ++snapshot.suites_failed;
        } else if (r.state == SuiteState::Crashed) {
            ++snapshot.suites_crashed;
        }
        snapshot.total += r.total;
        snapshot.passed += r.passed;
        snapshot.failed += r.failed;
        snapshot.skipped += r.skipped;
    }
    return snapshot;
}

std::string snapshot_to_json(const RunSnapshot& snapshot)
{
    JsonValue root = JsonValue::object();
    root["started_at"] = snapshot.started_at;
    root["options"] = snapshot.options_desc;
    root["elapsed_seconds"] = snapshot.elapsed_seconds;
    root["running"] = snapshot.running;
    root["stop_requested"] = snapshot.stop_requested;

    JsonValue summary = JsonValue::object();
    summary["suites_total"] = snapshot.suites_total;
    summary["suites_finished"] = snapshot.suites_finished;
    summary["suites_passed"] = snapshot.suites_passed;
    summary["suites_failed"] = snapshot.suites_failed;
    summary["suites_crashed"] = snapshot.suites_crashed;
    summary["cases_total"] = snapshot.total;
    summary["cases_passed"] = snapshot.passed;
    summary["cases_failed"] = snapshot.failed;
    summary["cases_skipped"] = snapshot.skipped;
    root["summary"] = summary;

    JsonValue suites = JsonValue::array();
    for (std::size_t i = 0; i < snapshot.suites.size(); ++i) {
        const SuiteResult& r = snapshot.suites[i];
        JsonValue node = JsonValue::object();
        node["id"] = r.spec.id;
        node["label"] = r.spec.label;
        node["exe"] = r.spec.exe;
        node["selected"] = r.selected;
        node["state"] = suite_state_name(r.state);
        node["exit_code"] = r.exit_code;
        node["timed_out"] = r.timed_out;
        node["seconds"] = r.seconds;
        node["started_at_ms"] = r.started_at_ms;
        node["cases_total"] = r.total;
        node["cases_passed"] = r.passed;
        node["cases_failed"] = r.failed;
        node["cases_skipped"] = r.skipped;
        if (!r.note.empty()) {
            node["note"] = r.note;
        }
        if (!r.output_tail.empty()) {
            node["output_tail"] = r.output_tail;
        }
        JsonValue cases = JsonValue::array();
        for (std::size_t c = 0; c < r.cases.size(); ++c) {
            const CaseResult& item = r.cases[c];
            JsonValue cnode = JsonValue::object();
            cnode["suite"] = item.suite_name;
            cnode["name"] = item.name;
            cnode["full_name"] = item.full_name;
            cnode["status"] = case_status_name(item.status);
            cnode["seconds"] = item.seconds;
            if (!item.failures.empty()) {
                cnode["failures"] = item.failures;
            }
            cases.push_back(cnode);
        }
        node["cases"] = cases;
        suites.push_back(node);
    }
    root["suites"] = suites;
    return to_json_string(root);
}

}  // namespace tr
