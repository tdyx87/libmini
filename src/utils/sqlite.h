#ifndef LIBMINI_SQLITE_H
#define LIBMINI_SQLITE_H

#include <cstdint>
#include <string>
#include <vector>

#include "libmini.h"

struct sqlite3;
struct sqlite3_stmt;

namespace libmini {

// SQLite 封装（嵌入式存储；零配置单文件数据库）。
//
//   SqliteDatabase db("app.db");
//   db.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY,"
//           " name TEXT NOT NULL, score REAL)");
//
//   {
//       SqliteTransaction tx(db);              // BEGIN；析构未提交自动回滚
//       SqliteStatement ins(db,
//           "INSERT INTO users(name, score) VALUES(?, ?)");
//       for (int i = 0; i < 100; ++i) {
//           ins.reset();
//           ins.bind_text(1, "user" + std::to_string(i));
//           ins.bind_double(2, i * 0.5);
//           ins.step();                        // INSERT 无结果集 → Done
//       }
//       tx.commit();                           // 不提交则离开作用域时回滚
//   }
//
//   SqliteStatement st(db,
//       "SELECT id, name FROM users WHERE score >= ? ORDER BY id");
//   st.bind_int(1, 10);
//   while (st.step() == SqliteStatement::StepRow) {
//       int id = st.column_int(0);
//       std::string name = st.column_text(1);
//   }
//
// 设计约定：
//   - 不抛异常：操作失败返回 false / StepError，错误详情经
//     last_status()/error_message() 查询（与库内其他模块的返回式风格一致）；
//   - 文本一律 UTF-8（Windows 下路径与内容均按 UTF-8 处理，中文安全）；
//   - Database 与 Statement 不可拷贝、可移动；Statement 的生存期必须
//     覆盖在 Database 之内（未做运行时校验，由调用方保证）。

// SQLite 错误码（精选常用子集；完整码经 error_message() 文本区分）
enum class SqliteStatus {
    OK = 0,           // 成功
    Error,            // SQL 语法/执行错误（SQLITE_ERROR）
    CannotOpen,       // 文件无法打开（路径不存在/权限/占用）
    Busy,             // 数据库被其他连接锁定（并发写冲突）
    Constraint,       // 约束冲突（UNIQUE/NOT NULL/CHECK/外键）
    Misuse,           // API 误用（未 open/未 prepare/索引越界等）
    Corrupt,          // 数据库文件损坏
    NotADatabase,     // 文件不是 SQLite 数据库（魔数不符）
    ReadOnly,         // 只读连接上尝试写
    Unknown,          // 其他未归类错误
};

// 单元格的带类型值（行遍历/全量读取用；Blob 与 Text 共用 bytes 字段）
struct SqliteValue
{
    enum class Type { Null, Integer, Real, Text, Blob };

    Type type = Type::Null;
    std::int64_t integer = 0;   // Type::Integer
    double real = 0.0;          // Type::Real
    std::string bytes;          // Type::Text（UTF-8）/ Type::Blob（二进制）

    bool is_null() const { return type == Type::Null; }
    // 便捷转换：数值返回数值，文本按 std::stoll/stod 宽松转换，Null → 0
    std::int64_t to_int64() const;
    double to_double() const;
    std::string to_text() const;  // Null → ""；数值按列值格式化
};

// 数据库连接。打开失败不抛异常：is_open() 为 false，
// last_status() == CannotOpen。
class LIBMINI_API SqliteDatabase
{
public:
    // 打开模式（可组合）。默认读写 + 不存在则创建
    enum OpenFlag {
        OpenReadWrite = 0x01,
        OpenReadOnly = 0x02,
        OpenCreate = 0x04,
    };

    SqliteDatabase();
    explicit SqliteDatabase(const std::string& path, int flags = 0);
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    SqliteDatabase(SqliteDatabase&&) noexcept;
    SqliteDatabase& operator=(SqliteDatabase&&) noexcept;

    // path 为 ":memory:" 时打开内存库；flags = 0 表示默认（读写+创建）
    bool open(const std::string& path, int flags = 0);
    void close();
    bool is_open() const { return db_ != nullptr; }

    // 执行无结果集 SQL（DDL/DML/PRAGMA；支持分号分隔的多条语句）
    bool exec(const std::string& sql);

    // 忙等待：写冲突时最多重试等待 ms 毫秒再返回 Busy，默认 0（立即）
    void set_busy_timeout_ms(int ms);

    // 事务三件套（一般用下面的 SqliteTransaction RAII，不用手写）
    bool begin();
    bool commit();
    bool rollback();

    // 最近一次语句的结果
    SqliteStatus last_status() const { return status_; }
    std::string error_message() const;   // 含 SQLite 原生错误文本

