#ifndef LIBMINI_SQLITE_H
#define LIBMINI_SQLITE_H

#include <cstdint>
#include <string>
#include <vector>

#include "libmini.h"
#include "result.h"   // 统一错误面（Status/Result<T>）；叶子头，无包含时序问题

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
struct LIBMINI_API SqliteValue
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

    // ---------------- 统一错误面（Status / Result） ----------------
    // 上面的 bool + last_status() 接口保持不变（既有调用方零改动）；下面这几个
    // 是同一批操作的 Result 形状，专供跨模块传递错误：分类、原因、出错位置
    // 一次带全，不必调用方逐层转述。SqliteStatus → StatusCode 的映射见
    // sqlite.cpp：Busy→Conflict、Constraint/Misuse→InvalidArgument、
    // Corrupt/NotADatabase→Corrupt、ReadOnly→PermissionDenied、
    // CannotOpen→Io、其余→Failure（原因细节仍在 message 里）。

    // 最近一次失败的统一描述（成功时 Status::success()）；
    // context 为空时记作 "SqliteDatabase"
    Status last_error(const std::string& context = std::string()) const;

    // 与 open()/exec() 一一对应：成功返回 Status::success()，失败返回 classify 后的
    // Status（含路径与出错位置）
    Status try_open(const std::string& path, int flags = 0);
    Status try_exec(const std::string& sql);

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

    // query_all() 的 Result 版本：把「空结果集」与「查询失败」分开——前者是带
    // 空值的成功，后者是错误（分类与原因见 SqliteDatabase::last_error）
    Result<std::vector<std::vector<SqliteValue>>> try_query_all(
        const std::string& context = std::string("SqliteStatement::query_all"));

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

// 跨进程/线程共享数据库文件时的写互斥器。
// SQLite 的文件级锁（WAL + busy）能串行写事务，但两进程同时持有「写意向」
// 时第二个进程会挂在 busy 上直至超时——由调用方决定是否友好。应用程序若希望
// 「至多一个写入事务在库级工作、其他写请求排队/失败」的语义，可在事务外用
// 下面的写互斥器抢占库级写槽；持有写互斥器的线程/进程才允许 BEGIN ... COMMIT。
//
// 用法约定：
//   - 打开库后，只有在成功 acquire(wm, timeout) 之后才对该库执行 BEGIN，
//     写入事务完成（COMMIT/ROLLBACK）后再 release(wm)。acquire/release 的配对
//     由调用方保证；库本身不检查“未持锁时写事务”。
//   - acquire 的超时是阻塞等待时长；拿不到则返回 false，此时不应开始写事务。
//   - 默认每个数据库连接都会启用 busy_timeout（目前 5000ms），所以即便不配
//     写互斥器，SQLite 层的并发写冲突也会在超时内重试而非立即失败。
//     需要立即失败语义时可在该连接上调用 set_busy_timeout_ms(0)。
//
// .wlock 文件语义：
//   - 锁文件是 <path>.wlock，CreateFileW/OPEN_ALWAYS 语义，所以第一次使用时
//     创建，之后持久存在（不会由库自动删除）。这是故意的设计——把“谁曾经
//     拥有过写槽”保留在磁盘上，避免不同进程之间互相误判锁已释放。
//   - 正常析构会释放并关闭 .wlock 句柄，但文件本身留在磁盘上。
//   - 若持有写互斥器的进程异常终止（崩溃/杀掉/断电），OS 会释放 OS 级锁
//     （LockFile），但 .wlock 文件会残留。残留文件不是错误状态——下一个进程
//     仍然可以成功 acquire（该 OS 锁是空的）。不过若实现依赖“文件存在与否”
//     做额外判断时，可能需要手动删掉 <path>.wlock 后再重试。
//   - 跨进程串行依赖的是 OS 级字节范围锁（Windows: LockFile，POSIX: flock），
//     而非“文件是否存在”或“文件是否已被打开”。
class LIBMINI_API SqliteWriteMutex
{
public:
    // path：要保护的共享数据库文件路径（锁文件是 <path>.wlock，持久不删）。
    explicit SqliteWriteMutex(const std::string& path);
    ~SqliteWriteMutex();

    SqliteWriteMutex(const SqliteWriteMutex&) = delete;
    SqliteWriteMutex& operator=(const SqliteWriteMutex&) = delete;

    // 阻塞获取库级写槽——拿到后方可在对应数据库上 BEGIN 写入事务。
    // 返回是否拿到（超时/失败返回 false）。
    bool acquire(int timeout_ms);

    // 释放写槽（事务结束后、commit/rollback 后调用）。
    // - 未持有时为 no-op，重复调用安全。
    // - 只释放 OS 锁，不销毁锁句柄：释放后可以再次 acquire（得到同一个
    //   <path>.wlock 上的锁）。锁句柄在析构时关闭。
    void release();

    // 是否真正持有库级写槽（构造出对象本身不算持有）。
    bool is_held() const;
    const std::string& path() const;

private:
    std::string path_;
    void* impl_ = nullptr;  // 平台锁句柄（内部为堆上的 WLockHandle）
    bool held_ = false;     // 是否已实际取得 OS 锁
};

// RAII 写互斥器：构造时阻塞取得库级写槽，析构自动释放。
class LIBMINI_API SqliteWriteMutexGuard
{
public:
    explicit SqliteWriteMutexGuard(const std::string& path, int timeout_ms);
    ~SqliteWriteMutexGuard();

    SqliteWriteMutexGuard(const SqliteWriteMutexGuard&) = delete;
    SqliteWriteMutexGuard& operator=(const SqliteWriteMutexGuard&) = delete;

    bool acquired() const;

private:
    SqliteWriteMutex mutex_;
};

}  // namespace libmini

#endif  // LIBMINI_SQLITE_H
