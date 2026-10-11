// jobsvc —— libmini 应用示例 1：本地任务调度器
//
// 一个常驻小服务：任务定义与运行历史落在 SQLite，按「间隔 / cron / 一次性」计划
// 触发，动作为「跑一个子进程」或「发一个 HTTP 请求」；失败按指数退避重试；
// 管理接口 + Prometheus 指标 + 就绪探针都在同一个 HttpServer 上。
//
// 用法：
//   jobsvc --demo                      自检：临时目录起服务，跑一遍并汇总
//   jobsvc --db jobs.db --port 8780    正常模式（Ctrl+C 优雅退出）
//   jobsvc --seed                      写入示例任务后退出
//
// 计划语法：
//   every:<时长>        如 every:30s / every:5m / every:1h
//   cron:<分 时 日 月 周> 如 cron:*/5 * * * *（支持 * , - / 与标准 OR 语义）
//   once:<本地时间>      如 once:2026-10-11 09:00:00
//
// 任务动作：
//   kind=exec  target=程序路径  args=空格分隔的参数（无 shell，逐参数传递）
//   kind=http  target=基址 URL  args=请求路径（GET）
//
// 用到的 libmini 模块：sqlite(+Result 风格错误)、http_server、http_client、
// process、metrics、log_facade、json_utils、args、console、time_utils、thread_utils。
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>  // 调用 LogFacade::logger()->info() 需要完整类型

#include "app_common.h"
#include "libmini.h"
#include "utils/ui_panels.h"
#include "utils/process.h"

using namespace libmini;
using namespace app;

namespace {

// ================= 计划（schedule）=================

struct CronSpec
{
    std::set<int> minutes;
    std::set<int> hours;
    std::set<int> doms;
    std::set<int> months;
    std::set<int> dows;  // 0 = 周日
    bool dom_restricted = false;
    bool dow_restricted = false;
};

// 解析一个 cron 字段（"*"、"5"、"1,3"、"1-5"、"*/2"、"1-10/3"）展开成允许值集合
bool parse_cron_field(const std::string& text, int lo, int hi, std::set<int>* out,
                      std::string* error)
{
    out->clear();
    const std::vector<std::string> parts = split(text, ',');
    if (parts.empty()) {
        *error = "字段为空";
        return false;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::string range = trim(parts[i]);
        if (range.empty()) {
            *error = "空项";
            return false;
        }
        int step = 1;
        const std::size_t slash = range.find('/');
        if (slash != std::string::npos) {
            const std::string step_text = range.substr(slash + 1);
            range = range.substr(0, slash);
            if (!to_int(step_text, &step) || step <= 0) {
                *error = "步长非法: " + parts[i];
                return false;
            }
        }
        int first = lo;
        int last = hi;
        if (range != "*") {
            const std::size_t dash = range.find('-');
            if (dash == std::string::npos) {
                if (!to_int(range, &first)) {
                    *error = "取值非法: " + range;
                    return false;
                }
                last = first;
            } else {
                if (!to_int(range.substr(0, dash), &first) ||
                    !to_int(range.substr(dash + 1), &last)) {
                    *error = "区间非法: " + range;
                    return false;
                }
            }
        }
        if (first < lo || last > hi || first > last) {
            *error = "取值越界: " + parts[i];
            return false;
        }
        for (int v = first; v <= last; v += step) {
            out->insert(v);
        }
    }
    return !out->empty();
}

// 解析 "分 时 日 月 周" 五段
bool parse_cron(const std::string& text, CronSpec* out, std::string* error)
{
    const std::vector<std::string> fields = split(trim(text), ' ');
    std::vector<std::string> parts;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (!trim(fields[i]).empty()) {
            parts.push_back(trim(fields[i]));
        }
    }
    if (parts.size() != 5) {
        *error = "cron 需要 5 段（分 时 日 月 周），实际 " +
                 lexical_cast<std::string>(parts.size()) + " 段";
        return false;
    }
    if (!parse_cron_field(parts[0], 0, 59, &out->minutes, error) ||
        !parse_cron_field(parts[1], 0, 23, &out->hours, error) ||
        !parse_cron_field(parts[2], 1, 31, &out->doms, error) ||
        !parse_cron_field(parts[3], 1, 12, &out->months, error) ||
        !parse_cron_field(parts[4], 0, 6, &out->dows, error)) {
        return false;
    }
    out->dom_restricted = parts[2] != "*";
    out->dow_restricted = parts[4] != "*";
    return true;
}

