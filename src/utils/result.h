#ifndef LIBMINI_RESULT_H
#define LIBMINI_RESULT_H

#include <exception>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace libmini {

// ============================ 统一错误类型 ============================
//
// 为什么要有这个头：库内各模块此前各有一套失败表达——SqliteStatus、
// CharsetStatus、RpcError、SpanStatus，外加一堆 bool + std::string error
// 字段。调用方每换一个模块就要换一套判错习惯，错误也无法跨层传递。
// Status/Result 提供单一形状：失败带「分类 + 消息 + 出错位置」，
// 成功直接带值。C++11 没有 std::expected，故自备一份。
//
// 设计约束（改动前先读）：
//   1. 必须是叶子头文件——只依赖标准库。Status/Result 会被 sqlite.h、
//      http_client.h 等模块头在**任意包含顺序**下使用；若这里反向包含
//      libmini.h/optional.h（本库模块头普遍会 include 伞头），就会出现
//      「伞头还没展开完，模块头就要求 Result 已定义」的时序炸弹。
//   2. 全内联（模板 + 无外部依赖），与 optional.h/scope_guard.h 同类：
//      不引入新的导出符号，DLL 变体的导出面不受影响。
//   3. 不抛异常也不吞异常：Result 只是返回值形状，不需要异常的模块照旧
//      不需要（与库内既有的返回式风格一致）。

// 错误分类：跨模块统一的粗粒度语义。调用方按分类做分支决策
// （重试 / 放弃 / 降级 / 上报），细节永远放在 message 里。
enum class StatusCode {
    Ok = 0,
    InvalidArgument,   // 参数不合法，或 API 误用（未初始化、越界）
    NotFound,          // 目标不存在（文件 / 记录 / 主机）
    AlreadyExists,     // 目标已存在
    PermissionDenied,  // 权限不足或只读
    Conflict,          // 资源被占用 / 并发冲突（busy、locked）
    Unavailable,       // 依赖不可用（连不上、未启用、资源缺失）
    Timeout,           // 超时（含忙等待耗尽）
    Cancelled,         // 调用方取消 / 中断
    Io,                // 读写失败（磁盘、网络字节流）
    Parse,             // 格式 / 协议解析失败
    Corrupt,           // 数据损坏，或不是预期的格式
    Unsupported,       // 当前构建 / 平台不支持该能力
    Internal,          // 库内不变量被破坏（本库的 bug）
    Failure,           // 确有失败但无更精确分类——细节看 message
};

// 分类名（日志/打印用）。全内联，故各 TU 各有一份，不产生导出符号。
inline const char* status_code_name(StatusCode code)
{
    switch (code) {
        case StatusCode::Ok:               return "Ok";
        case StatusCode::InvalidArgument:  return "InvalidArgument";
        case StatusCode::NotFound:         return "NotFound";
        case StatusCode::AlreadyExists:    return "AlreadyExists";
        case StatusCode::PermissionDenied: return "PermissionDenied";
        case StatusCode::Conflict:         return "Conflict";
        case StatusCode::Unavailable:      return "Unavailable";
        case StatusCode::Timeout:          return "Timeout";
        case StatusCode::Cancelled:        return "Cancelled";
        case StatusCode::Io:               return "Io";
        case StatusCode::Parse:            return "Parse";
        case StatusCode::Corrupt:          return "Corrupt";
        case StatusCode::Unsupported:      return "Unsupported";
        case StatusCode::Internal:         return "Internal";
        case StatusCode::Failure:          return "Failure";
    }
    return "Unknown";
}

