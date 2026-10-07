#include "trace.h"

#include "encoding.h"
#include "json_utils.h"
#include "random_utils.h"  // rng_engine：CSPRNG 不可用时 make_id 的兑底随机源
#include "secure_random.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace libmini {

namespace {

// steady 时钟的毫秒读数（单调，算时长用；起点任意，只做差值）
std::int64_t steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// epoch 毫秒（墙上时钟，跨机对齐/展示用）
std::int64_t wall_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_hex(const std::string& s)
{
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

bool all_zero(const std::string& s)
{
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '0') {
            return false;
        }
    }
    return true;
}

// bytes 字节随机 id → 2*bytes 位小写十六进制。CSPRNG 不可用时退回
// mt19937——追踪 id 只用于关联，不是安全凭证，弱随机可接受（见 secure_random 注释）
std::string make_id(std::size_t bytes)
{
    std::string raw(bytes, '\0');
    if (secure_random_bytes(&raw[0], bytes)) {
        return Hex::encode(raw, true);
    }
    std::mt19937& engine = rng_engine();
    for (std::size_t i = 0; i < bytes; ++i) {
        raw[i] = static_cast<char>(engine() & 0xFF);
    }
    return Hex::encode(raw, true);
}

// 属性数值：整数值不带小数点，其余 9 位有效数字；NaN/±Inf 不做整型转换
//（超出 long long 范围的转换是未定义行为，先判有限再判范围）
std::string format_number(double v)
{
    if (v != v) {
        return "nan";
    }
    if (v > 1.7976931348623157e308) {
        return "inf";
    }
    if (v < -1.7976931348623157e308) {
        return "-inf";
    }
    if (v > -1e15 && v < 1e15 &&
        v == static_cast<double>(static_cast<long long>(v))) {
        return std::to_string(static_cast<long long>(v));
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

const char* status_text(SpanStatus status)
{
    switch (status) {
    case SpanStatus::Ok:
        return "ok";
    case SpanStatus::Error:
        return "error";
    case SpanStatus::Unset:
        break;
    }
    return "unset";
}

// 当前线程 current 槽（SpanScope 与 current_span() 共用；同 TU 内可见）
std::shared_ptr<Span>& current_span_slot()
{
    static thread_local std::shared_ptr<Span> slot;
    return slot;
}

}  // namespace

// ==================== TraceContext ====================

bool TraceContext::valid() const
{
    return trace_id.size() == 32 && span_id.size() == 16 &&
           is_hex(trace_id) && is_hex(span_id) && !all_zero(trace_id) &&
           !all_zero(span_id);
}

std::string TraceContext::traceparent() const
{
    if (!valid()) {
        return std::string();
    }
    char flags[4];
    std::snprintf(flags, sizeof(flags), "%02x", sampled ? 1 : 0);
    return "00-" + trace_id + "-" + span_id + "-" + flags;
}

TraceContext TraceContext::parse_traceparent(const std::string& value)
{
    TraceContext out;
    // 规范格式：version(2)-trace(32)-span(16)-flags(2)，'-'.join 四段
    std::string parts[4];
    std::size_t begin = 0;
    for (int i = 0; i < 4; ++i) {
        const std::size_t dash = value.find('-', begin);
        if (i < 3) {
            if (dash == std::string::npos) {
                return TraceContext();  // 段数不足 → 无效
            }
            parts[i] = value.substr(begin, dash - begin);
            begin = dash + 1;
        } else {
            if (dash != std::string::npos) {
                return TraceContext();  // 段数超出 → 无效
            }
            parts[i] = value.substr(begin);
        }
    }
    if (parts[0] != "00" || parts[1].size() != 32 || parts[2].size() != 16 ||
        parts[3].size() != 2) {
        return TraceContext();
    }
    if (!is_hex(parts[1]) || !is_hex(parts[2]) || !is_hex(parts[3])) {
        return TraceContext();
    }
    if (all_zero(parts[1]) || all_zero(parts[2])) {
        return TraceContext();
    }
    out.trace_id = parts[1];
    out.span_id = parts[2];
    out.sampled = (std::strtoul(parts[3].c_str(), NULL, 16) & 0x1u) != 0;
    return out;
}// ==================== TracerImpl ====================

struct TracerImpl {
    mutable std::mutex mutex;  // 保护收集状态
    std::string service;
    std::size_t max_finished = 4096;
    std::deque<SpanSnapshot> finished;  // 完成顺序；超限丢最旧
    std::size_t dropped = 0;

    void on_finished(SpanSnapshot snapshot)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (max_finished == 0) {
            ++dropped;
            return;
        }
        finished.push_back(std::move(snapshot));
        while (finished.size() > max_finished) {
            finished.pop_front();
            ++dropped;
        }
    }
};

