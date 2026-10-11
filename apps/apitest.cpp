// apitest —— libmini 应用示例 4：API 回归与压测台
//
// 用一份 JSON 用例集描述接口回归（方法 / 路径 / 请求体 / 期望状态码 / 期望字段 /
// 响应时延上限），跑完出报告；再可选跑一轮并发压测，给出 RPS 与延迟分位（直接用
// MetricHistogram 的分位估算）。报告可写 JSON、打包成 zip、并与上一次报告做基线
// 对比（新修好的 / 新坏掉的分别列出）——适合放进 CI 当接口回归门禁。
//
// 用法：
//   apitest --demo                                自检：内置靶子服务跑全流程
//   apitest --init cases.json                     生成示例用例集
//   apitest --cases cases.json                    跑回归
//   apitest --cases cases.json --load 2000 -c 8   再跑一轮 2000 请求 / 8 并发压测
//   apitest --cases cases.json --report r.json --zip r.zip --baseline prev.json
//
// 用例集格式：
//   {
//     "name": "smoke",
//     "base_url": "http://127.0.0.1:8080",
//     "cases": [
//       {"name": "健康检查", "method": "GET", "path": "/healthz",
//        "expect_status": 200, "expect_contains": "ok", "max_ms": 500},
//       {"name": "回显", "method": "POST", "path": "/api/echo", "body": "{\"a\":1}",
//        "expect_json": {"a": 1, "path": "a.b"}}
//     ]
//   }
//
// 用到的 libmini 模块：http_client、http_server（demo 靶子）、json_utils、metrics、
// file_utils、zip、args、stopwatch、thread_utils。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_common.h"
#include "libmini.h"
#include "utils/ui_panels.h"

using namespace libmini;
using namespace app;

namespace {

// ---------------- 用例模型 ----------------

struct Case
{
    std::string name;
    std::string method = "GET";
    std::string path = "/";
    std::string body;
    std::string content_type = "application/json";
    int expect_status = -1;  // -1 = 不校验
    std::string expect_contains;
    bool has_expect_json = false;
    JsonValue expect_json;         // 期望的 JSON 片段
    std::string expect_json_path;  // 在响应里的取值路径（"a.b.c"）
    int max_ms = 0;                // 0 = 不校验
};

struct CaseResult
{
    std::string name;
    bool ok = false;
    int status = 0;
    double ms = 0.0;
    std::string failure;
    std::string body_excerpt;
};

struct CaseSet
{
    std::string name = "cases";
    std::string base_url;
    std::vector<Case> cases;
};

bool parse_case_set(const std::string& text, CaseSet* out, std::string* error)
{
    JsonValue doc;
    try {
        doc = parse_json(text);
    } catch (const std::exception& e) {
        *error = std::string("用例集 JSON 非法: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        *error = "用例集根节点需为对象";
        return false;
    }
    out->cases.clear();
    out->name = jstr(doc, "name", "cases");
    out->base_url = jstr(doc, "base_url");
    if (doc.find("cases") == doc.end() || !doc["cases"].is_array()) {
        *error = "用例集缺少 cases 数组";
        return false;
    }
    const JsonValue& array = doc["cases"];
    for (JsonValue::const_iterator it = array.begin(); it != array.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        Case item;
        item.name = jstr(*it, "name");
        item.method = to_upper(jstr(*it, "method", "GET"));
        item.path = jstr(*it, "path", "/");
        item.body = jstr(*it, "body");
        item.content_type = jstr(*it, "content_type", "application/json");
        item.expect_status = static_cast<int>(jint(*it, "expect_status", -1));
        item.expect_contains = jstr(*it, "expect_contains");
        item.max_ms = static_cast<int>(jint(*it, "max_ms", 0));
        if (it->find("expect_json") != it->end()) {
            const JsonValue& expect = (*it)["expect_json"];
            if (expect.is_object() && expect.find("path") != expect.end() &&
                expect.find("value") != expect.end()) {
                item.expect_json_path = jstr(expect, "path");
                item.expect_json = expect["value"];
            } else {
                // 直接比对响应体对象的子集
                item.expect_json_path.clear();
                item.expect_json = expect;
            }
            item.has_expect_json = true;
        }
        if (item.name.empty()) {
            item.name = item.method + " " + item.path;
        }
        out->cases.push_back(item);
    }
    if (out->cases.empty()) {
        *error = "用例集里没有用例";
        return false;
    }
    return true;
}

// 按 "a.b.c" 从响应 JSON 里取值；找不到返回 nullptr
const JsonValue* json_lookup(const JsonValue& root, const std::string& path)
{
    const JsonValue* current = &root;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string key =
            path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!current->is_object() || current->find(key) == current->end()) {
            return nullptr;
        }
        current = &(*current)[key];
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return current;
}

std::string excerpt(const std::string& text, std::size_t limit)
{
    std::string out = text;
    if (out.size() > limit) {
        out = out.substr(0, limit) + "...";
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = ' ';
        }
    }
    return out;
}

