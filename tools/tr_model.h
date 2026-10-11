#ifndef LIBMINI_TOOLS_TR_MODEL_H
#define LIBMINI_TOOLS_TR_MODEL_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 测试聚合器的数据模型：把 test/ 下的各个 gtest 可执行文件当成「套件」，
// 用子进程方式逐个（或并行）跑起来，解析 gtest 的 --gtest_output=json 报告，
// 汇总成统一的进度/结果结构。终端界面与浏览器仪表板都只读 RunState 的快照，
// 因此两套前端可以共用同一份状态，互不干扰。
//
// 依赖库本体（libmini.h 的 run_process / file_utils / json_utils 等），
// 不做任何平台特化——所有平台差异都在 libmini 内部收敛。

namespace tr {

// ---------------- 套件描述 ----------------

// 一个测试套件 = 一个 gtest 可执行文件。exe 由构建系统用生成器表达式注入
// 绝对路径（见 tools/CMakeLists.txt），避免依赖运行时的工作目录。
struct SuiteSpec
{
    std::string id;     // 短名，命令行 --suite 与钩子用（如 "libmini"）
    std::string label;  // 展示名（如 "libmini 基础模块"）
    std::string exe;    // 可执行文件路径
};

// 内置套件表：取自构建时注入的 LIBMINI_TR_SUITE_* 宏
std::vector<SuiteSpec> builtin_suites();

// ---------------- 结果模型 ----------------

enum class CaseStatus
{
    Passed,
    Failed,
    Skipped,
};

enum class SuiteState
{
    Pending,   // 尚未开始
    Running,   // 正在执行
    Passed,    // 跑完且全部通过
    Failed,    // 跑完但有失败用例
    Crashed,   // 未产出报告：崩溃 / 超时 / 报告解析失败
    NotRun,    // 因停止请求而未执行
};

const char* suite_state_name(SuiteState state);
const char* case_status_name(CaseStatus status);

// 单个测试用例（gtest 的 TEST(Suite, Name)）
struct CaseResult
{
    std::string suite_name;  // gtest 套件名（报告里的 classname）
    std::string name;        // 用例名（报告里的 name）
    std::string full_name;   // "suite_name.name"
    CaseStatus status = CaseStatus::Passed;
    double seconds = 0.0;
    std::vector<std::string> failures;  // 失败断言文本（多条）
};

struct SuiteResult
{
    SuiteSpec spec;
    bool selected = false;  // 是否属于本次运行选中的套件
    SuiteState state = SuiteState::Pending;
    int exit_code = -1;
    bool timed_out = false;
    double seconds = 0.0;
    long long started_at_ms = 0;  // 进入 Running 的时刻（Unix 毫秒），供前端算实时耗时
    int total = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<CaseResult> cases;
    std::string note;         // 崩溃/超时/解析失败等诊断文本
    std::string output_tail;  // 子进程输出尾部（诊断用，仅异常时填充）
};

// ---------------- 运行参数 ----------------

struct RunOptions
{
    std::vector<std::string> suites;  // 空 = 全部内置套件（按内置顺序）
    std::string gtest_filter;         // 透传给 --gtest_filter
    int timeout_ms = 900000;          // 单套件超时（毫秒）
    int jobs = 1;                     // 并发运行的套件数
    bool keep_going = true;           // 某套件失败后是否继续跑其余套件
};

// ---------------- 底层操作 ----------------

// 执行 `exe --gtest_list_tests` 列出全部用例全名（"Suite.Case"）。
// 成功返回 true；失败时 error 带诊断。
bool list_tests(const SuiteSpec& spec, std::vector<std::string>* out,
                std::string* error);

// 解析 gtest 的 JSON 报告文本，填进 out 的计数与 cases。成功返回 true。
// 只覆盖 out 的结果字段，不动 out->spec / state。
bool parse_gtest_json(const std::string& text, SuiteResult* out,
                      std::string* error);

// 跑一个套件（同步阻塞）。always 返回结果对象，失败信息在 state/note 里。
SuiteResult run_suite(const SuiteSpec& spec, const RunOptions& opts);

// ---------------- 聚合状态 ----------------

// 给前端消费的只读快照
struct RunSnapshot
{
    std::string started_at;      // 本次运行的起始本地时间
    std::string options_desc;    // 本次运行参数的摘要（展示用）
    double elapsed_seconds = 0.0;
    bool running = false;
    bool stop_requested = false;
    int suites_total = 0;
    int suites_finished = 0;
    int suites_passed = 0;
    int suites_failed = 0;
    int suites_crashed = 0;
    int total = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<SuiteResult> suites;
};

// 快照 → JSON 文本（仪表板的 /api/state 与 --json 报告共用）
std::string snapshot_to_json(const RunSnapshot& snapshot);

// 聚合运行状态：start() 后由后台线程执行套件，任意线程通过 snapshot() 读进度。
// 生命周期内可反复 start()（每次覆盖上一轮结果）。
class RunState
{
public:
    RunState();
    explicit RunState(const std::vector<SuiteSpec>& suites);
    ~RunState();

    RunState(const RunState&) = delete;
    RunState& operator=(const RunState&) = delete;

    const std::vector<SuiteSpec>& specs() const { return specs_; }

    // 线程安全快照（深拷贝；results 未开始过时全部为 Pending）
    RunSnapshot snapshot() const;

    // 启动一轮运行；已在运行返回 false 并写 error
    bool start(const RunOptions& opts, std::string* error = nullptr);

    // 请求停止：当前正在跑的套件会跑完，其余标记为 NotRun
    void request_stop();

    bool running() const { return running_.load() != 0; }

    // 阻塞直到本轮运行结束（未运行时立即返回）
    void wait();

    // 每个套件跑完时回调一次（在后台线程执行，勿做阻塞 IO）
    void set_on_suite_finished(std::function<void(const SuiteResult&)> cb);

private:
    void worker_loop();

    std::vector<SuiteSpec> specs_;
    std::vector<SuiteResult> results_;
    mutable std::mutex mutex_;
    std::atomic<int> running_;
    std::atomic<bool> stop_;
    std::atomic<std::size_t> next_;
    RunOptions opts_;
    std::string started_at_;
    std::string options_desc_;
    long long started_ms_;
    long long finished_ms_;
    std::thread supervisor_;
    std::function<void(const SuiteResult&)> on_finished_;
};

}  // namespace tr

#endif  // LIBMINI_TOOLS_TR_MODEL_H