// ==================== Span ====================

Span::Span(std::shared_ptr<TracerImpl> impl, std::string service,
           TraceContext context, std::string parent_span_id, std::string name)
    : impl_(std::move(impl)),
      service_(std::move(service)),
      context_(std::move(context)),
      parent_span_id_(std::move(parent_span_id)),
      name_(std::move(name))
{
    start_ms_ = wall_ms();
    start_steady_ms_ = steady_ms();
}

const std::string& Span::name() const
{
    return name_;
}

const TraceContext& Span::context() const
{
    return context_;
}

const std::string& Span::parent_span_id() const
{
    return parent_span_id_;
}

void Span::set_attribute(const std::string& key, const std::string& value)
{
    for (std::size_t i = 0; i < attributes_.size(); ++i) {
        if (attributes_[i].first == key) {
            attributes_[i].second = value;
            return;
        }
    }
    attributes_.push_back(std::make_pair(key, value));
}

void Span::set_attribute(const std::string& key, const char* value)
{
    set_attribute(key, std::string(value != NULL ? value : ""));
}

void Span::set_attribute(const std::string& key, bool value)
{
    set_attribute(key, std::string(value ? "true" : "false"));
}

void Span::set_number_attribute(const std::string& key, long long value)
{
    set_attribute(key, std::to_string(value));
}

void Span::set_number_attribute(const std::string& key, double value)
{
    set_attribute(key, format_number(value));
}

void Span::add_event(const std::string& name)
{
    SpanEvent event;
    event.name = name;
    event.at_ms = wall_ms();
    events_.push_back(std::move(event));
}

void Span::set_status(SpanStatus status, const std::string& message)
{
    status_ = status;
    status_message_ = message;
}

void Span::end()
{
    if (ended_) {
        return;
    }
    ended_ = true;
    end_ms_ = wall_ms();
    duration_ms_ = steady_ms() - start_steady_ms_;
    if (duration_ms_ < 0) {
        duration_ms_ = 0;
    }
    if (impl_) {
        impl_->on_finished(make_snapshot());
    }
}

bool Span::ended() const
{
    return ended_;
}

std::int64_t Span::start_ms() const
{
    return start_ms_;
}

std::int64_t Span::elapsed_ms() const
{
    if (ended_) {
        return duration_ms_;
    }
    const std::int64_t elapsed = steady_ms() - start_steady_ms_;
    return elapsed < 0 ? 0 : elapsed;
}

SpanSnapshot Span::make_snapshot() const
{
    SpanSnapshot snapshot;
    snapshot.service = service_;
    snapshot.trace_id = context_.trace_id;
    snapshot.span_id = context_.span_id;
    snapshot.parent_span_id = parent_span_id_;
    snapshot.name = name_;
    snapshot.status = status_;
    snapshot.status_message = status_message_;
    snapshot.start_ms = start_ms_;
    snapshot.end_ms = end_ms_;
    snapshot.duration_ms = duration_ms_;
    snapshot.attributes = attributes_;
    snapshot.events = events_;
    return snapshot;
}