// ---------------- 执行 ----------------

CaseResult run_case(HttpClient& http, const Case& test_case)
{
    CaseResult result;
    result.name = test_case.name;
    Stopwatch stopwatch;
    Result<HttpResponse> response =
        http.try_request(test_case.method, test_case.path, test_case.body,
                         test_case.content_type);
    result.ms = stopwatch.elapsed_seconds() * 1000.0;
    if (!response.ok()) {
        result.failure = "请求失败: " + response.status().to_string();
        return result;
    }
    const HttpResponse& http_response = response.value();
    result.status = http_response.status;
    result.body_excerpt = excerpt(http_response.body, 200);

    if (test_case.expect_status >= 0 && http_response.status != test_case.expect_status) {
        result.failure = "状态码 " + lexical_cast<std::string>(http_response.status) +
                         "，期望 " + lexical_cast<std::string>(test_case.expect_status);
        return result;
    }
    if (!test_case.expect_contains.empty() &&
        http_response.body.find(test_case.expect_contains) == std::string::npos) {
        result.failure = "响应体不含 '" + test_case.expect_contains + "'";
        return result;
    }
    if (test_case.has_expect_json) {
        JsonValue body;
        try {
            body = parse_json(http_response.body);
        } catch (const std::exception& e) {
            result.failure = std::string("响应不是合法 JSON: ") + e.what();
            return result;
        }
        if (test_case.expect_json_path.empty()) {
            // 子集断言：期望对象的每个字段都要在响应里原样出现
            if (!test_case.expect_json.is_object() || !body.is_object()) {
                result.failure = "expect_json 子集断言需要两边都是对象";
                return result;
            }
            for (JsonValue::const_iterator it = test_case.expect_json.begin();
                 it != test_case.expect_json.end(); ++it) {
                const std::string key = it.key();
                if (body.find(key) == body.end() || to_json_string(body[key]) != to_json_string(*it)) {
                    result.failure = "字段 " + key + " 期望 " + to_json_string(*it) + "，实际 " +
                                     (body.find(key) == body.end() ? std::string("<缺失>")
                                                                   : to_json_string(body[key]));
                    return result;
                }
            }
        } else {
            const JsonValue* found = json_lookup(body, test_case.expect_json_path);
            if (found == nullptr) {
                result.failure = "响应里找不到 " + test_case.expect_json_path;
                return result;
            }
            if (to_json_string(*found) != to_json_string(test_case.expect_json)) {
                result.failure = test_case.expect_json_path + " 期望 " +
                                 to_json_string(test_case.expect_json) + "，实际 " +
                                 to_json_string(*found);
                return result;
            }
        }
    }
    if (test_case.max_ms > 0 && result.ms > static_cast<double>(test_case.max_ms)) {
        result.failure = "耗时 " + lexical_cast<std::string>(static_cast<int>(result.ms)) +
                         "ms 超过上限 " + lexical_cast<std::string>(test_case.max_ms) + "ms";
        return result;
    }
    result.ok = true;
    return result;
}

struct RunOutcome
{
    std::vector<CaseResult> results;
    std::int64_t elapsed_ms = 0;
    int passed = 0;
    int failed = 0;
};

RunOutcome run_regression(const CaseSet& set, const std::string& base_url)
{
    RunOutcome outcome;
    HttpClient http(base_url);
    http.set_timeout_ms(15000);
    Stopwatch stopwatch;
    for (std::size_t i = 0; i < set.cases.size(); ++i) {
        const CaseResult result = run_case(http, set.cases[i]);
        if (result.ok) {
            ++outcome.passed;
        } else {
            ++outcome.failed;
        }
        outcome.results.push_back(result);
    }
    outcome.elapsed_ms = stopwatch.elapsed_ms();
    return outcome;
}