    // 最近一次插入的 rowid / 影响的行数
    std::int64_t last_insert_rowid() const;
    int changes() const;

    sqlite3* handle() { return db_; }    // 供 Statement 使用（内部）

private:
    friend class SqliteStatement;
    sqlite3* db_ = nullptr;
    SqliteStatus status_ = SqliteStatus::OK;
    std::string last_msg_;
};

// 预编译语句：参数绑定（? 占位 1-based / :name 命名）、步进、列读取
class LIBMINI_API SqliteStatement
{
public:
    // step() 的结果：有数据行 / 遍历结束 / 出错（详情在 db 上）
    enum StepResult { StepRow, StepDone, StepError };

    SqliteStatement();
    SqliteStatement(SqliteDatabase& db, const std::string& sql);
    ~SqliteStatement();

    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    SqliteStatement(SqliteStatement&&) noexcept;
    SqliteStatement& operator=(SqliteStatement&&) noexcept;

    // 编译 SQL；同一对象可重新 prepare 复用。失败返回 false（db 上查详情）
    bool prepare(SqliteDatabase& db, const std::string& sql);
    bool is_prepared() const { return stmt_ != nullptr; }

    // ------------------ 参数绑定（? 索引从 1 开始）------------------
    // 上次 step 之后未 reset 时，bind 会自动 reset（清执行状态、保留旧绑定）——
    // 复用语句换参重跑、错误后重试都无需手动 reset；边遍历边绑定的罕见
    // 场景会被 reset 终止，属预期行为
    bool bind_null(int index);
    bool bind_int(int index, int value);
    bool bind_int64(int index, std::int64_t value);
    bool bind_double(int index, double value);
    bool bind_text(int index, const std::string& value);
    bool bind_blob(int index, const std::string& value);  // 二进制安全

    // 命名参数：先取 :name 的索引再绑定（SQLITE 未使用该名返回 -1 → Misuse）
    int parameter_index(const std::string& name) const;
    bool bind_null_by_name(const std::string& name);
    bool bind_int_by_name(const std::string& name, int value);
    bool bind_int64_by_name(const std::string& name, std::int64_t value);
    bool bind_double_by_name(const std::string& name, double value);
    bool bind_text_by_name(const std::string& name, const std::string& value);

    // ------------------ 执行与遍历 ------------------
    // SELECT：首次调用执行并返回第一行（StepRow）；再次调用返回后续行，
    // 取尽返回 StepDone。INSERT/UPDATE/DELETE：直接返回 StepDone
    StepResult step();

    // 重置到可重新执行状态（绑定参数保留，可重绑部分参数）
    bool reset();
    // 重置并清空全部绑定（配合 reset 实现同语句批量插入）
    bool reset_and_clear_bindings();

    // ------------------ 列读取（0-based；越界返回默认值 + Misuse）-----
    int column_count() const;
    bool column_is_null(int col) const;
    int column_int(int col) const;
    std::int64_t column_int64(int col) const;
    double column_double(int col) const;
    std::string column_text(int col) const;
    std::string column_blob(int col) const;
    SqliteValue column_value(int col) const;
    std::string column_name(int col) const;

    // 小结果集一次性读取：每行按列值读取（类型保留）。游标位置不变，
    // 内部会 reset 后重新遍历，可在 step() 前后任意时刻调用
    std::vector<std::vector<SqliteValue>> query_all();

private:
    bool check(int rc);                  // SQLite 返回码 → 状态记录 + bool
    void set_misuse(const std::string& what) const;  // 错误落在 owner_ 上

    SqliteDatabase* owner_ = nullptr;    // 指向宿主库（错误记录落在这里）
    sqlite3_stmt* stmt_ = nullptr;
    bool last_step_done_ = false;        // 上次 step 返回 Done（下次 step 自动 reset）
    bool stepped_ = false;               // 自上次 reset 后执行过 step（bind 自动 reset 依据）

    void auto_reset_if_stepped();
};

// 事务 RAII：构造时 BEGIN，析构时未提交则 ROLLBACK。
// 嵌套 BEGIN 会失败（is_active() == false），commit/rollback 幂等。
class LIBMINI_API SqliteTransaction
{
public:
    explicit SqliteTransaction(SqliteDatabase& db);
    ~SqliteTransaction();

    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;

    // 成功后事务结束；再次调用返回 false（幂等，不抛异常）
    bool commit();
    void rollback();
    bool is_active() const { return active_; }

private:
    SqliteDatabase* db_;
    bool active_ = false;
};

}  // namespace libmini

#endif  // LIBMINI_SQLITE_H
