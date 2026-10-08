#include "sqlite.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include <spdlog/logger.h>

#include "log_facade.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace libmini {

namespace {

// SQLite 返回码 → 封装状态（SQLITE_ROW/DONE 由 step() 单独处理，不算错误）
SqliteStatus map_status(int rc)
{
    switch (rc) {
        case SQLITE_OK:       return SqliteStatus::OK;
        case SQLITE_CANTOPEN: return SqliteStatus::CannotOpen;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:   return SqliteStatus::Busy;
        case SQLITE_CONSTRAINT:
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_NOTNULL:
        case SQLITE_CONSTRAINT_CHECK:
        case SQLITE_CONSTRAINT_PRIMARYKEY:
        case SQLITE_CONSTRAINT_FOREIGNKEY: return SqliteStatus::Constraint;
        case SQLITE_MISUSE:   return SqliteStatus::Misuse;
        case SQLITE_CORRUPT:  return SqliteStatus::Corrupt;
        case SQLITE_NOTADB:   return SqliteStatus::NotADatabase;
        case SQLITE_READONLY: return SqliteStatus::ReadOnly;
        default:              return SqliteStatus::Error;
    }
}

}  // namespace

// ==================== SqliteValue ====================

std::int64_t SqliteValue::to_int64() const
{
    switch (type) {
        case Type::Integer: return integer;
        case Type::Real:    return static_cast<std::int64_t>(real);
        case Type::Text:
        case Type::Blob: {
            // 宽松转换：取前缀数字（"42abc" → 42），失败返回 0。
            // strtoll 语义与 sscanf("%lld") 一致（前缀解析），且无 MSVC C4996 弃用告警
            return std::strtoll(bytes.c_str(), nullptr, 10);
        }
        default: return 0;
    }
}

double SqliteValue::to_double() const
{
    switch (type) {
        case Type::Integer: return static_cast<double>(integer);
        case Type::Real:    return real;
        case Type::Text:
        case Type::Blob: {
            // strtod 语义与 sscanf("%lf") 一致（前缀解析），且无 MSVC C4996 弃用告警
            return std::strtod(bytes.c_str(), nullptr);
        }
        default: return 0.0;
    }
}

std::string SqliteValue::to_text() const
{
    switch (type) {
        case Type::Null:    return std::string();
        case Type::Integer: return std::to_string(integer);
        case Type::Real: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", real);
            return std::string(buf);
        }
        default: return bytes;
    }
}

// ==================== SqliteDatabase ====================

SqliteDatabase::SqliteDatabase() = default;

SqliteDatabase::SqliteDatabase(const std::string& path, int flags)
{
    open(path, flags);
}

SqliteDatabase::~SqliteDatabase()
{
    close();
}

SqliteDatabase::SqliteDatabase(SqliteDatabase&& other) noexcept
    : db_(other.db_), status_(other.status_), last_msg_(std::move(other.last_msg_))
{
    other.db_ = nullptr;
}

SqliteDatabase& SqliteDatabase::operator=(SqliteDatabase&& other) noexcept
{
    if (this != &other) {
        close();
        db_ = other.db_;
        status_ = other.status_;
        last_msg_ = std::move(other.last_msg_);
        other.db_ = nullptr;
    }
    return *this;
}

static const int DEFAULT_BUSY_TIMEOUT_MS = 5000;

