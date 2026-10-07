#ifndef LIBMINI_METRICS_H
#define LIBMINI_METRICS_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 指标类型（决定序列在 Prometheus 文本中的 TYPE 行与序列名后缀）
enum class MetricType {
    Counter,    // 单调计数器：只增不减（如请求总数）
    Gauge,      // 仪表：可增可减、可直接设置（如队列长度、温度）
    Histogram,  // 直方图：分桶计数 + 观测值累计和（如延迟分布）
};

// 一次采集的序列快照（MetricRegistry::collect() 返回，按指标族名排序）。
// counter/gauge 用 value；histogram 用 upper_bounds/counts/total_count/sum。
// 名称可携带标签（'requests_total{code="200"}'），base 为去标签的族名。
struct LIBMINI_API MetricSample {
    std::string name;   // 序列名（含标签原文）
    std::string base;   // 指标族名（'{' 之前的部分）
    MetricType type = MetricType::Counter;
    std::string help;

    double value = 0.0;  // counter/gauge 当前值（histogram 不用）

    // ---- histogram 分布（counts 为累计式，长度 = 上界数 + 1，末位是 +Inf）----
    std::vector<double> upper_bounds;
    std::vector<std::uint64_t> counts;
    std::uint64_t total_count = 0;
    double sum = 0.0;

    // 分位数估算（p 取 [0,100]）：桶内线性插值；无样本或非 histogram 返回 -1。
    // 误差不超过桶宽——需要更准就传更密的 upper_bounds
    double percentile(int p) const;
};

// 计数器：单调递增，线程安全（CAS 循环，无锁）。
// inc(v) 对 v <= 0 为空操作——计数器语义禁止回退，防御误用
class LIBMINI_API MetricCounter {
public:
    MetricCounter();

    void inc(double v = 1.0);
    double value() const;
    void reset();  // 归零（采集/测试用；生产一般只读）

private:
    std::atomic<double> value_;

    MetricCounter(const MetricCounter&) = delete;
    MetricCounter& operator=(const MetricCounter&) = delete;
};

// 仪表：可任意设置/增减，线程安全
class LIBMINI_API MetricGauge {
public:
    MetricGauge();

    void set(double v);
    void add(double v);  // 可为负（等价于 sub）
    void sub(double v);
    double value() const;
    void reset();

private:
    std::atomic<double> value_;

    MetricGauge(const MetricGauge&) = delete;
    MetricGauge& operator=(const MetricGauge&) = delete;
};

// 直方图：固定桶上界（构造后不可变），每桶计数与 sum/count 全程原子更新，
// observe 与 percentile/采集可并发。
// 上界必须严格递增且有限；非法项经 LogFacade 记 warn 后剔除，全部非法则
// 落到内置默认桶 {1,2,5,10,25,50,100,250,500,1000}（单位无关的兜底，
// 精确统计请按量纲传入自己的上界，如秒制 {0.005,0.01,0.025,...}）
class LIBMINI_API MetricHistogram {
public:
    explicit MetricHistogram(const std::vector<double>& upper_bounds = std::vector<double>());

    void observe(double v);  // NaN 忽略；超过最大上界计入 +Inf 桶
    std::uint64_t count() const;
    double sum() const;
    double percentile(int p) const;  // 语义同 MetricSample::percentile，无样本 -1
    std::vector<std::uint64_t> cumulative_counts() const;  // 长度 = 上界数 + 1
    const std::vector<double>& upper_bounds() const;
    void reset();

private:
    std::vector<double> bounds_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts_;
    std::atomic<std::uint64_t> count_;
    std::atomic<double> sum_;

    MetricHistogram(const MetricHistogram&) = delete;
    MetricHistogram& operator=(const MetricHistogram&) = delete;
};

// 指标注册表：按序列名注册/复用指标，导出采集快照与 Prometheus 文本
//（ exposition 格式，可直接给 pushgateway / node_exporter 拉取）。
//
//   MetricRegistry reg;
//   auto reqs = reg.counter("http_requests_total", "请求总数");
//   auto dur  = reg.histogram("latency_ms{route=\"/api\"}",
//                             {1, 5, 10, 50, 100, 500}, "耗时分布");
//   reqs->inc();
//   dur->observe(12.5);
//   std::string scrape = reg.render_prometheus();
//
// 语义与约束：
//   - 序列名 = 族名 + 可选标签原文；同一序列名重复注册返回同一实例（幂等）；
//   - 同族（base 相同）类型必须一致，冲突时经 LogFacade 记 warn 并返回
//     nullptr（类型无法转换，这是编程错误，不是可兑底的运行时错误）；
//   - 同族第一个非空 help 生效；创建后不可删除，reset_all() 只归零值；
//   - 名称需符合 Prometheus 规范（base：[a-zA-Z_:][a-zA-Z0-9_:]*，带标签时
//     以 '}' 收尾），非法名注册时记 warn 但不拒绝（注册表本质是字符串键值表）。
// 线程安全：注册（写表）、collect、render 可并发；单个指标对象自身线程安全。
class LIBMINI_API MetricRegistry {
public:
    MetricRegistry();
    ~MetricRegistry();
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
    MetricRegistry(MetricRegistry&& other) noexcept;
    MetricRegistry& operator=(MetricRegistry&& other) noexcept;

    // 以下三个：同名同类型返回既有实例；同族类型冲突返回 nullptr（已记 warn）
    std::shared_ptr<MetricCounter> counter(const std::string& name,
                                           const std::string& help = std::string());
    std::shared_ptr<MetricGauge> gauge(const std::string& name,
                                       const std::string& help = std::string());
    // 无 bounds 重载用内置默认桶；显式传 bounds 时，同名已有直方图的旧桶不变
    //（换桶等于换序列，warn 提示）
    std::shared_ptr<MetricHistogram> histogram(const std::string& name,
                                               const std::string& help = std::string());
    std::shared_ptr<MetricHistogram> histogram(const std::string& name,
                                               const std::vector<double>& upper_bounds,
                                               const std::string& help = std::string());

    // 全序列快照，按 (族名, 序列名) 排序——同一进程两次采集顺序稳定
    std::vector<MetricSample> collect() const;

    // Prometheus 文本格式（exposition 0.0.4）：每族一组 # HELP / # TYPE 在前，
    // histogram 输出 _bucket{le=...}（累计计数，末位 +Inf）/ _sum / _count
    std::string render_prometheus() const;

    void reset_all();           // 所有指标归零（保留注册项与桶边界）
    std::size_t size() const;   // 已注册序列数

    // 名称校验（静态工具）：base 非空且首字符 [a-zA-Z_:]、其余 [a-zA-Z0-9_:]，
    // 带标签时必须以 '}' 收尾。仅用于注册告警与调用方自检
    static bool valid_name(const std::string& name);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_METRICS_H