// 一次失败（或成功）的描述。默认构造 = 成功。
//
//   return Status::not_found("no such table: users")
//       .with_context("SqliteStatement::prepare");
//
// where：
//   code     —— 供机器分支的粗分类；
//   message  —— 人读的原因（含底层库原文，如 SQLite/httplib 的错误文本）；
//   context  —— 「谁在做这件事」，跨层包装时逐层累加。
class Status {
public:
    Status() = default;

    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message))
    {
    }

    Status(StatusCode code, std::string message, std::string context)
        : code_(code), message_(std::move(message)),
          context_(std::move(context))
    {
    }

    // 成功值（默认构造就是它）。不叫 ok() 是因为 ok() 已经是判定用的
    // 成员函数，同名会撞重载
    static Status success() { return Status(); }

    // 常用分类的工厂。都带默认 message，只有 message 说不清时才展开写
    static Status invalid_argument(std::string message)
    {
        return Status(StatusCode::InvalidArgument, std::move(message));
    }
    static Status not_found(std::string message)
    {
        return Status(StatusCode::NotFound, std::move(message));
    }
    static Status already_exists(std::string message)
    {
        return Status(StatusCode::AlreadyExists, std::move(message));
    }
    static Status permission_denied(std::string message)
    {
        return Status(StatusCode::PermissionDenied, std::move(message));
    }
    static Status conflict(std::string message)
    {
        return Status(StatusCode::Conflict, std::move(message));
    }
    static Status unavailable(std::string message)
    {
        return Status(StatusCode::Unavailable, std::move(message));
    }
    static Status timeout(std::string message)
    {
        return Status(StatusCode::Timeout, std::move(message));
    }
    static Status cancelled(std::string message)
    {
        return Status(StatusCode::Cancelled, std::move(message));
    }
    static Status io(std::string message)
    {
        return Status(StatusCode::Io, std::move(message));
    }
    static Status parse(std::string message)
    {
        return Status(StatusCode::Parse, std::move(message));
    }
    static Status corrupt(std::string message)
    {
        return Status(StatusCode::Corrupt, std::move(message));
    }
    static Status unsupported(std::string message)
    {
        return Status(StatusCode::Unsupported, std::move(message));
    }
    static Status internal(std::string message)
    {
        return Status(StatusCode::Internal, std::move(message));
    }
    static Status failure(std::string message)
    {
        return Status(StatusCode::Failure, std::move(message));
    }

    bool ok() const { return code_ == StatusCode::Ok; }
    explicit operator bool() const { return ok(); }

    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }
    const std::string& context() const { return context_; }

    // 追加一层上下文。已有上下文时新层放在更外层，读起来就是调用链：
    //   Status::io("connection reset")
    //       .with_context("HttpClient::send")
    //       .with_context("TaskRunner::run")
    //   → to_string() = "Io: connection reset [TaskRunner::run -> HttpClient::send]"
    Status with_context(std::string context) const
    {
        if (context.empty()) {
            return *this;
        }
        Status out = *this;
        if (out.context_.empty()) {
            out.context_ = std::move(context);
        } else {
            out.context_ = context + " -> " + out.context_;
        }
        return out;
    }

    // "分类: 消息 [上下文]"；成功返回 "Ok"
    std::string to_string() const
    {
        if (ok()) {
            return std::string("Ok");
        }
        std::string out = status_code_name(code_);
        if (!message_.empty()) {
            out += ": ";
            out += message_;
        }
        if (!context_.empty()) {
            out += " [";
            out += context_;
            out += "]";
        }
        return out;
    }

private:
    StatusCode code_ = StatusCode::Ok;
    std::string message_;
    std::string context_;
};

// 在非 Ok 的 Result 上调用 value() 时抛出（与 optional 的 bad_optional_access
// 对称）。what() 即 Status::to_string()，日志里能直接看到原因。
class bad_result_access : public std::exception {
public:
    explicit bad_result_access(std::string what) : what_(std::move(what)) {}

    const char* what() const noexcept override { return what_.c_str(); }

private:
    std::string what_;
};

// ============================ Result<T> ============================
//
// 「值或错误」二选一（对应 C++23 的 std::expected / Rust 的 Result）：
//
//   Result<std::vector<Row>> SqliteStatement::try_query_all();
//
//   Result<std::string> read_config(const std::string& path)
//   {
//       if (!file_exists(path)) {
//           return Status::not_found("no such file: " + path)   // 失败
//               .with_context("read_config");
//       }
//       ...
//       return text;                                            // 成功
//   }
//
//   auto r = read_config("app.json");
//   if (!r) {
//       LOG_ERROR("{}", r.status().to_string());
//       return;
//   }
//   use(*r);                     // 或 r.value() / r->size()
//
// 取值约定与 optional<T> 一致：先判 ok()/operator bool 再取。value() 在不 ok
// 时抛 bad_result_access；operator*/operator-> 不做检查（未定义行为）。
template <typename T>
class Result {
public:
    static_assert(!std::is_void<T>::value,
                  "use Result<void> (or a plain Status) for value-less results");
    static_assert(!std::is_reference<T>::value,
                  "Result<T&> is not supported; store a pointer or a copy");