bool SqliteDatabase::open(const std::string& path, int flags)
{
    close();
    if (flags == 0) {
        flags = OpenReadWrite | OpenCreate;
    }
    int sqlite_flags = SQLITE_OPEN_FULLMUTEX;  // 串行模式，跨线程安全
    if (flags & OpenReadOnly) {
        sqlite_flags |= SQLITE_OPEN_READONLY;
    } else {
        sqlite_flags |= SQLITE_OPEN_READWRITE;
    }
    if (flags & OpenCreate) {
        sqlite_flags |= SQLITE_OPEN_CREATE;
    }
    // 路径按 UTF-8（Windows 下 SQLite 内部转 UTF-16，中文路径安全）
    const int rc = ::sqlite3_open_v2(path.c_str(), &db_, sqlite_flags, nullptr);
    if (rc != SQLITE_OK) {
        status_ = map_status(rc);
        last_msg_ = db_ != nullptr ? ::sqlite3_errmsg(db_) : "sqlite3_open failed";
        // 打开失败是生产环境最常见的故障之一，且库本身不抛异常——必须让
        // 统一日志留下痕迹，否则失败会静默传播到调用方。
        if (spdlog::logger* log = LogFacade::logger()) {
            log->error("sqlite: open '{}' failed: {}", path, last_msg_);
        }
        if (db_ != nullptr) {
            ::sqlite3_close(db_);  // 失败路径仍需释放部分构造的句柄
            db_ = nullptr;
        }
        return false;
    }
    // 默认启用 busy 超时：多进程/多线程写场景下，写意向冲突时不立刻返回
    // SQLITE_BUSY，而在有限时间内重试。调用方可通过 set_busy_timeout_ms(0)
    // 关闭此行为（恢复立刻失败语义）。只对写/读写连接设；只读连接不涉及写
    // 冲突，跳过以免无意义调用。
    if (!(flags & OpenReadOnly)) {
        ::sqlite3_busy_timeout(db_, DEFAULT_BUSY_TIMEOUT_MS);
    }
    status_ = SqliteStatus::OK;
    last_msg_.clear();
    return true;
}

void SqliteDatabase::close()
{
    if (db_ != nullptr) {
        // SQLITE_OK 表示所有语句已 finalize；有遗留语句时返回 SQLITE_BUSY，
        // 析构顺序由调用方保证，这里尽力关闭。若未能顺利关闭（例如仍有
        // 未 finalize 的语句），把错误码记到 last_msg_ 以便外部诊断。
        const int rc = ::sqlite3_close(db_);
        db_ = nullptr;
        if (rc != SQLITE_OK) {
            last_msg_ = ::sqlite3_errmsg(nullptr);
            if (last_msg_.empty()) {
                last_msg_ = "sqlite3_close failed";
            }
            // 非 OK 通常意味着仍有未 finalize 的语句占着连接；调用方很难
            // 从返回值发现（close 无返回值），因此记一条告警。
            if (spdlog::logger* log = LogFacade::logger()) {
                log->warn("sqlite: close failed (rc={}): {}", rc, last_msg_);
            }
        }
    }
}

bool SqliteDatabase::exec(const std::string& sql)
{
    if (db_ == nullptr) {
        status_ = SqliteStatus::Misuse;
        last_msg_ = "database is not open";
        return false;
    }
    char* err = nullptr;
    const int rc = ::sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    status_ = map_status(rc);
    if (err != nullptr) {
        last_msg_ = err;
        ::sqlite3_free(err);
    } else if (rc == SQLITE_OK) {
        last_msg_.clear();
    }
    return rc == SQLITE_OK;
}

void SqliteDatabase::set_busy_timeout_ms(int ms)
{
    if (db_ != nullptr) {
        ::sqlite3_busy_timeout(db_, ms);
    }
}

bool SqliteDatabase::begin()
{
    return exec("BEGIN");
}

bool SqliteDatabase::commit()
{
    return exec("COMMIT");
}

bool SqliteDatabase::rollback()
{
    return exec("ROLLBACK");
}

std::string SqliteDatabase::error_message() const
{
    if (db_ != nullptr && status_ != SqliteStatus::OK) {
        const char* msg = ::sqlite3_errmsg(db_);
        if (msg != nullptr && msg[0] != '\0') {
            return msg;
        }
    }
    return last_msg_.empty() ? "no error" : last_msg_;
}

std::int64_t SqliteDatabase::last_insert_rowid() const
{
    return db_ != nullptr ? ::sqlite3_last_insert_rowid(db_) : 0;
}

int SqliteDatabase::changes() const
{
    return db_ != nullptr ? ::sqlite3_changes(db_) : 0;
}

// ==================== SqliteStatement ====================

SqliteStatement::SqliteStatement() = default;

SqliteStatement::SqliteStatement(SqliteDatabase& db, const std::string& sql)
{
    prepare(db, sql);
}

