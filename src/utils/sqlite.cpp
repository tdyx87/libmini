#include "sqlite.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>

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
            // 宽松转换：取前缀数字（"42abc" → 42），失败返回 0
            std::int64_t v = 0;
            if (std::sscanf(bytes.c_str(), "%lld",
                            static_cast<long long*>(&v)) == 1) {
                return v;
            }
            return 0;
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
            double v = 0.0;
            if (std::sscanf(bytes.c_str(), "%lf", &v) == 1) {
                return v;
            }
            return 0.0;
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
        if (db_ != nullptr) {
            ::sqlite3_close(db_);  // 失败路径仍需释放部分构造的句柄
            db_ = nullptr;
        }
        return false;
    }
    status_ = SqliteStatus::OK;
    last_msg_.clear();
    return true;
}

void SqliteDatabase::close()
{
    if (db_ != nullptr) {
        // SQLITE_OK 表示所有语句已 finalize；有遗留语句时返回 SQLITE_BUSY，
        // 析构顺序由调用方保证，这里尽力关闭
        ::sqlite3_close(db_);
        db_ = nullptr;
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

}  // namespace libmini