struct tm local_tm_of(std::time_t t)
{
    struct tm out;
    std::memset(&out, 0, sizeof(out));
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

// 日/周匹配：两者都被限制时用标准 cron 的 OR 语义
bool cron_day_matches(const CronSpec& spec, const struct tm& t)
{
    const bool dom_ok = spec.doms.count(t.tm_mday) != 0;
    const bool dow_ok = spec.dows.count(t.tm_wday) != 0;
    if (spec.dom_restricted && spec.dow_restricted) {
        return dom_ok || dow_ok;
    }
    return dom_ok && dow_ok;
}

// 从 from_ms 之后找下一次命中（逐分钟扫描，上限一年；找不到返回 0）
std::int64_t cron_next_after(const CronSpec& spec, std::int64_t from_ms)
{
    std::time_t t = static_cast<std::time_t>(from_ms / 1000);
    t -= t % 60;
    t += 60;  // 下一个分钟边界
    const int kMaxMinutes = 366 * 24 * 60;
    for (int i = 0; i < kMaxMinutes; ++i) {
        const struct tm tm_local = local_tm_of(t);
        if (spec.months.count(tm_local.tm_mon + 1) != 0 &&
            spec.hours.count(tm_local.tm_hour) != 0 &&
            spec.minutes.count(tm_local.tm_min) != 0 && cron_day_matches(spec, tm_local)) {
            return static_cast<std::int64_t>(t) * 1000;
        }
        t += 60;
    }
    return 0;
}

struct Schedule
{
    enum class Kind
    {
        Every,
        Cron,
        Once,
    };
    Kind kind = Kind::Every;
    int interval_ms = 60000;
    CronSpec cron;
    std::int64_t at_ms = 0;
    std::string text;
};

bool parse_schedule(const std::string& text, Schedule* out, std::string* error)
{
    const std::string t = trim(text);
    const std::size_t colon = t.find(':');
    if (colon == std::string::npos) {
        *error = "计划需形如 every:30s / cron:*/5 * * * * / once:2026-11-01 09:00:00";
        return false;
    }
    const std::string kind = to_lower(trim(t.substr(0, colon)));
    const std::string rest = trim(t.substr(colon + 1));
    out->text = t;
    if (kind == "every" || kind == "interval") {
        int ms = 0;
        if (!parse_duration_ms(rest, &ms)) {
            *error = "间隔非法（示例 30s / 5m / 1h）";
            return false;
        }
        out->kind = Schedule::Kind::Every;
        out->interval_ms = ms;
        return true;
    }
    if (kind == "cron") {
        out->kind = Schedule::Kind::Cron;
        return parse_cron(rest, &out->cron, error);
    }
    if (kind == "once" || kind == "at") {
        int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
        if (std::sscanf(rest.c_str(), "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6) {
            *error = "一次性时间非法（示例 2026-11-01 09:00:00）";
            return false;
        }
        struct tm tm_at;
        std::memset(&tm_at, 0, sizeof(tm_at));
        tm_at.tm_year = year - 1900;
        tm_at.tm_mon = mon - 1;
        tm_at.tm_mday = day;
        tm_at.tm_hour = hour;
        tm_at.tm_min = min;
        tm_at.tm_sec = sec;
        tm_at.tm_isdst = -1;
        const std::time_t t_at = std::mktime(&tm_at);
        if (t_at <= 0) {
            *error = "一次性时间无法解析";
            return false;
        }
        out->kind = Schedule::Kind::Once;
        out->at_ms = static_cast<std::int64_t>(t_at) * 1000;
        return true;
    }
    *error = "未知计划类型: " + kind;
    return false;
}

// 计算下一次运行时刻；once 已过期返回 0（表示不再运行）
std::int64_t schedule_next(const Schedule& s, std::int64_t from_ms)
{
    switch (s.kind) {
    case Schedule::Kind::Every:
        return from_ms + s.interval_ms;
    case Schedule::Kind::Cron:
        return cron_next_after(s.cron, from_ms);
    case Schedule::Kind::Once:
        return s.at_ms > from_ms ? s.at_ms : 0;
    }
    return 0;
}

std::string ms_to_local_text(std::int64_t ms)
{
    if (ms <= 0) {
        return "-";
    }
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    const struct tm tm_local = local_tm_of(t);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_local);
    return std::string(buffer);
}

// ================= 数据模型 =================

struct Job
{
    std::int64_t id = 0;
    std::string name;
    std::string kind;      // exec | http
    std::string target;    // 程序路径 或 基址 URL
    std::string args;      // 空格分隔参数 或 请求路径
    std::string schedule_text;
    Schedule schedule;
    int timeout_ms = 30000;
    int retries = 0;
    bool enabled = true;
    std::int64_t next_run_ms = 0;
    std::int64_t last_run_ms = 0;
    std::string last_status;
    std::int64_t created_ms = 0;
};

struct RunRecord
{
    std::int64_t id = 0;
    std::int64_t job_id = 0;
    std::string job_name;
    std::int64_t started_ms = 0;
    std::int64_t elapsed_ms = 0;
    int attempt = 1;
    std::string status;  // ok | failed | timeout | error
    int exit_code = 0;
    std::string output;
};

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS jobs("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " name TEXT NOT NULL UNIQUE,"
    " kind TEXT NOT NULL,"
    " target TEXT NOT NULL,"
    " args TEXT NOT NULL DEFAULT '',"
    " schedule TEXT NOT NULL,"
    " timeout_ms INTEGER NOT NULL DEFAULT 30000,"
    " retries INTEGER NOT NULL DEFAULT 0,"
    " enabled INTEGER NOT NULL DEFAULT 1,"
    " next_run_ms INTEGER NOT NULL DEFAULT 0,"
    " last_run_ms INTEGER NOT NULL DEFAULT 0,"
    " last_status TEXT NOT NULL DEFAULT '',"
    " created_ms INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS job_runs("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " job_id INTEGER NOT NULL,"
    " started_ms INTEGER NOT NULL,"
    " elapsed_ms INTEGER NOT NULL,"
    " attempt INTEGER NOT NULL,"
    " status TEXT NOT NULL,"
    " exit_code INTEGER NOT NULL,"
    " output TEXT NOT NULL DEFAULT '');"
    "CREATE INDEX IF NOT EXISTS idx_job_runs_job ON job_runs(job_id, id DESC);";

// SQLite 存储：所有访问串行化（SqliteDatabase 的一个连接不并发使用）
class JobStore
{
public:
    bool open(const std::string& path, std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Status status = db_.try_open(path, SqliteDatabase::OpenReadWrite |
                                               SqliteDatabase::OpenCreate);
        if (!status.ok()) {
            *error = status.to_string();
            return false;
        }
        db_.set_busy_timeout_ms(5000);
        if (!db_.exec(kSchema)) {
            *error = db_.last_error("JobStore::schema").to_string();
            return false;
        }
        return true;
    }

    bool create_job(Job* job, std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "INSERT INTO jobs(name, kind, target, args, schedule, timeout_ms,"
                           " retries, enabled, next_run_ms, created_ms)"
                           " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        if (!st.is_prepared() ||
            !st.bind_text(1, job->name) || !st.bind_text(2, job->kind) ||
            !st.bind_text(3, job->target) || !st.bind_text(4, job->args) ||
            !st.bind_text(5, job->schedule_text) || !st.bind_int(6, job->timeout_ms) ||
            !st.bind_int(7, job->retries) || !st.bind_int(8, job->enabled ? 1 : 0) ||
            !st.bind_int64(9, job->next_run_ms) || !st.bind_int64(10, job->created_ms)) {
            *error = db_.last_error("JobStore::create_job").to_string();
            return false;
        }
        if (st.step() != SqliteStatement::StepDone) {
            *error = db_.last_error("JobStore::create_job").to_string();
            return false;
        }
        job->id = db_.last_insert_rowid();
        return true;
    }

    bool remove_job(std::int64_t id, std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_, "DELETE FROM jobs WHERE id = ?");
        if (!st.is_prepared() || !st.bind_int64(1, id) ||
            st.step() != SqliteStatement::StepDone) {
            *error = db_.last_error("JobStore::remove_job").to_string();
            return false;
        }
        return true;
    }