SqliteStatement::~SqliteStatement()
{
    if (stmt_ != nullptr) {
        ::sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept
    : owner_(other.owner_), stmt_(other.stmt_)
{
    other.owner_ = nullptr;
    other.stmt_ = nullptr;
}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other) noexcept
{
    if (this != &other) {
        if (stmt_ != nullptr) {
            ::sqlite3_finalize(stmt_);
        }
        owner_ = other.owner_;
        stmt_ = other.stmt_;
        other.owner_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

bool SqliteStatement::check(int rc)
{
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) {
        if (owner_ != nullptr) {
            owner_->status_ = SqliteStatus::OK;
        }
        return true;
    }
    if (owner_ != nullptr) {
        owner_->status_ = map_status(rc);
        if (owner_->db_ != nullptr) {
            const char* msg = ::sqlite3_errmsg(owner_->db_);
            owner_->last_msg_ = msg != nullptr ? msg : "sqlite error";
        }
    }
    return false;
}

void SqliteStatement::set_misuse(const std::string& what) const
{
    if (owner_ != nullptr) {
        owner_->status_ = SqliteStatus::Misuse;
        owner_->last_msg_ = what;
    }
}

// bind 前语句已执行过（Row/Done/Error）时自动 reset：清除执行状态，
// 保留旧绑定参数。使「同语句重跑换参」「错误后重试」无需手动 reset
void SqliteStatement::auto_reset_if_stepped()
{
    if (stmt_ != nullptr && stepped_) {
        ::sqlite3_reset(stmt_);
        stepped_ = false;
        last_step_done_ = false;
    }
}

bool SqliteStatement::prepare(SqliteDatabase& db, const std::string& sql)
{
    if (stmt_ != nullptr) {
        ::sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
    owner_ = &db;
    if (db.db_ == nullptr) {
        set_misuse("database is not open");
        return false;
    }
    const int rc = ::sqlite3_prepare_v2(db.db_, sql.c_str(),
                                        static_cast<int>(sql.size()), &stmt_,
                                        nullptr);
    if (!check(rc)) {
        return false;
    }
    stepped_ = false;
    last_step_done_ = false;
    return stmt_ != nullptr;
}

bool SqliteStatement::bind_null(int index)
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return false;
    }
    auto_reset_if_stepped();
    return check(::sqlite3_bind_null(stmt_, index));
}

bool SqliteStatement::bind_int(int index, int value)
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return false;
    }
    auto_reset_if_stepped();
    return check(::sqlite3_bind_int(stmt_, index, value));
}

bool SqliteStatement::bind_int64(int index, std::int64_t value)
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return false;
    }
    auto_reset_if_stepped();
    return check(::sqlite3_bind_int64(stmt_, index, value));
}

bool SqliteStatement::bind_double(int index, double value)
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return false;
    }
    auto_reset_if_stepped();
    return check(::sqlite3_bind_double(stmt_, index, value));
}

bool SqliteStatement::bind_text(int index, const std::string& value)
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return false;
    }
    auto_reset_if_stepped();
    // SQLITE_TRANSIENT：SQLite 自行拷贝，调用方字符串可即刻销毁
    return check(::sqlite3_bind_text(stmt_, index, value.c_str(),
                                     static_cast<int>(value.size()),
                                     SQLITE_TRANSIENT));
}

bool SqliteStatement::bind_blob(int index, const std::string& value)
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return false;
    }
    auto_reset_if_stepped();
    return check(::sqlite3_bind_blob(stmt_, index,
                                     value.empty() ? "" : value.c_str(),
                                     static_cast<int>(value.size()),
                                     SQLITE_TRANSIENT));
}

int SqliteStatement::parameter_index(const std::string& name) const
{
    if (stmt_ == nullptr) {
        return -1;
    }
    return ::sqlite3_bind_parameter_index(stmt_, name.c_str());
}

bool SqliteStatement::bind_null_by_name(const std::string& name)
{
    const int idx = parameter_index(name);
    if (idx <= 0) {
        set_misuse("no such parameter: " + name);
        return false;
    }
    return bind_null(idx);
}

bool SqliteStatement::bind_int_by_name(const std::string& name, int value)
{
    const int idx = parameter_index(name);
    if (idx <= 0) {
        set_misuse("no such parameter: " + name);
        return false;
    }
    return bind_int(idx, value);
}

bool SqliteStatement::bind_int64_by_name(const std::string& name,
                                         std::int64_t value)
{
    const int idx = parameter_index(name);
    if (idx <= 0) {
        set_misuse("no such parameter: " + name);
        return false;
    }
    return bind_int64(idx, value);
}