struct LoadOutcome
{
    int requests = 0;
    int errors = 0;
    double elapsed_ms = 0.0;
    double rps = 0.0;
    double p50 = -1.0;
    double p95 = -1.0;
    double p99 = -1.0;
    bool ran = false;
};

// 并发压测：每个工作线程持有自己的 HttpClient（连接池不跨线程共享）
LoadOutcome run_load(const CaseSet& set, const std::string& base_url, int requests,
                     int concurrency)
{
    LoadOutcome outcome;
    if (requests <= 0) {
        return outcome;
    }
    outcome.ran = true;
    if (concurrency < 1) {
        concurrency = 1;
    }
    if (concurrency > requests) {
        concurrency = requests;
    }
    // 压测用第一条用例；若要压测特定用例，把它放到用例集首位即可
    const Case target = set.cases[0];

    MetricHistogram latency;  // 用内置默认桶
    std::atomic<int> done(0);
    std::atomic<int> errors(0);
    std::mutex mutex;
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(requests));

    Stopwatch stopwatch;
    std::vector<std::thread> workers;
    for (int w = 0; w < concurrency; ++w) {
        workers.push_back(std::thread([&]() {
            HttpClient http(base_url);
            http.set_timeout_ms(15000);
            for (;;) {
                const int index = done.fetch_add(1);
                if (index >= requests) {
                    return;
                }
                const CaseResult result = run_case(http, target);
                latency.observe(result.ms);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    samples.push_back(result.ms);
                }
                if (!result.ok) {
                    errors.fetch_add(1);
                }
            }
        }));
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    outcome.elapsed_ms = static_cast<double>(stopwatch.elapsed_ms());
    outcome.requests = requests;
    outcome.errors = errors.load();
    outcome.rps = outcome.elapsed_ms > 0 ? static_cast<double>(requests) * 1000.0 /
                                               outcome.elapsed_ms
                                         : 0.0;
    outcome.p50 = latency.percentile(50);
    outcome.p95 = latency.percentile(95);
    outcome.p99 = latency.percentile(99);
    return outcome;
}

// ---------------- 报告 ----------------

JsonValue outcome_to_json(const CaseSet& set, const RunOutcome& run,
                          const LoadOutcome& load, const std::string& base_url)
{
    JsonValue root = JsonValue::object();
    root["suite"] = set.name;
    root["base_url"] = base_url;
    root["finished_at"] = current_time_string();
    root["elapsed_ms"] = run.elapsed_ms;
    JsonValue cases = JsonValue::array();
    for (std::size_t i = 0; i < run.results.size(); ++i) {
        const CaseResult& result = run.results[i];
        JsonValue node = JsonValue::object();
        node["name"] = result.name;
        node["ok"] = result.ok;
        node["status"] = result.status;
        node["ms"] = result.ms;
        if (!result.failure.empty()) {
            node["failure"] = result.failure;
            node["body"] = result.body_excerpt;
        }
        cases.push_back(node);
    }
    root["cases"] = cases;
    JsonValue summary = JsonValue::object();
    summary["total"] = static_cast<std::int64_t>(run.results.size());
    summary["passed"] = run.passed;
    summary["failed"] = run.failed;
    root["summary"] = summary;
    if (load.ran) {
        JsonValue node = JsonValue::object();
        node["requests"] = load.requests;
        node["concurrency"] = 0;  // 由调用方补齐
        node["errors"] = load.errors;
        node["elapsed_ms"] = load.elapsed_ms;
        node["rps"] = load.rps;
        node["p50_ms"] = load.p50;
        node["p95_ms"] = load.p95;
        node["p99_ms"] = load.p99;
        root["load"] = node;
    }
    return root;
}