    bool update_after_run(std::int64_t id, std::int64_t last_run_ms,
                          const std::string& status, std::int64_t next_run_ms, bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "UPDATE jobs SET last_run_ms = ?, last_status = ?,"
                           " next_run_ms = ?, enabled = ? WHERE id = ?");
        if (!st.is_prepared() || !st.bind_int64(1, last_run_ms) ||
            !st.bind_text(2, status) || !st.bind_int64(3, next_run_ms) ||
            !st.bind_int(4, enabled ? 1 : 0) || !st.bind_int64(5, id)) {
            return false;
        }
        return st.step() == SqliteStatement::StepDone;
    }

    bool add_run(const RunRecord& record, std::int64_t* id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "INSERT INTO job_runs(job_id, started_ms, elapsed_ms, attempt,"
                           " status, exit_code, output) VALUES(?, ?, ?, ?, ?, ?, ?)");
        if (!st.is_prepared() || !st.bind_int64(1, record.job_id) ||
            !st.bind_int64(2, record.started_ms) || !st.bind_int64(3, record.elapsed_ms) ||
            !st.bind_int(4, record.attempt) || !st.bind_text(5, record.status) ||
            !st.bind_int(6, record.exit_code) || !st.bind_text(7, record.output)) {
            return false;
        }
        if (st.step() != SqliteStatement::StepDone) {
            return false;
        }
        if (id != nullptr) {
            *id = db_.last_insert_rowid();
        }
        return true;
    }

    std::vector<Job> jobs()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Job> out;
        SqliteStatement st(db_,
                           "SELECT id, name, kind, target, args, schedule, timeout_ms, retries,"
                           " enabled, next_run_ms, last_run_ms, last_status, created_ms"
                           " FROM jobs ORDER BY id");
        if (!st.is_prepared()) {
            return out;
        }
        Result<std::vector<std::vector<SqliteValue> > > rows = st.try_query_all();
        if (!rows.ok()) {
            return out;
        }
        const std::vector<std::vector<SqliteValue> >& data = rows.value();
        for (std::size_t i = 0; i < data.size(); ++i) {
            const std::vector<SqliteValue>& row = data[i];
            if (row.size() < 13) {
                continue;
            }
            Job job;
            job.id = row[0].to_int64();
            job.name = row[1].to_text();
            job.kind = row[2].to_text();
            job.target = row[3].to_text();
            job.args = row[4].to_text();
            job.schedule_text = row[5].to_text();
            job.timeout_ms = static_cast<int>(row[6].to_int64());
            job.retries = static_cast<int>(row[7].to_int64());
            job.enabled = row[8].to_int64() != 0;
            job.next_run_ms = row[9].to_int64();
            job.last_run_ms = row[10].to_int64();
            job.last_status = row[11].to_text();
            job.created_ms = row[12].to_int64();
            std::string ignored;
            parse_schedule(job.schedule_text, &job.schedule, &ignored);
            out.push_back(job);
        }
        return out;
    }

    std::vector<RunRecord> runs(int limit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<RunRecord> out;
        SqliteStatement st(db_,
                           "SELECT r.id, r.job_id, j.name, r.started_ms, r.elapsed_ms,"
                           " r.attempt, r.status, r.exit_code, r.output"
                           " FROM job_runs r LEFT JOIN jobs j ON j.id = r.job_id"
                           " ORDER BY r.id DESC LIMIT ?");
        if (!st.is_prepared() || !st.bind_int(1, limit)) {
            return out;
        }
        while (st.step() == SqliteStatement::StepRow) {
            RunRecord rec;
            rec.id = st.column_int64(0);
            rec.job_id = st.column_int64(1);
            rec.job_name = st.column_text(2);
            rec.started_ms = st.column_int64(3);
            rec.elapsed_ms = st.column_int64(4);
            rec.attempt = st.column_int(5);
            rec.status = st.column_text(6);
            rec.exit_code = st.column_int(7);
            rec.output = st.column_text(8);
            out.push_back(rec);
        }
        return out;
    }

    std::size_t run_count()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_, "SELECT COUNT(*) FROM job_runs");
        if (!st.is_prepared()) {
            return 0;
        }
        Result<std::vector<std::vector<SqliteValue> > > rows = st.try_query_all();
        if (!rows.ok() || rows.value().empty() || rows.value()[0].empty()) {
            return 0;
        }
        return static_cast<std::size_t>(rows.value()[0][0].to_int64());
    }

private:
    SqliteDatabase db_;
    std::mutex mutex_;
};

// ================= 执行器 / 调度器 =================

// 保留文本尾部（运行输出只关心结尾的报错）
std::string tail_of_text(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit) {
        return text;
    }
    return "...[截断]..." + text.substr(text.size() - limit);
}

struct ExecOutcome
{
    bool ok = false;
    bool timed_out = false;
    int exit_code = 0;
    std::string status;  // ok | failed | timeout | error
    std::string output;
};