bool SqliteStatement::bind_double_by_name(const std::string& name, double value)
{
    const int idx = parameter_index(name);
    if (idx <= 0) {
        set_misuse("no such parameter: " + name);
        return false;
    }
    return bind_double(idx, value);
}

bool SqliteStatement::bind_text_by_name(const std::string& name,
                                        const std::string& value)
{
    const int idx = parameter_index(name);
    if (idx <= 0) {
        set_misuse("no such parameter: " + name);
        return false;
    }
    return bind_text(idx, value);
}

SqliteStatement::StepResult SqliteStatement::step()
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return StepError;
    }
    // 上次已到 Done：自动 reset，让同一条语句可以再次遍历
    //（绑定参数保留；需要换参数请先重绑）
    if (last_step_done_) {
        ::sqlite3_reset(stmt_);
        last_step_done_ = false;
    }
    const int rc = ::sqlite3_step(stmt_);
    stepped_ = true;
    if (rc == SQLITE_ROW) {
        if (owner_ != nullptr) {
            owner_->status_ = SqliteStatus::OK;
        }
        return StepRow;
    }
    if (rc == SQLITE_DONE) {
        last_step_done_ = true;
        if (owner_ != nullptr) {
            owner_->status_ = SqliteStatus::OK;
        }
        return StepDone;
    }
    last_step_done_ = false;
    check(rc);  // 记录错误详情到 owner
    return StepError;
}

bool SqliteStatement::reset()
{
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return false;
    }
    stepped_ = false;
    last_step_done_ = false;
    return check(::sqlite3_reset(stmt_));
}

bool SqliteStatement::reset_and_clear_bindings()
{
    if (!reset()) {
        return false;
    }
    return check(::sqlite3_clear_bindings(stmt_));
}

int SqliteStatement::column_count() const
{
    return stmt_ != nullptr ? ::sqlite3_column_count(stmt_) : 0;
}

bool SqliteStatement::column_is_null(int col) const
{
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        return true;
    }
    return ::sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int SqliteStatement::column_int(int col) const
{
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        set_misuse("column index out of range");
        return 0;
    }
    return ::sqlite3_column_int(stmt_, col);
}

std::int64_t SqliteStatement::column_int64(int col) const
{
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        set_misuse("column index out of range");
        return 0;
    }
    return ::sqlite3_column_int64(stmt_, col);
}

double SqliteStatement::column_double(int col) const
{
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        set_misuse("column index out of range");
        return 0.0;
    }
    return ::sqlite3_column_double(stmt_, col);
}

std::string SqliteStatement::column_text(int col) const
{
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        set_misuse("column index out of range");
        return std::string();
    }
    const unsigned char* text = ::sqlite3_column_text(stmt_, col);
    if (text == nullptr) {
        return std::string();  // NULL 列
    }
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(::sqlite3_column_bytes(stmt_, col)));
}

std::string SqliteStatement::column_blob(int col) const
{
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        set_misuse("column index out of range");
        return std::string();
    }
    const void* data = ::sqlite3_column_blob(stmt_, col);
    const int n = ::sqlite3_column_bytes(stmt_, col);
    if (data == nullptr || n <= 0) {
        return std::string();
    }
    return std::string(static_cast<const char*>(data), static_cast<std::size_t>(n));
}

SqliteValue SqliteStatement::column_value(int col) const
{
    SqliteValue v;
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        set_misuse("column index out of range");
        return v;
    }
    switch (::sqlite3_column_type(stmt_, col)) {
        case SQLITE_INTEGER:
            v.type = SqliteValue::Type::Integer;
            v.integer = ::sqlite3_column_int64(stmt_, col);
            break;
        case SQLITE_FLOAT:
            v.type = SqliteValue::Type::Real;
            v.real = ::sqlite3_column_double(stmt_, col);
            break;
        case SQLITE_TEXT:
            v.type = SqliteValue::Type::Text;
            v.bytes = column_text(col);
            break;
        case SQLITE_BLOB:
            v.type = SqliteValue::Type::Blob;
            v.bytes = column_blob(col);
            break;
        default:
            v.type = SqliteValue::Type::Null;
            break;
    }
    return v;
}

std::string SqliteStatement::column_name(int col) const
{
    if (stmt_ == nullptr || col < 0 || col >= column_count()) {
        return std::string();
    }
    const char* name = ::sqlite3_column_name(stmt_, col);
    return name != nullptr ? name : std::string();
}

