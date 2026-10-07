#ifndef LIBMINI_TRACE_H
#define LIBMINI_TRACE_H

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "export.h"

namespace libmini {

struct TracerImpl;  // 内部实现（trace.cpp）；Span 经 shared_ptr 持有，Tracer
                    // 先析构也不会让在途 span 悬垂

// W3C Trace Context 上下文：trace_id（32 位十六进制）/ span_id（16 位十六进制）
// / 采样标志。经 traceparent() 序列化后可放进 HTTP 头跨进程传播，
// parse_traceparent() 解析对端注入的头值
struct LIBMINI_API TraceContext {
    std::string trace_id;
    std::string span_id;
    bool sampled = true;

    // id 为合法十六进制且非全零
    bool valid() const;

    // "00-<trace_id>-<span_id>-<00|01>"；无效上下文返回空串
    std::string traceparent() const;

    // 解析 traceparent 头值；任何不合规范的输入返回 valid()==false 的上下文
    static TraceContext parse_traceparent(const std::string& value);
};

// span 终态：Unset（未显式设置）/ Ok / Error
enum class SpanStatus {
    Unset,
    Ok,
    Error,
};

// span 时间线事件（add_event 记录，epoch 毫秒）
struct SpanEvent {
    std::string name;
    std::int64_t at_ms = 0;
};

// 结束后的不可变快照：Tracer::finished()/finished_json() 的导出单元。
// attributes 保插入序，同键只留最后一次 set_attribute 的值
struct SpanSnapshot {
    std::string service;
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;  // 空 = 根 span
    std::string name;
    SpanStatus status = SpanStatus::Unset;
    std::string status_message;
    std::int64_t start_ms = 0;     // epoch 毫秒（墙上时钟，便于跨机对齐）
    std::int64_t end_ms = 0;
    std::int64_t duration_ms = 0;  // steady_clock 计时，不受系统校时跳变影响
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<SpanEvent> events;
};

// 一次调用的执行单元。
//
// 线程约定：单个 span 由单个线程驱动（属性/事件/结束不做同步，与主流 tracing
// 实现一致）；Tracer 的收集、finished()/finished_json() 在多线程下安全。
// end() 幂等——首次调用冻结快照交给 Tracer，其后的属性修改不再进入快照；
// 未 end() 的 span 不会被收集（SpanScope 析构自动补 end）。
class LIBMINI_API Span {
public:
    const std::string& name() const;
    const TraceContext& context() const;       // span_id 是自己的 id
    const std::string& parent_span_id() const;

    // 同键覆盖（属性在快照里保插入序）
    void set_attribute(const std::string& key, const std::string& value);
    // 字面量重载：没有它，const char* 会走「指针→bool」标准转换抢中 bool 重载
    //（"v" → true → "true"，实测过的坑）
    void set_attribute(const std::string& key, const char* value);
    void set_attribute(const std::string& key, bool value);  // "true"/"false"
    // 整数/浮点统一走模板，避免 int/long/size_t 重载歧义
    template <typename T>
    typename std::enable_if<std::is_integral<T>::value, void>::type
    set_attribute(const std::string& key, T value)
    {
        set_number_attribute(key, static_cast<long long>(value));
    }
    template <typename T>
    typename std::enable_if<std::is_floating_point<T>::value, void>::type
    set_attribute(const std::string& key, T value)
    {
        set_number_attribute(key, static_cast<double>(value));
    }

    void add_event(const std::string& name);
    void set_status(SpanStatus status, const std::string& message = std::string());

    void end();        // 幂等；首次调用生成快照交给 Tracer
    bool ended() const;
    std::int64_t start_ms() const;
    std::int64_t elapsed_ms() const;  // 进行中 = 至今耗时；结束后 = 总时长

private:
    friend class Tracer;
    Span(std::shared_ptr<TracerImpl> impl, std::string service,
         TraceContext context, std::string parent_span_id, std::string name);

    void set_number_attribute(const std::string& key, long long value);
    void set_number_attribute(const std::string& key, double value);
    SpanSnapshot make_snapshot() const;

    std::shared_ptr<TracerImpl> impl_;
    std::string service_;
    TraceContext context_;
    std::string parent_span_id_;
    std::string name_;
    std::vector<std::pair<std::string, std::string>> attributes_;
    std::vector<SpanEvent> events_;
    SpanStatus status_ = SpanStatus::Unset;
    std::string status_message_;
    std::int64_t start_ms_ = 0;
    std::int64_t end_ms_ = 0;
    std::int64_t duration_ms_ = 0;
    bool ended_ = false;
    // 用 steady 起点算时长（end 前后的读取都基于它）
    std::int64_t start_steady_ms_ = 0;

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
};

// RAII 作用域：构造把 span 设为当前线程的 current_span()（可嵌套，析构恢复
// 上一层），析构自动 end()（幂等，内部提前 end 过则无操作）。当前 span 是
// start_child_span() 的挂靠目标——子 span 落在作用域里的父下面
class LIBMINI_API SpanScope {
public:
    explicit SpanScope(const std::shared_ptr<Span>& span);  // 空指针 = 无操作作用域
    ~SpanScope();

    const std::shared_ptr<Span>& span() const;

    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;

private:
    std::shared_ptr<Span> span_;
    std::shared_ptr<Span> prev_;
};

// 当前线程的当前 span（无则返回空指针）
LIBMINI_API std::shared_ptr<Span> current_span();

// 追踪器：span 工厂 + 结束快照收集器（环形上限，丢最旧保最新）。
//
//   Tracer tracer("auth-service");
//   auto sp = tracer.start_span("login", remote_ctx);  // remote_ctx 来自上游头
//   SpanScope scope(sp);
//   tracer.start_child_span("db.query");               // 自动挂在 sp 下
//   ...
//   std::string report = tracer.finished_json();       // 上报/落盘
//
// 线程安全：start_*、finished*/clear 可并发调用；span 自身的线程约定见上。
// 采样：本实现不做过滤，新根 trace 恒 sampled=true，上游 traceparent 的
// sampled 标志原样继承——sampled 只是传播语义，不影响是否收集
class LIBMINI_API Tracer {
public:
    // max_finished = 保留的结束快照上限（环形，超限丢最旧并计 dropped）；
    // 0 = 不保留快照（仍计数 dropped）。service 写入每个快照的 service 字段
    explicit Tracer(const std::string& service_name = std::string(),
                    std::size_t max_finished = 4096);
    ~Tracer() = default;

    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;
    Tracer(Tracer&&) noexcept = default;
    Tracer& operator=(Tracer&&) noexcept = default;

    // 新根：生成新 trace_id
    std::shared_ptr<Span> start_span(const std::string& name);
    // 继续上游/远程上下文：同 trace_id、父指向上游 span_id；上下文无效按新根处理
    std::shared_ptr<Span> start_span(const std::string& name,
                                     const TraceContext& parent);
    // 当前线程 current_span() 的子；无当前 span 时等价于新根
    std::shared_ptr<Span> start_child_span(const std::string& name);

    // 已结束快照，按完成顺序（环形保留最近 max_finished 份）
    std::vector<SpanSnapshot> finished() const;
    std::size_t finished_count() const;
    std::size_t dropped_count() const;  // 因超上限被丢弃的最旧快照数
    void clear();                       // 清空快照（含 dropped 计数）
    // JSON 数组导出（字段同 SpanSnapshot，status 为 "unset"/"ok"/"error"）
    std::string finished_json() const;

    const std::string& service_name() const;

private:
    std::shared_ptr<TracerImpl> impl_;
};

}  // namespace libmini

#endif  // LIBMINI_TRACE_H