std::vector<std::string> split_args(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

ExecOutcome execute_exec(const Job& job)
{
    ExecOutcome outcome;
    const ProcessResult result = run_process(job.target, split_args(job.args), job.timeout_ms);
    outcome.exit_code = result.exit_code;
    outcome.timed_out = result.timed_out;
    outcome.output = tail_of_text(result.stdout_text + result.stderr_text, 4000);
    if (result.timed_out) {
        outcome.status = "timeout";
    } else if (result.exit_code == 0) {
        outcome.ok = true;
        outcome.status = "ok";
    } else {
        outcome.status = "failed";
    }
    return outcome;
}

ExecOutcome execute_http(const Job& job)
{
    ExecOutcome outcome;
    HttpClient client(job.target);
    Stopwatch stopwatch;
    const std::string path = job.args.empty() ? std::string("/") : job.args;
    Result<HttpResponse> response = client.try_get(path);
    (void)stopwatch;
    if (!response.ok()) {
        outcome.status = "error";
        outcome.output = response.status().to_string();
        return outcome;
    }
    const HttpResponse& http = response.value();
    outcome.exit_code = http.status;
    if (http.ok()) {
        outcome.ok = true;
        outcome.status = "ok";
    } else {
        outcome.status = "failed";
    }
    outcome.output = "HTTP " + lexical_cast<std::string>(http.status) + " " +
                     tail_of_text(http.body, 2000);
    return outcome;
}

ExecOutcome execute_job(const Job& job)
{
    if (job.kind == "http") {
        return execute_http(job);
    }
    return execute_exec(job);
}

class Scheduler
{
public:
    Scheduler(JobStore* store, MetricRegistry* metrics, int workers)
        : store_(store), metrics_(metrics), workers_(workers < 1 ? 1 : workers),
          stopping_(false), inflight_(0)
    {
        runs_ok_ = metrics_->counter("jobsvc_runs_total{status=\"ok\"}", "任务执行成功次数");
        runs_failed_ = metrics_->counter("jobsvc_runs_total{status=\"failed\"}", "任务执行失败次数");
        runs_timeout_ = metrics_->counter("jobsvc_runs_total{status=\"timeout\"}", "任务超时次数");
        jobs_gauge_ = metrics_->gauge("jobsvc_jobs", "已登记任务数");
        inflight_gauge_ = metrics_->gauge("jobsvc_inflight", "正在执行的任务数");
        duration_ = metrics_->histogram("jobsvc_run_duration_ms", "任务耗时（毫秒）");
    }

    ~Scheduler() { stop(); }

    bool start(std::string* error)
    {
        (void)error;
        stopping_.store(false);
        for (int i = 0; i < workers_; ++i) {
            workers_threads_.push_back(std::thread([this]() { worker_loop(); }));
        }
        dispatch_thread_ = std::thread([this]() { dispatch_loop(); });
        return true;
    }

    void stop()
    {
        if (stopping_.exchange(true)) {
            return;
        }
        queue_cv_.notify_all();
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
        for (std::size_t i = 0; i < workers_threads_.size(); ++i) {
            if (workers_threads_[i].joinable()) {
                workers_threads_[i].join();
            }
        }
        workers_threads_.clear();
    }

    // 手动触发：把任务塞进队列，忽略 enabled 与 next_run（历史照常记录）
    bool trigger(std::int64_t job_id, std::string* error)
    {
        std::vector<Job> jobs = store_->jobs();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].id == job_id) {
                enqueue(jobs[i], true);
                return true;
            }
        }
        *error = "任务不存在: " + lexical_cast<std::string>(job_id);
        return false;
    }

    std::size_t inflight() const { return inflight_.load(); }
    std::size_t queued() const
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queue_.size();
    }

private:
    void enqueue(const Job& job, bool manual)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            Pending pending;
            pending.job = job;
            pending.manual = manual;
            queue_.push_back(pending);
        }
        queue_cv_.notify_one();
    }

    // 调度循环：每 200ms 扫一遍到期任务
    void dispatch_loop()
    {
        while (!stopping_.load()) {
            const std::int64_t now = current_timestamp_ms();
            std::vector<Job> jobs = store_->jobs();
            jobs_gauge_->set(static_cast<double>(jobs.size()));
            for (std::size_t i = 0; i < jobs.size(); ++i) {
                const Job& job = jobs[i];
                if (!job.enabled) {
                    continue;
                }
                if (job.next_run_ms == 0 || job.next_run_ms > now) {
                    continue;
                }
                enqueue(job, false);
                // 先推进下次时间，避免同一任务被反复入队
                const std::int64_t next = schedule_next(job.schedule, now);
                store_->update_after_run(job.id, job.last_run_ms, job.last_status, next,
                                         next != 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    void worker_loop()
    {
        for (;;) {
            Pending pending;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() { return stopping_.load() || !queue_.empty(); });
                if (queue_.empty()) {
                    return;  // 停止且队列已空
                }
                pending = queue_.front();
                queue_.pop_front();
            }
            if (stopping_.load() && !pending.manual) {
                return;
            }
            run_job(pending.job);
        }
    }

    void run_job(const Job& job)
    {
        inflight_.store(inflight_.load() + 1);
        inflight_gauge_->set(static_cast<double>(inflight_.load()));
        const int max_attempts = job.retries + 1;
        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            const std::int64_t started = current_timestamp_ms();
            Stopwatch stopwatch;
            const ExecOutcome outcome = execute_job(job);
            RunRecord record;
            record.job_id = job.id;
            record.started_ms = started;
            record.elapsed_ms = static_cast<std::int64_t>(stopwatch.elapsed_ms());
            record.attempt = attempt;
            record.status = outcome.status;
            record.exit_code = outcome.exit_code;
            record.output = outcome.output;
            store_->add_run(record, nullptr);
            duration_->observe(static_cast<double>(record.elapsed_ms));
            if (outcome.status == "ok") {
                runs_ok_->inc();
            } else if (outcome.status == "timeout") {
                runs_timeout_->inc();
            } else {
                runs_failed_->inc();
            }
            if (outcome.ok) {
                finish_job(job, started, outcome.status);
                inflight_.store(inflight_.load() - 1);
                inflight_gauge_->set(static_cast<double>(inflight_.load()));
                return;
            }
            if (attempt < max_attempts) {
                const int backoff = 200 * (1 << (attempt - 1));  // 200ms / 400ms / 800ms...
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
            } else {
                finish_job(job, started, outcome.status);
            }
        }
        inflight_.store(inflight_.load() - 1);
        inflight_gauge_->set(static_cast<double>(inflight_.load()));
    }

    void finish_job(const Job& job, std::int64_t at_ms, const std::string& status)
    {
        const std::int64_t next = schedule_next(job.schedule, at_ms);
        store_->update_after_run(job.id, at_ms, status, next, next != 0);
    }

    struct Pending
    {
        Job job;
        bool manual = false;
    };

    JobStore* store_;
    MetricRegistry* metrics_;
    int workers_;
    std::atomic<bool> stopping_;
    std::atomic<std::size_t> inflight_;
    std::deque<Pending> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<std::thread> workers_threads_;
    std::thread dispatch_thread_;
    std::shared_ptr<MetricCounter> runs_ok_;
    std::shared_ptr<MetricCounter> runs_failed_;
    std::shared_ptr<MetricCounter> runs_timeout_;
    std::shared_ptr<MetricGauge> jobs_gauge_;
    std::shared_ptr<MetricGauge> inflight_gauge_;
    std::shared_ptr<MetricHistogram> duration_;
};