// 基线对比：上一次报告的每个用例 ok 与本次比较
JsonValue diff_baseline(const JsonValue& baseline, const JsonValue& current)
{
    JsonValue root = JsonValue::object();
    JsonValue fixed_list = JsonValue::array();
    JsonValue regressed = JsonValue::array();
    JsonValue unchanged = JsonValue::array();

    std::map<std::string, bool> previous;
    if (baseline.is_object() && baseline.find("cases") != baseline.end() &&
        baseline["cases"].is_array()) {
        const JsonValue& cases = baseline["cases"];
        for (JsonValue::const_iterator it = cases.begin(); it != cases.end(); ++it) {
            if (it->is_object()) {
                previous[jstr(*it, "name")] = jbool(*it, "ok", false);
            }
        }
    }
    if (current.is_object() && current.find("cases") != current.end() &&
        current["cases"].is_array()) {
        const JsonValue& cases = current["cases"];
        for (JsonValue::const_iterator it = cases.begin(); it != cases.end(); ++it) {
            if (!it->is_object()) {
                continue;
            }
            const std::string name = jstr(*it, "name");
            const bool now_ok = jbool(*it, "ok", false);
            std::map<std::string, bool>::const_iterator found = previous.find(name);
            if (found == previous.end()) {
                unchanged.push_back(name + "（基线中无此用例）");
            } else if (!found->second && now_ok) {
                fixed_list.push_back(name);
            } else if (found->second && !now_ok) {
                regressed.push_back(name);
            } else {
                unchanged.push_back(name);
            }
        }
    }
    root["fixed"] = fixed_list;
    root["regressed"] = regressed;
    root["unchanged"] = unchanged;
    return root;
}

void print_report(const CaseSet& set, const RunOutcome& run, const LoadOutcome& load)
{
    std::cout << "\n用例集: " << set.name << "\n";
    for (std::size_t i = 0; i < run.results.size(); ++i) {
        const CaseResult& result = run.results[i];
        std::cout << (result.ok ? "  [通过] " : "  [失败] ") << result.name << "  "
                  << result.status << "  " << lexical_cast<std::string>(
                         static_cast<int>(result.ms))
                  << "ms";
        if (!result.failure.empty()) {
            std::cout << "  " << result.failure;
        }
        std::cout << "\n";
    }
    std::cout << "回归: 通过 " << run.passed << " / 失败 " << run.failed << "，用时 "
              << ms_text(run.elapsed_ms) << "\n";
    if (load.ran) {
        std::cout << "压测: " << load.requests << " 请求 " << lexical_cast<std::string>(
                         static_cast<int>(load.rps))
                  << " rps，错误 " << load.errors << "，p50 "
                  << lexical_cast<std::string>(static_cast<int>(load.p50)) << "ms，p95 "
                  << lexical_cast<std::string>(static_cast<int>(load.p95)) << "ms，p99 "
                  << lexical_cast<std::string>(static_cast<int>(load.p99)) << "ms\n";
    }
    std::cout.flush();
}

const char* kSampleCaseSet =
    "{\n"
    "  \"name\": \"sample\",\n"
    "  \"base_url\": \"http://127.0.0.1:8080\",\n"
    "  \"cases\": [\n"
    "    {\"name\": \"健康检查\", \"method\": \"GET\", \"path\": \"/healthz\",\n"
    "     \"expect_status\": 200, \"expect_contains\": \"ok\", \"max_ms\": 500},\n"
    "    {\"name\": \"就绪探针\", \"method\": \"GET\", \"path\": \"/readyz\",\n"
    "     \"expect_status\": 200},\n"
    "    {\"name\": \"回显\", \"method\": \"POST\", \"path\": \"/api/echo\",\n"
    "     \"body\": \"{\\\"a\\\":1}\", \"expect_json\": {\"path\": \"a\", \"value\": 1}}\n"
    "  ]\n"
    "}\n";

// ---------------- demo（自带靶子服务） ----------------

