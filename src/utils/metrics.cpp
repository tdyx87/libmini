#include "metrics.h"

#include "log_facade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>

#include <spdlog/logger.h>

namespace libmini {

namespace {

// 原子浮点加法：C++11 的 atomic<double> 没有 fetch_add，用 CAS 循环
void atomic_add(std::atomic<double>& target, double delta)
{
    double old = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(old, old + delta,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

void warn_log(const char* message)
{
    spdlog::logger* log = LogFacade::logger();
    if (log) {
        log->warn("metrics: {}", message);
    }
}

// 内置默认桶：单位无关的兜底（1/2/5 序列）；按量纲统计请传自己的上界
const std::vector<double>& default_bounds()
{
    static const std::vector<double> bounds = {1,   2,   5,   10,  25,
                                               50,  100, 250, 500, 1000};
    return bounds;
}

// 清洗桶上界：保留有限且严格递增的项；丢弃任何一项都告警（一次汇总）。
// 清洗后为空（含全非法输入）回落默认桶；纯空输入走默认桶不告警（文档化行为）
std::vector<double> normalize_bounds(const std::vector<double>& input)
{
    std::vector<double> out;
    out.reserve(input.size());
    bool dropped = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double v = input[i];
        if (!std::isfinite(v) || (!out.empty() && v <= out.back())) {
            dropped = true;
            continue;
        }
        out.push_back(v);
    }
    if (dropped) {
        warn_log("histogram bounds must be finite and strictly increasing; "
                 "invalid entries dropped");
    }
    if (out.empty() && !input.empty()) {
        warn_log("histogram bounds all invalid; falling back to default buckets");
        return default_bounds();
    }
    if (out.empty()) {
        return default_bounds();
    }
    return out;
}

// Prometheus 文本数值格式：整数值不带小数点，其余 9 位有效数字；
// 非有限值按 exposition 规范输出 +Inf/-Inf/NaN
std::string format_value(double v)
{
    if (std::isnan(v)) {
        return "NaN";
    }
    if (std::isinf(v)) {
        return v > 0 ? "+Inf" : "-Inf";
    }
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

const char* type_name(MetricType type)
{
    switch (type) {
    case MetricType::Counter:
        return "counter";
    case MetricType::Gauge:
        return "gauge";
    case MetricType::Histogram:
        return "histogram";
    }
    return "untyped";
}

std::string base_of(const std::string& name)
{
    const std::size_t brace = name.find('{');
    return brace == std::string::npos ? name : name.substr(0, brace);
}

// 序列名 '{...}' 内的标签原文（无标签为空串）
std::string label_inner(const std::string& name)
{
    const std::size_t brace = name.find('{');
    if (brace == std::string::npos) {
        return std::string();
    }
    std::size_t end = name.size();
    if (end > brace + 1 && name[end - 1] == '}') {
        --end;
    }
    return name.substr(brace + 1, end - brace - 1);
}

// HELP 文本转义（exposition 规范：反斜杠与换行）
std::string escape_help(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

// ==================== MetricSample ====================

double MetricSample::percentile(int p) const
{
    if (p < 0) {
        p = 0;
    }
    if (p > 100) {
        p = 100;
    }
    if (type != MetricType::Histogram || total_count == 0 || counts.empty()) {
        return -1.0;
    }
    const double target =
        static_cast<double>(p) / 100.0 * static_cast<double>(total_count);
    double prev_cum = 0.0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const double cum = static_cast<double>(counts[i]);
        if (cum >= target) {
            const double lower = (i == 0) ? 0.0 : upper_bounds[i - 1];
            if (i >= upper_bounds.size()) {
                return lower;  // 落在 +Inf 桶：只能给最后一个有限上界
            }
            const double width = cum - prev_cum;
            if (width <= 0.0) {
                return lower;
            }
            double frac = (target - prev_cum) / width;
            if (frac < 0.0) {
                frac = 0.0;
            }
            if (frac > 1.0) {
                frac = 1.0;
            }
            return lower + frac * (upper_bounds[i] - lower);
        }
        prev_cum = cum;
    }
    return upper_bounds.empty() ? -1.0 : upper_bounds.back();
}

// ==================== MetricCounter ====================

MetricCounter::MetricCounter() : value_(0.0)
{
}

void MetricCounter::inc(double v)
{
    if (!(v > 0.0)) {
        return;  // <=0 或 NaN：计数器禁止回退
    }
    atomic_add(value_, v);
}

double MetricCounter::value() const
{
    return value_.load(std::memory_order_relaxed);
}

void MetricCounter::reset()
{
    value_.store(0.0, std::memory_order_relaxed);
}

// ==================== MetricGauge ====================

MetricGauge::MetricGauge() : value_(0.0)
{
}

void MetricGauge::set(double v)
{
    if (std::isnan(v)) {
        return;
    }
    value_.store(v, std::memory_order_relaxed);
}

void MetricGauge::add(double v)
{
    if (std::isnan(v)) {
        return;
    }
    atomic_add(value_, v);
}

void MetricGauge::sub(double v)
{
    add(-v);
}

double MetricGauge::value() const
{
    return value_.load(std::memory_order_relaxed);
}

void MetricGauge::reset()
{
    value_.store(0.0, std::memory_order_relaxed);
}

// ==================== MetricHistogram ====================

MetricHistogram::MetricHistogram(const std::vector<double>& upper_bounds)
    : bounds_(normalize_bounds(upper_bounds)),
      counts_(new std::atomic<std::uint64_t>[bounds_.size() + 1]()),
      count_(0),
      sum_(0.0)
{
}

void MetricHistogram::observe(double v)
{
    if (std::isnan(v)) {
        return;
    }
    // le 语义：桶上界含端点——lower_bound 给出第一个 >= v 的上界；
    // 超过全部上界落到最后的 +Inf 桶
    const std::size_t idx = static_cast<std::size_t>(
        std::lower_bound(bounds_.begin(), bounds_.end(), v) - bounds_.begin());
    counts_[idx].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    atomic_add(sum_, v);
}

std::uint64_t MetricHistogram::count() const
{
    return count_.load(std::memory_order_relaxed);
}

double MetricHistogram::sum() const
{
    return sum_.load(std::memory_order_relaxed);
}

double MetricHistogram::percentile(int p) const
{
    if (p < 0) {
        p = 0;
    }
    if (p > 100) {
        p = 100;
    }
    const std::uint64_t total = count_.load(std::memory_order_relaxed);
    if (total == 0) {
        return -1.0;
    }
    const double target =
        static_cast<double>(p) / 100.0 * static_cast<double>(total);
    double prev_cum = 0.0;
    std::uint64_t run = 0;
    for (std::size_t i = 0; i <= bounds_.size(); ++i) {
        run += counts_[i].load(std::memory_order_relaxed);
        const double cum = static_cast<double>(run);
        if (cum >= target) {
            if (i >= bounds_.size()) {
                return bounds_.back();
            }
            const double lower = (i == 0) ? 0.0 : bounds_[i - 1];
            const double width = cum - prev_cum;
            if (width <= 0.0) {
                return lower;
            }
            double frac = (target - prev_cum) / width;
            if (frac < 0.0) {
                frac = 0.0;
            }
            if (frac > 1.0) {
                frac = 1.0;
            }
            return lower + frac * (bounds_[i] - lower);
        }
        prev_cum = cum;
    }
    return bounds_.back();
}

std::vector<std::uint64_t> MetricHistogram::cumulative_counts() const
{
    std::vector<std::uint64_t> out(bounds_.size() + 1, 0);
    std::uint64_t run = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        run += counts_[i].load(std::memory_order_relaxed);
        out[i] = run;
    }
    return out;
}

const std::vector<double>& MetricHistogram::upper_bounds() const
{
    return bounds_;
}

void MetricHistogram::reset()
{
    for (std::size_t i = 0; i <= bounds_.size(); ++i) {
        counts_[i].store(0, std::memory_order_relaxed);
    }
    count_.store(0, std::memory_order_relaxed);
    sum_.store(0.0, std::memory_order_relaxed);
}

// ==================== MetricRegistry ====================

struct MetricRegistry::Impl
{
    struct Entry {
        MetricType type = MetricType::Counter;
        std::string base;
        std::shared_ptr<MetricCounter> counter;
        std::shared_ptr<MetricGauge> gauge;
        std::shared_ptr<MetricHistogram> histogram;
    };

    mutable std::mutex mutex;  // 保护下列全部容器
    std::map<std::string, Entry> entries;             // 序列名 → 条目
    std::map<std::string, MetricType> families;       // 族名 → 类型（冲突检查）
    std::map<std::string, std::string> helps;         // 族名 → help（首个非空）

    // 注册前置检查：非法名告警（不拒绝）；已存在条目做类型匹配并补 help。
    // 返回 true = 应继续创建新条目；false = 调用方直接用 existing
    bool prepare(const std::string& name, MetricType type,
                 const std::string& help, const Entry** existing)
    {
        *existing = NULL;
        if (!MetricRegistry::valid_name(name)) {
            warn_log("invalid metric name registered (Prometheus exposition "
                     "requires [a-zA-Z_:][a-zA-Z0-9_:]* and a closing '}')");
        }
        const std::string base = base_of(name);
        std::map<std::string, Entry>::const_iterator it = entries.find(name);
        if (it != entries.end()) {
            if (it->second.type != type) {
                spdlog::logger* log = LogFacade::logger();
                if (log) {
                    log->warn("metrics: metric \"{}\" already registered with "
                              "another type; returning null",
                              name);
                }
                return false;
            }
            *existing = &it->second;
            if (!help.empty() && helps[base].empty()) {
                helps[base] = help;
            }
            return false;
        }
        std::map<std::string, MetricType>::const_iterator fam =
            families.find(base);
        if (fam != families.end() && fam->second != type) {
            spdlog::logger* log = LogFacade::logger();
            if (log) {
                log->warn("metrics: metric family \"{}\" already registered "
                          "with another type; returning null",
                          base);
            }
            return false;
        }
        if (fam == families.end()) {
            families[base] = type;
        }
        if (!help.empty() && helps[base].empty()) {
            helps[base] = help;
        }
        return true;
    }
};

MetricRegistry::MetricRegistry() : impl_(new Impl())
{
}

MetricRegistry::~MetricRegistry()
{
    delete impl_;
    impl_ = NULL;
}

MetricRegistry::MetricRegistry(MetricRegistry&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = NULL;
}

MetricRegistry& MetricRegistry::operator=(MetricRegistry&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = NULL;
    return *this;
}

std::shared_ptr<MetricCounter> MetricRegistry::counter(const std::string& name,
                                                       const std::string& help)
{
    if (!impl_) {
        return std::shared_ptr<MetricCounter>();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const Impl::Entry* existing = NULL;
    if (!impl_->prepare(name, MetricType::Counter, help, &existing)) {
        return existing ? existing->counter
                        : std::shared_ptr<MetricCounter>();  // 类型冲突 → null
    }
    Impl::Entry entry;
    entry.type = MetricType::Counter;
    entry.base = base_of(name);
    entry.counter = std::make_shared<MetricCounter>();
    impl_->entries[name] = entry;
    return entry.counter;
}

std::shared_ptr<MetricGauge> MetricRegistry::gauge(const std::string& name,
                                                   const std::string& help)
{
    if (!impl_) {
        return std::shared_ptr<MetricGauge>();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const Impl::Entry* existing = NULL;
    if (!impl_->prepare(name, MetricType::Gauge, help, &existing)) {
        return existing ? existing->gauge : std::shared_ptr<MetricGauge>();
    }
    Impl::Entry entry;
    entry.type = MetricType::Gauge;
    entry.base = base_of(name);
    entry.gauge = std::make_shared<MetricGauge>();
    impl_->entries[name] = entry;
    return entry.gauge;
}

std::shared_ptr<MetricHistogram> MetricRegistry::histogram(
    const std::string& name, const std::string& help)
{
    return histogram(name, std::vector<double>(), help);
}

std::shared_ptr<MetricHistogram> MetricRegistry::histogram(
    const std::string& name, const std::vector<double>& upper_bounds,
    const std::string& help)
{
    if (!impl_) {
        return std::shared_ptr<MetricHistogram>();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const Impl::Entry* existing = NULL;
    if (!impl_->prepare(name, MetricType::Histogram, help, &existing)) {
        if (existing && !upper_bounds.empty() &&
            upper_bounds != existing->histogram->upper_bounds()) {
            // 换桶等于换序列（桶边界构造后不可变），保留旧桶并提示
            warn_log("histogram already registered with different buckets; "
                     "existing buckets kept");
        }
        return existing ? existing->histogram
                        : std::shared_ptr<MetricHistogram>();
    }
    Impl::Entry entry;
    entry.type = MetricType::Histogram;
    entry.base = base_of(name);
    entry.histogram =
        std::make_shared<MetricHistogram>(upper_bounds);
    impl_->entries[name] = entry;
    return entry.histogram;
}

std::vector<MetricSample> MetricRegistry::collect() const
{
    std::vector<MetricSample> out;
    if (!impl_) {
        return out;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    out.reserve(impl_->entries.size());
    for (std::map<std::string, Impl::Entry>::const_iterator it =
             impl_->entries.begin();
         it != impl_->entries.end(); ++it) {
        MetricSample s;
        s.name = it->first;
        s.base = it->second.base;
        s.type = it->second.type;
        std::map<std::string, std::string>::const_iterator help =
            impl_->helps.find(it->second.base);
        if (help != impl_->helps.end()) {
            s.help = help->second;
        }
        if (it->second.type == MetricType::Counter) {
            s.value = it->second.counter->value();
        } else if (it->second.type == MetricType::Gauge) {
            s.value = it->second.gauge->value();
        } else {
            const MetricHistogram& h = *it->second.histogram;
            s.upper_bounds = h.upper_bounds();
            s.counts = h.cumulative_counts();
            s.total_count = h.count();
            s.sum = h.sum();
        }
        out.push_back(std::move(s));
    }
    // 按 (族名, 序列名) 排序：同族连续，两次采集顺序稳定
    std::sort(out.begin(), out.end(),
              [](const MetricSample& a, const MetricSample& b) {
                  if (a.base != b.base) {
                      return a.base < b.base;
                  }
                  return a.name < b.name;
              });
    return out;
}

std::string MetricRegistry::render_prometheus() const
{
    const std::vector<MetricSample> samples = collect();
    std::string out;
    std::string last_base;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const MetricSample& s = samples[i];
        if (s.base != last_base) {  // 每族只写一组 HELP/TYPE（在序列之前）
            if (!s.help.empty()) {
                out += "# HELP " + s.base + " " + escape_help(s.help) + "\n";
            }
            out += "# TYPE " + s.base + " " + std::string(type_name(s.type)) +
                   "\n";
            last_base = s.base;
        }
        if (s.type != MetricType::Histogram) {
            out += s.name + " " + format_value(s.value) + "\n";
            continue;
        }
        const std::string inner = label_inner(s.name);
        const std::string series_suffix =
            inner.empty() ? std::string() : "{" + inner + "}";
        // 桶序列：'族名_bucket{已有标签,le="上界"}'，计数为累计式，末位 +Inf
        for (std::size_t b = 0; b < s.upper_bounds.size(); ++b) {
            out += s.base + "_bucket{";
            if (!inner.empty()) {
                out += inner + ",";
            }
            out += "le=\"" + format_value(s.upper_bounds[b]) + "\"} " +
                   std::to_string(s.counts[b]) + "\n";
        }
        out += s.base + "_bucket{";
        if (!inner.empty()) {
            out += inner + ",";
        }
        out += "le=\"+Inf\"} " + std::to_string(s.total_count) + "\n";
        out += s.base + "_sum" + series_suffix + " " + format_value(s.sum) +
               "\n";
        out += s.base + "_count" + series_suffix + " " +
               std::to_string(s.total_count) + "\n";
    }
    return out;
}

void MetricRegistry::reset_all()
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (std::map<std::string, Impl::Entry>::iterator it =
             impl_->entries.begin();
         it != impl_->entries.end(); ++it) {
        if (it->second.type == MetricType::Counter) {
            it->second.counter->reset();
        } else if (it->second.type == MetricType::Gauge) {
            it->second.gauge->reset();
        } else {
            it->second.histogram->reset();
        }
    }
}

std::size_t MetricRegistry::size() const
{
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->entries.size();
}

bool MetricRegistry::valid_name(const std::string& name)
{
    const std::size_t brace = name.find('{');
    const std::string base =
        brace == std::string::npos ? name : name.substr(0, brace);
    if (base.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < base.size(); ++i) {
        const char c = base[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == '_' || c == ':';
        const bool digit = (c >= '0' && c <= '9');
        if (!(alpha || (digit && i > 0))) {
            return false;
        }
    }
    if (brace != std::string::npos) {
        // 带标签时必须以 '}' 收尾，且不允许第二个 '{'
        if (name[name.size() - 1] != '}') {
            return false;
        }
        if (name.find('{', brace + 1) != std::string::npos) {
            return false;
        }
    }
    return true;
}

}  // namespace libmini