// ================= HTTP 管理接口 =================

JsonValue job_to_json(const Job& job)
{
    JsonValue node = JsonValue::object();
    node["id"] = job.id;
    node["name"] = job.name;
    node["kind"] = job.kind;
    node["target"] = job.target;
    node["args"] = job.args;
    node["schedule"] = job.schedule_text;
    node["timeout_ms"] = job.timeout_ms;
    node["retries"] = job.retries;
    node["enabled"] = job.enabled;
    node["next_run_ms"] = job.next_run_ms;
    node["next_run_at"] = ms_to_local_text(job.next_run_ms);
    node["last_run_ms"] = job.last_run_ms;
    node["last_run_at"] = ms_to_local_text(job.last_run_ms);
    node["last_status"] = job.last_status;
    return node;
}

JsonValue run_to_json(const RunRecord& record)
{
    JsonValue node = JsonValue::object();
    node["id"] = record.id;
    node["job_id"] = record.job_id;
    node["job"] = record.job_name;
    node["started_at"] = ms_to_local_text(record.started_ms);
    node["elapsed_ms"] = record.elapsed_ms;
    node["attempt"] = record.attempt;
    node["status"] = record.status;
    node["exit_code"] = record.exit_code;
    node["output"] = record.output;
    return node;
}

const char kAdminPage[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<title>jobsvc</title><style>"
    "body{font:14px/1.5 ui-sans-serif,system-ui,'Segoe UI',sans-serif;background:#0e1015;"
    "color:#e7e9ef;margin:24px}h1{font-size:19px;margin:0 0 4px}.dim{color:#8b93a7}"
    "table{border-collapse:collapse;width:100%;margin-top:10px;font-size:13px}"
    "th,td{border-bottom:1px solid #242938;padding:6px 8px;text-align:left}"
    "th{color:#8b93a7;font-weight:500}code{color:#7db4ff}"
    ".ok{color:#3ddc84}.bad{color:#ff5d5d}.warn{color:#ffb454}</style></head><body>"
    "<h1>jobsvc 任务调度器</h1>"
    "<div class=\"dim\" id=\"meta\">加载中…</div>"
    "<h3>任务</h3><div id=\"jobs\"></div>"
    "<h3>最近运行</h3><div id=\"runs\"></div>"
    "<script>"
    "function esc(v){return String(v==null?'':v).replace(/[&<>\"]/g,function(c){"
    "return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c];});}"
    "function cls(s){return s==='ok'?'ok':(s==='timeout'?'warn':'bad');}"
    "function poll(){fetch('/api/state',{cache:'no-store'}).then(function(r){return r.json();})"
    ".then(function(d){"
    "$('meta').textContent='任务 '+d.jobs.length+' 个 · 运行记录 '+d.runs.length+' 条 · 队列 '+"
    "d.queued+' · 执行中 '+d.inflight;"
    "$('jobs').innerHTML='<table><tr><th>id</th><th>名称</th><th>类型</th><th>计划</th>"
    "<th>下次</th><th>上次</th><th>状态</th></tr>'+d.jobs.map(function(j){return '<tr><td>'+"
    "j.id+'</td><td>'+esc(j.name)+'</td><td>'+esc(j.kind)+'</td><td><code>'+esc(j.schedule)+"
    "'</code></td><td class=\"dim\">'+esc(j.next_run_at)+'</td><td class=\"dim\">'+"
    "esc(j.last_run_at)+'</td><td class=\"'+cls(j.last_status)+'\">'+esc(j.last_status)+"
    "'</td></tr>';}).join('')+'</table>';"
    "$('runs').innerHTML='<table><tr><th>任务</th><th>开始</th><th>耗时</th><th>尝试</th>"
    "<th>状态</th><th>输出</th></tr>'+d.runs.map(function(r){return '<tr><td>'+esc(r.job)+"
    "'</td><td class=\"dim\">'+esc(r.started_at)+'</td><td>'+r.elapsed_ms+'ms</td><td>'+"
    "r.attempt+'</td><td class=\"'+cls(r.status)+'\">'+esc(r.status)+'</td><td class=\"dim\">'+"
    "esc((r.output||'').slice(0,90))+'</td></tr>';}).join('')+'</table>';"
    "}).catch(function(e){$('meta').textContent='与进程失联：'+e;});}"
    "function $(id){return document.getElementById(id);}"
    "poll();setInterval(poll,1500);</script></body></html>";

class JobApi
{
public:
    JobApi(JobStore* store, Scheduler* scheduler, MetricRegistry* metrics)
        : store_(store), scheduler_(scheduler), metrics_(metrics)
    {
    }

    void register_routes(HttpServer* server)
    {
        server->get("/", [this](const HttpRequest&) { return index(); });
        server->get("/api/state", [this](const HttpRequest&) { return state(); });
        server->get("/api/jobs", [this](const HttpRequest&) { return jobs(); });
        server->get("/api/runs", [this](const HttpRequest& req) { return runs(req); });
        server->get("/metrics", [this](const HttpRequest&) { return metrics_text(); });
        server->post("/api/jobs", [this](const HttpRequest& req) { return create_job(req); });
        server->post("/api/jobs/:id/run", [this](const HttpRequest& req) { return run_now(req); });
        server->del("/api/jobs/:id", [this](const HttpRequest& req) { return remove_job(req); });
    }

private:
    HttpReply index()
    {
        HttpReply reply = HttpReply::text(200, kAdminPage);
        reply.headers["Content-Type"] = "text/html; charset=utf-8";
        return reply;
    }

    HttpReply state()
    {
        const std::vector<Job> jobs = store_->jobs();
        const std::vector<RunRecord> runs = store_->runs(20);
        JsonValue root = JsonValue::object();
        JsonValue job_array = JsonValue::array();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            job_array.push_back(job_to_json(jobs[i]));
        }
        JsonValue run_array = JsonValue::array();
        for (std::size_t i = 0; i < runs.size(); ++i) {
            run_array.push_back(run_to_json(runs[i]));
        }
        root["jobs"] = job_array;
        root["runs"] = run_array;
        root["inflight"] = static_cast<std::int64_t>(scheduler_->inflight());
        root["queued"] = static_cast<std::int64_t>(scheduler_->queued());
        root["total_runs"] = static_cast<std::int64_t>(store_->run_count());
        return json_ok(root);
    }

    HttpReply jobs()
    {
        const std::vector<Job> jobs = store_->jobs();
        JsonValue array = JsonValue::array();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            array.push_back(job_to_json(jobs[i]));
        }
        JsonValue root = JsonValue::object();
        root["jobs"] = array;
        return json_ok(root);
    }

    HttpReply runs(const HttpRequest& req)
    {
        int limit = 20;
        std::map<std::string, std::string>::const_iterator it = req.query.find("limit");
        if (it != req.query.end()) {
            to_int(it->second, &limit);
        }
        if (limit < 1) {
            limit = 1;
        }
        if (limit > 500) {
            limit = 500;
        }
        const std::vector<RunRecord> records = store_->runs(limit);
        JsonValue array = JsonValue::array();
        for (std::size_t i = 0; i < records.size(); ++i) {
            array.push_back(run_to_json(records[i]));
        }
        JsonValue root = JsonValue::object();
        root["runs"] = array;
        return json_ok(root);
    }

    HttpReply metrics_text()
    {
        HttpReply reply = HttpReply::text(200, metrics_->render_prometheus());
        reply.headers["Content-Type"] = "text/plain; version=0.0.4; charset=utf-8";
        return reply;
    }

    HttpReply create_job(const HttpRequest& req)
    {
        JsonValue body;
        try {
            body = parse_json(req.body.empty() ? std::string("{}") : req.body);
        } catch (const std::exception& e) {
            return json_error(400, std::string("请求体不是合法 JSON: ") + e.what());
        }
        if (!body.is_object()) {
            return json_error(400, "请求体需为 JSON 对象");
        }
        Job job;
        job.name = jstr(body, "name");
        job.kind = to_lower(jstr(body, "kind", "exec"));
        job.target = jstr(body, "target");
        job.args = jstr(body, "args");
        job.schedule_text = jstr(body, "schedule", "every:60s");
        job.timeout_ms = static_cast<int>(jint(body, "timeout_ms", 30000));
        job.retries = static_cast<int>(jint(body, "retries", 0));
        const bool enabled = jbool(body, "enabled", true);
        if (job.name.empty() || job.target.empty()) {
            return json_error(400, "name 与 target 必填");
        }
        if (job.kind != "exec" && job.kind != "http") {
            return json_error(400, "kind 只支持 exec / http");
        }
        std::string error;
        const bool enabled_flag = enabled;
        if (!parse_schedule(job.schedule_text, &job.schedule, &error)) {
            return json_error(400, "计划非法: " + error);
        }
        const std::int64_t now = current_timestamp_ms();
        job.created_ms = now;
        // 0 = 没有下次（once 已过期 / cron 一年内无解）→ 不启用
        job.next_run_ms = schedule_next(job.schedule, now);
        job.enabled = enabled_flag && job.next_run_ms != 0;
        if (job.timeout_ms <= 0) {
            job.timeout_ms = 30000;
        }
        if (job.retries < 0) {
            job.retries = 0;
        }
        if (!store_->create_job(&job, &error)) {
            return json_error(409, "创建任务失败: " + error);
        }
        JsonValue root = JsonValue::object();
        root["job"] = job_to_json(job);
        return json_ok(root, 201);
    }

    HttpReply run_now(const HttpRequest& req)
    {
        int id = 0;
        if (!to_int(req.param("id"), &id)) {
            return json_error(400, "任务 id 非法");
        }
        std::string error;
        if (!scheduler_->trigger(id, &error)) {
            return json_error(404, error);
        }
        return json_ok();
    }

    HttpReply remove_job(const HttpRequest& req)
    {
        int id = 0;
        if (!to_int(req.param("id"), &id)) {
            return json_error(400, "任务 id 非法");
        }
        std::string error;
        if (!store_->remove_job(id, &error)) {
            return json_error(404, error.empty() ? std::string("删除失败") : error);
        }
        return json_ok();
    }

    JobStore* store_;
    Scheduler* scheduler_;
    MetricRegistry* metrics_;
};