std::vector<std::vector<SqliteValue>> SqliteStatement::query_all()
{
    std::vector<std::vector<SqliteValue>> rows;
    if (stmt_ == nullptr) {
        set_misuse("statement is not prepared");
        return rows;
    }
    ::sqlite3_reset(stmt_);
    last_step_done_ = false;
    stepped_ = false;
    const int n = column_count();
    for (;;) {
        const StepResult s = step();
        if (s != StepRow) {
            break;  // Done 或 Error
        }
        std::vector<SqliteValue> row;
        row.reserve(static_cast<std::size_t>(n));
        for (int c = 0; c < n; ++c) {
            row.push_back(column_value(c));
        }
        rows.push_back(std::move(row));
    }
    ::sqlite3_reset(stmt_);  // 归还干净的游标，调用方可自行 step()
    return rows;
}

// ==================== SqliteTransaction ====================

SqliteTransaction::SqliteTransaction(SqliteDatabase& db) : db_(&db)
{
    // 嵌套 BEGIN 会失败（"cannot start a transaction within a transaction"），
    // 此时 is_active() 为 false，commit/rollback 均为安全空操作
    active_ = db_->begin();
}

SqliteTransaction::~SqliteTransaction()
{
    // 未提交的事务在析构时回滚——异常路径的安全网
    if (active_ && db_ != nullptr) {
        db_->rollback();
    }
}

bool SqliteTransaction::commit()
{
    if (!active_) {
        return false;
    }
    const bool ok = db_->commit();
    active_ = false;  // 失败（如 Busy）时事务仍由 SQLite 维持？——COMMIT
                      // 失败时 SQLite 自动回滚，标记为已结束是安全语义
    return ok;
}

void SqliteTransaction::rollback()
{
    if (!active_) {
        return;
    }
    db_->rollback();
    active_ = false;
}