    // 成功：接受任何能构造 T 的实参（含 T 本身、字符串字面量等），
    // 用 SFINAE 与下面的 Status 构造区分开
    template <typename U,
              typename std::enable_if<
                  !std::is_same<typename std::decay<U>::type, Status>::value &&
                      std::is_constructible<T, U&&>::value,
                  int>::type = 0>
    Result(U&& value) : ok_(true)
    {
        construct(std::forward<U>(value));
    }

    // 失败
    Result(Status status) : ok_(false), status_(std::move(status)) {}

    Result(const Result& other) : ok_(other.ok_), status_(other.status_)
    {
        if (ok_) {
            construct(*other.val());
        }
    }

    Result(Result&& other) : ok_(other.ok_), status_(std::move(other.status_))
    {
        if (ok_) {
            construct(std::move(*other.val()));
            other.reset();
        }
    }

    Result& operator=(const Result& other)
    {
        if (this != &other) {
            assign_from(other);
        }
        return *this;
    }

    Result& operator=(Result&& other)
    {
        if (this != &other) {
            // other.ok_ 必须在 other.reset() 之前取出来：reset() 会把它置回 false
            const bool was_ok = other.ok_;
            reset();
            if (was_ok) {
                construct(std::move(*other.val()));
                other.reset();
            }
            ok_ = was_ok;
            status_ = std::move(other.status_);
        }
        return *this;
    }

    ~Result() { reset(); }

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    // 成功时为 Status::success()（默认构造的 Status）
    const Status& status() const { return status_; }
    StatusCode code() const { return status_.code(); }

    T& value()
    {
        ensure_ok();
        return *val();
    }
    const T& value() const
    {
        ensure_ok();
        return *val();
    }

    T& operator*() { return *val(); }
    const T& operator*() const { return *val(); }
    T* operator->() { return val(); }
    const T* operator->() const { return val(); }

    // 失败时返回 fallback（复制；大对象用 std::move 传参）
    T value_or(T fallback) const
    {
        return ok_ ? *val() : std::move(fallback);
    }

    // 清空为「失败」（Status 保持原样，默认 Ok）
    void reset()
    {
        if (ok_) {
            val()->~T();
            ok_ = false;
        }
    }

private:
    void ensure_ok() const
    {
        if (!ok_) {
            throw bad_result_access(status_.to_string());
        }
    }

    template <typename U>
    void construct(U&& value)
    {
        ::new (static_cast<void*>(&storage_)) T(std::forward<U>(value));
        ok_ = true;
    }

    void assign_from(const Result& other)
    {
        if (other.ok_ && ok_) {
            *val() = *other.val();
        } else if (other.ok_) {
            reset();
            construct(*other.val());
        } else {
            reset();
        }
        ok_ = other.ok_;
        status_ = other.status_;
    }

    T* val() { return static_cast<T*>(static_cast<void*>(&storage_)); }
    const T* val() const
    {
        return static_cast<const T*>(static_cast<const void*>(&storage_));
    }

    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
    bool ok_ = false;
    Status status_;
};

// 无返回值的 Result：等价于 Status，但让「统一用 Result<T> 书写」的泛型代码
// （模板里 T 可能是 void）不必特判。
template <>
class Result<void> {
public:
    Result() = default;
    Result(Status status) : status_(std::move(status)) {}

    bool ok() const { return status_.ok(); }
    explicit operator bool() const { return ok(); }

    const Status& status() const { return status_; }
    StatusCode code() const { return status_.code(); }

    // 保持与 Result<T> 相同的取值约定（不 ok 时抛）
    void value() const
    {
        if (!ok()) {
            throw bad_result_access(status_.to_string());
        }
    }

private:
    Status status_;
};

}  // namespace libmini

#endif  // LIBMINI_RESULT_H