// ================= demo =================

void demo_exec_program(std::string* program, std::string* args)
{
#ifdef _WIN32
    *program = "cmd.exe";
    *args = "/C echo tick";
#else
    *program = "echo";
    *args = "tick";
#endif
}

// 用 JSON 写入器拼 POST /api/jobs 的请求体（不手拼字符串）
std::string create_job_body(const std::string& name, const std::string& kind,
                            const std::string& target, const std::string& args,
                            const std::string& schedule, int retries)
{
    JsonValue node = JsonValue::object();
    node["name"] = name;
    node["kind"] = kind;
    node["target"] = target;
    node["args"] = args;
    node["schedule"] = schedule;
    node["retries"] = retries;
    return to_json_string(node);
}

bool wait_for_status(JobStore* store, const std::string& job_name, int min_runs,
                     int timeout_ms)
{
    const std::int64_t deadline = current_timestamp_ms() + timeout_ms;
    while (current_timestamp_ms() < deadline) {
        const std::vector<RunRecord> records = store->runs(200);
        int count = 0;
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (records[i].job_name == job_name) {
                ++count;
            }
        }
        if (count >= min_runs) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

int run_demo()
{
    DemoReport report("jobsvc demo");

    // ---- 1) 计划解析（纯函数）----
    Schedule schedule;
    std::string error;
    report.check(parse_schedule("every:5s", &schedule, &error) &&
                     schedule.interval_ms == 5000,
                 "解析 every:5s");
    report.check(parse_schedule("cron:*/2 * * * *", &schedule, &error), "解析 cron:*/2 * * * *");
    const std::int64_t cron_next = cron_next_after(schedule.cron, current_timestamp_ms());
    report.check(cron_next > 0 && (cron_next / 1000) % 120 == 0,
                 "cron 下一次落在偶数分钟");
    report.check(!parse_schedule("cron:99 * * * *", &schedule, &error), "拒绝越界分钟 99");
    report.check(!parse_schedule("nonsense", &schedule, &error), "拒绝无冒号的计划");

    // ---- 2) 准备存储与调度器 ----
    const std::string workspace = make_workspace("jobsvc_demo");
    report.check(!workspace.empty(), "创建临时工作目录");
    if (workspace.empty()) {
        return report.finish();
    }
    const std::string db_path = workspace_file(workspace, "jobs.db");

    JobStore store;
    if (!store.open(db_path, &error)) {
        report.check(false, "打开 SQLite: " + error);
        remove_tree(workspace);
        return report.finish();
    }
    report.check(true, "打开 SQLite 并建表");

    MetricRegistry metrics;
    Scheduler scheduler(&store, &metrics, 2);
    if (!scheduler.start(&error)) {
        report.check(false, "启动调度器: " + error);
        remove_tree(workspace);
        return report.finish();
    }
    report.check(true, "启动调度器（2 个执行线程）");

    JobApi api(&store, &scheduler, &metrics);
    HttpServer server;
    server.enable_health_endpoints();
    api.register_routes(&server);
    const bool listening = server.start_background(0) && server.wait_until_ready(5000);
    report.check(listening, "管理接口监听成功（自动分配端口）");
    if (!listening) {
        scheduler.stop();
        remove_tree(workspace);
        return report.finish();
    }
    const std::string base = "http://127.0.0.1:" + lexical_cast<std::string>(server.port());
    report.info("管理接口: " + base + "/");

    // ---- 3) 通过 API 建 4 个任务 ----
    HttpClient http(base);
    std::string exec_program;
    std::string exec_args;
    demo_exec_program(&exec_program, &exec_args);

    std::string bodies[4];
    bodies[0] = create_job_body("tick", "exec", exec_program, exec_args, "every:1s", 0);
    bodies[1] = create_job_body("probe", "http", base, "/healthz", "every:1s", 0);
    bodies[2] = create_job_body("minute", "exec", exec_program, exec_args,
                                "cron:*/2 * * * *", 0);
    bodies[3] = create_job_body("doomed", "exec", "no_such_program_libmini_demo", "",
                                "every:1h", 1);
    int created = 0;
    for (int i = 0; i < 4; ++i) {
        Result<HttpResponse> response = http.try_post_json("/api/jobs", bodies[i]);
        if (response.ok() && response.value().status == 201) {
            ++created;
        } else {
            report.info(std::string("创建任务失败: ") +
                        (response.ok() ? response.value().body : response.status().to_string()));
        }
    }
    report.check(created == 4, "通过 POST /api/jobs 建 4 个任务");

    // “doomed” 是 every:1h，不会自动跑；手动触发它以验证 retries=1 → 两次尝试
    std::int64_t doomed_id = 0;
    {
        const std::vector<Job> jobs = store.jobs();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].name == "doomed") {
                doomed_id = jobs[i].id;
            }
        }
    }
    {
        const std::string path = "/api/jobs/" + lexical_cast<std::string>(doomed_id) + "/run";
        Result<HttpResponse> response = http.try_post_json(path, "{}");
        report.check(response.ok() && response.value().status == 200,
                     "手动触发必败任务以验证重试");
    }

    // ---- 4) 等间隔任务各跑一次，并验证重试 ----
    report.check(wait_for_status(&store, "tick", 1, 8000), "exec 任务按期执行");
    report.check(wait_for_status(&store, "probe", 1, 8000), "http 任务按期执行");
    report.check(wait_for_status(&store, "doomed", 2, 8000),
                 "失败任务按 retries=1 重试两次");

    // ---- 5) 手动触发 + 状态接口 ----
    Result<HttpResponse> state_before = http.try_get("/api/state");
    std::int64_t runs_before = 0;
    if (state_before.ok()) {
        try {
            runs_before = parse_json(state_before.value().body)["total_runs"].get<std::int64_t>();
        } catch (const std::exception&) {
        }
    }
    std::int64_t tick_id = 0;
    {
        const std::vector<Job> jobs = store.jobs();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].name == "tick") {
                tick_id = jobs[i].id;
            }
        }
    }
    const std::string run_path = "/api/jobs/" + lexical_cast<std::string>(tick_id) + "/run";
    Result<HttpResponse> triggered = http.try_post_json(run_path, "{}");
    report.check(triggered.ok() && triggered.value().status == 200, "POST 手动触发任务");
    const bool grown = wait_for_status(&store, "tick", 2, 6000);
    report.check(grown, "手动触发产生了新的运行记录");
    (void)runs_before;

    // ---- 6) 状态与指标 ----
    Result<HttpResponse> state_after = http.try_get("/api/state");
    bool state_ok = false;
    if (state_after.ok()) {
        try {
            const JsonValue doc = parse_json(state_after.value().body);
            state_ok = doc["jobs"].is_array() && doc["jobs"].size() == 4 &&
                       doc["runs"].is_array() && doc["runs"].size() >= 3;
        } catch (const std::exception&) {
        }
    }
    report.check(state_ok, "GET /api/state 返回 4 个任务与运行历史");

    Result<HttpResponse> prometheus = http.try_get("/metrics");
    report.check(prometheus.ok() &&
                     prometheus.value().body.find("jobsvc_runs_total") != std::string::npos,
                 "GET /metrics 输出 Prometheus 文本");

    Result<HttpResponse> ready = http.try_get("/readyz");
    report.check(ready.ok() && ready.value().status == 200, "GET /readyz 就绪探针通过");

    // cron 任务的下一次时间应该正好落在偶数分钟（未被自动触发）
    {
        bool minute_ok = false;
        const std::vector<Job> jobs = store.jobs();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].name == "minute") {
                minute_ok = jobs[i].next_run_ms > 0 && (jobs[i].next_run_ms / 1000) % 120 == 0;
            }
        }
        report.check(minute_ok, "cron 任务的下次时间对齐到偶数分钟");
    }

    // 失败任务的最近记录状态应为 failed
    {
        bool doomed_failed = false;
        const std::vector<RunRecord> records = store.runs(200);
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (records[i].job_name == "doomed" && records[i].status == "failed") {
                doomed_failed = true;
            }
        }
        report.check(doomed_failed, "失败任务的运行记录标记为 failed");
    }

    // ---- 7) 清理 ----
    server.stop();
    scheduler.stop();
    report.info("调度器已停止；工作目录 " + workspace);
    remove_tree(workspace);
    return report.finish();
}