int run_demo()
{
    DemoReport report("apitest demo");

    // 1) 用例集解析
    CaseSet parsed;
    std::string error;
    report.check(parse_case_set(kSampleCaseSet, &parsed, &error), "解析示例用例集");
    report.check(parsed.cases.size() == 3, "示例用例集含 3 条用例");
    report.check(!parse_case_set("{\"cases\":[]}", &parsed, &error), "拒绝空用例集");
    report.check(!parse_case_set("not json", &parsed, &error), "拒绝非法 JSON");

    // 2) 起一个内置靶子服务
    HttpServer server;
    server.enable_health_endpoints();
    server.post("/api/echo", [](const HttpRequest& req) {
        JsonValue body = JsonValue::object();
        try {
            const JsonValue parsed_body = parse_json(req.body.empty() ? "{}" : req.body);
            for (JsonValue::const_iterator it = parsed_body.begin(); it != parsed_body.end(); ++it) {
                body[it.key()] = *it;
            }
        } catch (const std::exception&) {
        }
        body["echoed"] = true;
        return HttpReply::json(200, to_json_string(body));
    });
    server.get("/slow", [](const HttpRequest&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return HttpReply::text(200, "slow ok");
    });
    const bool listening = server.start_background(0) && server.wait_until_ready(5000);
    report.check(listening, "启动内置靶子服务（自动分配端口）");
    if (!listening) {
        return report.finish();
    }
    const std::string base = "http://127.0.0.1:" + lexical_cast<std::string>(server.port());
    report.info("靶子服务: " + base);

    // 3) 组装一份「3 条会过 + 1 条故意错」的用例集
    CaseSet set;
    set.name = "demo";
    set.base_url = base;
    Case healthy;
    healthy.name = "健康检查";
    healthy.path = "/healthz";
    healthy.expect_status = 200;
    healthy.expect_contains = "ok";
    healthy.max_ms = 2000;
    set.cases.push_back(healthy);

    Case echo;
    echo.name = "回显字段";
    echo.method = "POST";
    echo.path = "/api/echo";
    echo.body = "{\"a\":1,\"b\":\"x\"}";
    echo.expect_status = 200;
    echo.has_expect_json = true;
    echo.expect_json_path = "a";
    echo.expect_json = 1;
    set.cases.push_back(echo);

    Case subset;
    subset.name = "子集断言";
    subset.method = "POST";
    subset.path = "/api/echo";
    subset.body = "{\"a\":7,\"b\":\"y\"}";
    subset.expect_status = 200;
    subset.has_expect_json = true;
    subset.expect_json = JsonValue::object();
    subset.expect_json["a"] = 7;
    subset.expect_json["echoed"] = true;
    set.cases.push_back(subset);

    Case slowed;
    slowed.name = "时延上限";
    slowed.path = "/slow";
    slowed.expect_status = 200;
    slowed.max_ms = 20;  // 靶子是 120ms，必然超限
    set.cases.push_back(slowed);

    Case wrong_status;
    wrong_status.name = "故意错的状态码";
    wrong_status.path = "/healthz";
    wrong_status.expect_status = 418;
    set.cases.push_back(wrong_status);

    const RunOutcome run = run_regression(set, base);
    report.check(run.results.size() == 5, "回归跑完 5 条用例");
    report.check(run.passed == 3 && run.failed == 2, "3 条通过、2 条按预期失败");

    bool failure_text_ok = false;
    for (std::size_t i = 0; i < run.results.size(); ++i) {
        if (run.results[i].name == "时延上限" &&
            run.results[i].failure.find("超过上限") != std::string::npos) {
            failure_text_ok = true;
        }
    }
    report.check(failure_text_ok, "时延超限给出可读原因");

    bool json_expect_ok = false;
    for (std::size_t i = 0; i < run.results.size(); ++i) {
        if (run.results[i].name == "子集断言" && run.results[i].ok) {
            json_expect_ok = true;
        }
    }
    report.check(json_expect_ok, "JSON 子集断言通过");

    // 4) 压测：靶子 /healthz，200 请求 / 8 并发
    LoadOutcome load = run_load(set, base, 200, 8);
    report.check(load.ran && load.requests == 200, "压测发满 200 个请求");
    report.check(load.errors == 0, "压测无错误响应");
    report.check(load.rps > 0.0 && load.p50 >= 0.0 && load.p95 >= load.p50,
                 "给出 RPS 与延迟分位（p95 >= p50）");
    report.info("压测: " + lexical_cast<std::string>(static_cast<int>(load.rps)) +
                " rps, p95 " + lexical_cast<std::string>(static_cast<int>(load.p95)) + "ms");

    // 5) 报告与基线
    const std::string workspace = make_workspace("apitest_demo");
    report.check(!workspace.empty(), "创建临时工作目录");
    if (workspace.empty()) {
        server.stop();
        return report.finish();
    }
    JsonValue root = outcome_to_json(set, run, load, base);
    if (root.find("load") != root.end()) {
        root["load"]["concurrency"] = 8;
    }
    JsonValue diff = diff_baseline(root, root);
    root["baseline_diff"] = diff;
    const std::string report_json = to_json_string(root);

    const std::string report_path = workspace_file(workspace, "report.json");
    report.check(write_file(report_path, report_json), "写出 JSON 报告");
    report.check(read_file(report_path) == report_json, "报告可原样读回");

    // 自身对自身的基线对比：既没有修复也没有回归
    report.check(diff["fixed"].empty() && diff["regressed"].empty() &&
                     diff["unchanged"].size() == 5,
                 "基线自比对：0 修复 0 回归 5 未变");

    // 造一份「上一次全绿」的基线，本次应有 2 条回归
    JsonValue previous = parse_json(report_json);
    for (JsonValue::iterator it = previous["cases"].begin(); it != previous["cases"].end(); ++it) {
        (*it)["ok"] = true;
    }
    const JsonValue regressed = diff_baseline(previous, root);
    report.check(regressed["regressed"].size() == 2 && regressed["fixed"].empty(),
                 "对比基线能识别出 2 条新回归");

    ZipWriter writer;
    writer.add_file("report.json", report_json);
    const std::string zip_bytes = writer.finish();
    const std::string zip_path = workspace_file(workspace, "report.zip");
    report.check(write_file(zip_path, zip_bytes), "报告打包成 zip");
    ZipReader reader;
    report.check(reader.open(read_file(zip_path)) && reader.contains("report.json") &&
                     reader.extract("report.json") == report_json,
                 "zip 内报告内容一致");

    server.stop();
    remove_tree(workspace);
    return report.finish();
}