namespace {
struct WLockHandle {
#ifdef _WIN32
    void* h = nullptr;
#else
    int fd = -1;
#endif
};

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE reinterpret_cast<void*>(-1)
#endif

#ifdef _WIN32
// 锁定的字节范围：1MB（从偏移 0 开始）。同步 LockFile/UnlockFile 必须成对
// 使用同一范围。
static const DWORD WLOCK_LEN = 0x100000;

static void* wlock_create(const std::string& db) {
    const std::wstring w = [] (const std::string& p) {
        if (p.empty()) return std::wstring();
        int n = static_cast<int>(::MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0));
        std::wstring buf(static_cast<std::size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &buf[0], n);
        return buf;
    }(db + ".wlock");
    // 共享读/写模式：允许多个进程打开同一个 .wlock 文件（获得各自的句柄），
    // 但真正的互斥由后续 LockFile 锁定字节范围来保障（系统范围的字节范围锁，
    // 跨进程、跨线程）。OPEN_ALWAYS 语义：文件不存在则创建，存在则打开。
    void* h = ::CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    // 句柄放进堆对象：指针本身就是“创建成功与否”的判据，不再拿平台原始
    // 值（HANDLE / fd）当指针用。
    WLockHandle* handle = new WLockHandle();
    handle->h = h;
    return handle;
}
static void wlock_destroy(void* p) {
    if (p == nullptr) {
        return;
    }
    WLockHandle* handle = static_cast<WLockHandle*>(p);
    if (handle->h != nullptr && handle->h != INVALID_HANDLE_VALUE) {
        HANDLE fh = reinterpret_cast<HANDLE>(handle->h);
        // 防御性解锁（若已释放则忽略），随后关闭句柄
        (void)::UnlockFile(fh, 0, 0, WLOCK_LEN, 0);
        ::CloseHandle(fh);
        handle->h = nullptr;
    }
    delete handle;
}
static bool wlock_try_lock(void* p, int timeout_ms) {
    WLockHandle* handle = static_cast<WLockHandle*>(p);
    if (handle->h == nullptr || handle->h == INVALID_HANDLE_VALUE) {
        return false;
    }
    HANDLE fh = reinterpret_cast<HANDLE>(handle->h);
    // 使用 LockFile（纯同步、无 OVERLAPPED）：锁定文件起始处的 1MB，足以覆盖
    // .wlock 文件（同步 API 不涉及异步事件句柄，避免访问空 hEvent 的 AV）。
    if (timeout_ms <= 0) {
        return ::LockFile(fh, 0, 0, WLOCK_LEN, 0) != 0;
    }
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (::LockFile(fh, 0, 0, WLOCK_LEN, 0) != 0) {
            return true;
        }
        const DWORD err = ::GetLastError();
        // 只有“被别人占着”才值得重试；其它错误立刻失败
        if (err != ERROR_LOCK_VIOLATION && err != ERROR_BUSY) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= static_cast<long long>(timeout_ms)) {
            return false;
        }
    }
}
static void wlock_unlock(void* p) {
    WLockHandle* handle = static_cast<WLockHandle*>(p);
    if (handle->h != nullptr && handle->h != INVALID_HANDLE_VALUE) {
        // 解锁范围与 LockFile 一致（1MB，从偏移 0 开始）
        (void)::UnlockFile(reinterpret_cast<HANDLE>(handle->h), 0, 0, WLOCK_LEN, 0);
    }
}
#else
static void* wlock_create(const std::string& db) {
    const int fd = ::open((db + ".wlock").c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return nullptr;
    }
    // fd == 0 是合法结果（守护进程关掉 stdin 后 open 可能返回 0），因此
    // 不能用 fd 本身当空指针判据——包进堆对象。
    WLockHandle* handle = new WLockHandle();
    handle->fd = fd;
    return handle;
}
static void wlock_destroy(void* p) {
    if (p == nullptr) {
        return;
    }
    WLockHandle* handle = static_cast<WLockHandle*>(p);
    if (handle->fd >= 0) {
        ::close(handle->fd);  // close 会释放该 fd 上持有的 flock
        handle->fd = -1;
    }
    delete handle;
}
static bool wlock_try_lock(void* p, int timeout_ms) {
    WLockHandle* handle = static_cast<WLockHandle*>(p);
    if (handle->fd < 0) {
        return false;
    }
    if (timeout_ms <= 0) {
        return ::flock(handle->fd, LOCK_EX | LOCK_NB) == 0;
    }
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (::flock(handle->fd, LOCK_EX | LOCK_NB) == 0) {
            return true;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= static_cast<long long>(timeout_ms)) {
            return false;
        }
    }
}
static void wlock_unlock(void* p) {
    WLockHandle* handle = static_cast<WLockHandle*>(p);
    if (handle->fd >= 0) {
        ::flock(handle->fd, LOCK_UN);
    }
}
#endif
}  // namespace

// ==================== SqliteWriteMutex ====================
SqliteWriteMutex::SqliteWriteMutex(const std::string& path)
    : path_(path)
{
    impl_ = wlock_create(path);
}

SqliteWriteMutex::~SqliteWriteMutex()
{
    if (impl_ != nullptr) {
        wlock_destroy(impl_);  // 未显式 release 时这里一并解锁 + 关闭句柄
        impl_ = nullptr;
    }
    held_ = false;
}


bool SqliteWriteMutex::acquire(int timeout_ms)
{
    if (impl_ == nullptr) {
        return false;  // 锁文件创建失败（路径不可写 / 目录不存在）
    }
    if (held_) {
        return true;  // 已持有：幂等成功
    }
    held_ = wlock_try_lock(impl_, timeout_ms);
    return held_;
}

void SqliteWriteMutex::release()
{
    // 只解锁、不销毁句柄：释放后仍可再次 acquire（拿到同一个锁文件的锁）。
    // 未持有时是 no-op，重复 release 安全。
    if (impl_ != nullptr && held_) {
        wlock_unlock(impl_);
    }
    held_ = false;
}


bool SqliteWriteMutex::is_held() const
{
    // 只有真正拿到过 OS 锁才算持有——构造出对象本身不算。
    return held_;
}

const std::string& SqliteWriteMutex::path() const
{
    return path_;
}

SqliteWriteMutexGuard::SqliteWriteMutexGuard(const std::string& path, int timeout_ms)
    : mutex_(path)
{
    if (!mutex_.acquire(timeout_ms)) mutex_.release();
}

SqliteWriteMutexGuard::~SqliteWriteMutexGuard()
{
    mutex_.release();
}

bool SqliteWriteMutexGuard::acquired() const
{
    return mutex_.is_held();
}

}  // namespace libmini