// ---------------- 原生窗口模式 ----------------

// `--ui`：把本应用的管理服务拉起来（子进程，与启动器同一套托管机制），再开原生
// 窗口盯任务/运行历史与指标。selftest = true 时跑满固定帧数、截图并打印结论（CI 用）。
int run_ui(const std::string& argv0, const std::string& db_path, const int port,
           const int workers, bool selftest)
{
    ui::AppWindowSpec spec;
    spec.title = "jobsvc 任务调度器";
    spec.subtitle = "任务定义 / 运行历史 / Prometheus 指标";
    spec.port = port;
    spec.child_args.push_back(app::self_exe(argv0));
    spec.child_args.push_back("--db");
    spec.child_args.push_back(db_path);
    spec.child_args.push_back("--port");
    spec.child_args.push_back(lexical_cast<std::string>(port));
    if (workers > 0) {
        spec.child_args.push_back("--workers");
        spec.child_args.push_back(lexical_cast<std::string>(workers));
    }

    spec.panel.json_views.push_back("/api/state");
    spec.panel.json_views.push_back("/api/jobs");
    spec.panel.json_views.push_back("/api/runs");
    spec.panel.metric_filters.push_back("jobsvc");

    if (selftest) {
        spec.frames = 40;
        spec.report = true;
        spec.shot_path = app::workspace_file(app::make_workspace("jobsvc_ui"), "window.bmp");
    }

    std::string error;
    const int code = ui::run_app_window(spec, &error);
    if (code == 2) {
        std::cerr << "无法打开原生窗口（" << error << "），请改用 `jobsvc serve`。\n";
    }
    return code;
}