// ---------------- 原生窗口模式 ----------------

// `--ui`：本应用是一次性工具（没有常驻服务面），因此窗口是「运行器」形状：
// 拉起 `apitest --demo` 子进程，实时展示断言进展与结果，并可重跑/停止。
int run_ui(const std::string& argv0, const std::vector<std::string>& extra_args, bool selftest)
{
    ui::AppWindowSpec spec;
    spec.title = "apitest 回归与压测台";
    spec.subtitle = "用例断言 / 并发压测 / 报告（窗口内运行的是 --demo 全流程）";
    spec.port = 0;  // 运行器视图
    spec.child_args.push_back(app::self_exe(argv0));

    bool has_demo = false;
    for (std::size_t i = 0; i < extra_args.size(); ++i) {
        spec.child_args.push_back(extra_args[i]);
        if (extra_args[i] == "--demo") {
            has_demo = true;
        }
    }
    if (!has_demo) {
        spec.child_args.push_back("--demo");
    }

    if (selftest) {
        spec.frames = 40;
        spec.report = true;
        spec.shot_path = app::workspace_file(app::make_workspace("apitest_ui"), "window.bmp");
    }

    std::string error;
    const int code = ui::run_app_window(spec, &error);
    if (code == 2) {
        std::cerr << "无法打开原生窗口（" << error << "），请直接用命令行模式。\n";
    }
    return code;
}

// ---------------- 正常模式 ----------------