// ==================== SpanScope / current_span ====================

SpanScope::SpanScope(const std::shared_ptr<Span>& span)
    : span_(span), prev_(current_span_slot())
{
    current_span_slot() = span_;
}

SpanScope::~SpanScope()
{
    if (span_) {
        span_->end();  // 幂等：作用域内手动 end 过则无操作
    }
    current_span_slot() = prev_;
    // 之前在栈上留一个 shared_ptr 副本（prev_）——恢复后释放
}

const std::shared_ptr<Span>& SpanScope::span() const
{
    return span_;
}

std::shared_ptr<Span> current_span()
{
    return current_span_slot();
}

// ==================== Tracer ====================

Tracer::Tracer(const std::string& service_name, std::size_t max_finished)
    : impl_(std::make_shared<TracerImpl>())
{
    impl_->service = service_name;
    impl_->max_finished = max_finished;
}

std::shared_ptr<Span> Tracer::start_span(const std::string& name)
{
    TraceContext context;
    context.trace_id = make_id(16);  // 16 字节 → 32 hex
    context.span_id = make_id(8);    // 8 字节 → 16 hex
    context.sampled = true;
    return std::shared_ptr<Span>(
        new Span(impl_, impl_->service, context, std::string(), name));
}

std::shared_ptr<Span> Tracer::start_span(const std::string& name,
                                         const TraceContext& parent)
{
    TraceContext context;
    const bool has_parent = parent.valid();
    if (has_parent) {
        context.trace_id = parent.trace_id;  // 继续上游 trace
        context.sampled = parent.sampled;
    } else {
        context.trace_id = make_id(16);  // 无效父：降级为新根
    }
    context.span_id = make_id(8);
    const std::string parent_id = has_parent ? parent.span_id : std::string();
    return std::shared_ptr<Span>(
        new Span(impl_, impl_->service, context, parent_id, name));
}

std::shared_ptr<Span> Tracer::start_child_span(const std::string& name)
{
    const std::shared_ptr<Span> current = current_span_slot();
    if (current && !current->context().trace_id.empty()) {
        return start_span(name, current->context());
    }
    return start_span(name);
}

std::vector<SpanSnapshot> Tracer::finished() const
{
    std::vector<SpanSnapshot> out;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    out.assign(impl_->finished.begin(), impl_->finished.end());
    return out;
}

std::size_t Tracer::finished_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->finished.size();
}

std::size_t Tracer::dropped_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->dropped;
}

void Tracer::clear()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->finished.clear();
    impl_->dropped = 0;
}

std::string Tracer::finished_json() const
{
    JsonValue array = JsonValue::array();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (std::size_t i = 0; i < impl_->finished.size(); ++i) {
        const SpanSnapshot& s = impl_->finished[i];
        JsonValue item;
        item["service"] = s.service;
        item["trace_id"] = s.trace_id;
        item["span_id"] = s.span_id;
        item["parent_span_id"] = s.parent_span_id;
        item["name"] = s.name;
        item["status"] = status_text(s.status);
        item["status_message"] = s.status_message;
        item["start_ms"] = s.start_ms;
        item["end_ms"] = s.end_ms;
        item["duration_ms"] = s.duration_ms;
        JsonValue attrs = JsonValue::object();
        for (std::size_t a = 0; a < s.attributes.size(); ++a) {
            attrs[s.attributes[a].first] = s.attributes[a].second;
        }
        item["attributes"] = attrs;
        JsonValue events = JsonValue::array();
        for (std::size_t e = 0; e < s.events.size(); ++e) {
            JsonValue ev;
            ev["name"] = s.events[e].name;
            ev["at_ms"] = s.events[e].at_ms;
            events.push_back(ev);
        }
        item["events"] = events;
        array.push_back(item);
    }
    return to_json_string(array);
}

const std::string& Tracer::service_name() const
{
    return impl_->service;  // 构造后不再修改，无锁读安全
}

}  // namespace libmini