// ================= 正常模式 =================

int run_service(Args& args)
{
    const std::string db_path = args.get_string("db");
    JobStore store;
    std::string error;
    if (!store.open(db_path, &error)) {
        std::cerr << "打开数据库失败: " << error << "\n";
        return 1;
    }

    MetricRegistry metrics;
    Scheduler scheduler(&store, &metrics, args.get_int("workers"));
    if (!scheduler.start(&error)) {
        std::cerr << "启动调度器失败: " << error << "\n";
        return 1;
    }

    if (args.has_flag("seed")) {
        const std::vector<Job> existing = store.jobs();
        if (!existing.empty()) {
            std::cout << "已存在 " << existing.size() << " 个任务，跳过示例写入\n";
            return 0;
        }
        std::string program;
        std::string cmd_args;
        demo_exec_program(&program, &cmd_args);
        const char* seed_specs[][5] = {
            {"heartbeat", "exec", nullptr, nullptr, "every:30s"},
            {"selfcheck", "http", "http://127.0.0.1:8780", "/healthz", "every:1m"},
            {"nightly", "exec", nullptr, nullptr, "cron:0 3 * * *"},
        };
        for (int i = 0; i < 3; ++i) {
            Job job;
            job.name = seed_specs[i][0];
            job.kind = seed_specs[i][1];
            job.target = seed_specs[i][2] == nullptr ? program : seed_specs[i][2];
            job.args = seed_specs[i][3] == nullptr ? cmd_args : seed_specs[i][3];
            job.schedule_text = seed_specs[i][4];
            job.created_ms = current_timestamp_ms();
            if (!parse_schedule(job.schedule_text, &job.schedule, &error)) {
                std::cerr << "示例计划非法: " << error << "\n";
                return 1;
            }
            job.next_run_ms = schedule_next(job.schedule, job.created_ms);
            if (!store.create_job(&job, &error)) {
                std::cerr << "写入示例任务失败: " << error << "\n";
                return 1;
            }
        }
        std::cout << "已写入 3 个示例任务\n";
        return 0;
    }

    LogFacade::init();
    JobApi api(&store, &scheduler, &metrics);
    HttpServer server;
    server.enable_health_endpoints();
    api.register_routes(&server);
    const int port = args.get_int("port");
    if (!server.start_background(port) || !server.wait_until_ready(5000)) {
        std::cerr << "监听失败: " << server.last_error() << "\n";
        scheduler.stop();
        return 1;
    }

    print_banner("jobsvc", "数据库: " + db_path);
    std::cout << "管理界面: http://127.0.0.1:" << server.port() << "/\n"
              << "指标: http://127.0.0.1:" << server.port() << "/metrics\n"
              << "按 Ctrl+C 优雅退出\n"
              << std::flush;

    ConsoleExit exit_signal;
    exit_signal.wait();
    std::cout << "\n正在停止…\n";
    server.stop();
    scheduler.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args("jobsvc", "1.0",
              "libmini 应用：本地任务调度器（间隔/cron 计划 + 子进程/HTTP 执行器 + 管理 API）");
    args.add_flag("demo", "", "自检模式：临时目录起服务跑一遍并汇总结果");
    args.add_option("db", "", "SQLite 数据库路径", std::string("jobsvc.db"));
    args.add_int("port", "p", "管理接口端口（0 = 自动分配）", 8780);
    args.add_int("workers", "w", "执行线程数", 2);
    args.add_flag("seed", "", "写入示例任务后退出");
    args.add_flag("ui", "", "打开原生窗口（任务面板，Windows）");
    args.add_flag("ui-selftest", "", "窗口自检：限帧渲染 + 截图 + 结论（CI 用）");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }
    if (args.has_flag("demo")) {
        return run_demo();
    }
    if (args.has_flag("ui") || args.has_flag("ui-selftest")) {
        const int port = args.get_int("port") > 0 ? args.get_int("port") : 8780;
        const std::string argv0 = (argc > 0 && argv[0] != 0) ? std::string(argv[0])
                                                             : std::string("jobsvc");
        return run_ui(argv0, args.get_string("db"), port, args.get_int("workers"),
                      args.has_flag("ui-selftest"));
    }
    return run_service(args);
}