int run_cli(Args& args)
{
    const std::string cases_path = args.get_string("cases");
    if (cases_path.empty()) {
        std::cerr << "需要 --cases <用例集.json>（或用 --demo / --init）\n";
        return 2;
    }
    const std::string text = read_file(cases_path);
    if (text.empty()) {
        std::cerr << "读不到用例集: " << cases_path << "\n";
        return 2;
    }
    CaseSet set;
    std::string error;
    if (!parse_case_set(text, &set, &error)) {
        std::cerr << "用例集无效: " << error << "\n";
        return 2;
    }
    std::string base_url = trim(args.get_string("base-url"));
    if (base_url.empty()) {
        base_url = set.base_url;
    }
    if (base_url.empty()) {
        std::cerr << "用例集里没有 base_url，请用 --base-url 指定\n";
        return 2;
    }

    const RunOutcome run = run_regression(set, base_url);
    const LoadOutcome load =
        run_load(set, base_url, args.get_int("load"), args.get_int("concurrency"));
    print_report(set, run, load);

    JsonValue root = outcome_to_json(set, run, load, base_url);
    if (load.ran) {
        root["load"]["concurrency"] = args.get_int("concurrency");
    }

    const std::string baseline_path = trim(args.get_string("baseline"));
    bool regression_vs_baseline = false;
    if (!baseline_path.empty()) {
        const std::string previous_text = read_file(baseline_path);
        if (previous_text.empty()) {
            std::cerr << "读不到基线报告: " << baseline_path << "\n";
            return 2;
        }
        JsonValue previous;
        try {
            previous = parse_json(previous_text);
        } catch (const std::exception& e) {
            std::cerr << "基线报告非法: " << e.what() << "\n";
            return 2;
        }
        const JsonValue diff = diff_baseline(previous, root);
        root["baseline_diff"] = diff;
        regression_vs_baseline = !diff["regressed"].empty();
        std::cout << "基线对比: 修复 " << diff["fixed"].size() << " 条，回归 "
                  << diff["regressed"].size() << " 条\n";
        for (std::size_t i = 0; i < diff["regressed"].size(); ++i) {
            std::cout << "  回归: " << diff["regressed"][i].get<std::string>() << "\n";
        }
    }

    const std::string report_path = trim(args.get_string("report"));
    const std::string report_json = to_json_string(root);
    if (!report_path.empty() && !write_file(report_path, report_json)) {
        std::cerr << "写报告失败: " << report_path << "\n";
        return 2;
    }
    const std::string zip_path = trim(args.get_string("zip"));
    if (!zip_path.empty()) {
        ZipWriter writer;
        writer.add_file("report.json", report_json);
        if (!write_file(zip_path, writer.finish())) {
            std::cerr << "写 zip 失败: " << zip_path << "\n";
            return 2;
        }
        std::cout << "报告归档: " << zip_path << "\n";
    }
    if (!report_path.empty()) {
        std::cout << "报告: " << report_path << "\n";
    }
    if (run.failed > 0 || regression_vs_baseline) {
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args("apitest", "1.0",
              "libmini 应用：API 回归与压测台（用例断言 + 并发压测分位 + 报告/基线对比）");
    args.add_flag("demo", "", "自检模式：内置靶子服务跑全流程");
    args.add_option("cases", "c", "用例集 JSON 路径", std::string(""));
    args.add_option("base-url", "b", "覆盖用例集里的 base_url", std::string(""));
    args.add_option("init", "", "写出示例用例集到该路径后退出", std::string(""));
    args.add_int("load", "l", "压测请求数（0 = 不压测）", 0);
    args.add_int("concurrency", "", "压测并发数", 4);
    args.add_option("report", "r", "把报告写到该路径（JSON）", std::string(""));
    args.add_option("zip", "", "把报告打包到该路径（zip）", std::string(""));
    args.add_option("baseline", "", "上次报告路径：对比出修复/回归", std::string(""));
    args.add_flag("ui", "", "打开原生窗口（运行器视图，Windows）");
    args.add_flag("ui-selftest", "", "窗口自检：限帧渲染 + 截图 + 结论（CI 用）");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }
    if (args.has_flag("demo")) {
        return run_demo();
    }
    if (args.has_flag("ui") || args.has_flag("ui-selftest")) {
        const std::string argv0 = (argc > 0 && argv[0] != 0) ? std::string(argv[0])
                                                             : std::string("apitest");
        // 把用例集/靶子地址等透传给子进程，让窗口里跑的就是同一套配置
        std::vector<std::string> passthrough;
        const std::string cases = trim(args.get_string("cases"));
        if (!cases.empty()) {
            passthrough.push_back("--cases");
            passthrough.push_back(cases);
        }
        const std::string base_url = trim(args.get_string("base-url"));
        if (!base_url.empty()) {
            passthrough.push_back("--base-url");
            passthrough.push_back(base_url);
        }
        return run_ui(argv0, passthrough, args.has_flag("ui-selftest"));
    }
    const std::string init_path = trim(args.get_string("init"));
    if (!init_path.empty()) {
        if (!write_file(init_path, kSampleCaseSet)) {
            std::cerr << "写入示例用例集失败: " << init_path << "\n";
            return 2;
        }
        std::cout << "已写出示例用例集: " << init_path << "\n";
        return 0;
    }
    return run_cli(args);
}
