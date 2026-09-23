#ifndef LIBMINI_LOG_FACADE_H
#define LIBMINI_LOG_FACADE_H

#include <cstdint>
#include <string>

#include "export.h"

// 只前置声明：接口返回指针，不向消费者泄漏 spdlog 头
namespace spdlog {
class logger;
}

namespace libmini {

// 日志级别（与 spdlog 级别一一对应，库内不泄漏 spdlog 头文件）
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

// 统一日志门面：把项目里散落的 spdlog 初始化收拢为一次调用。
//
//   LogFacade::init(LogFacade::Options());          // 全默认：仅控制台 Info
//   LogFacade::Options o;
//   o.file_path   = "logs/app.log";                 // 启用滚动文件
//   o.max_size_mb = 10;
//   o.max_files   = 5;
//   o.level       = LogLevel::Debug;
//   o.pattern     = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";
//   LogFacade::init(o);
//
//   LogFacade::logger()->info("hello {}", 42);      // 拿 spdlog logger 用
//   LogFacade::set_level(LogLevel::Warn);           // 运行期动态调级
//
// 线程安全：init/shutdown/logger 可任意线程调用（内部加锁）。
// 幂等：重复 init 先关闭旧配置再重建。
class LIBMINI_API LogFacade {
public:
    struct Options
    {
        std::string file_path;      // 滚动文件路径；空 = 不写文件
        std::uint64_t max_size_mb = 10;  // 单文件上限（MB），滚动阈值
        std::uint32_t max_files = 5;     // 保留的滚动文件数
        LogLevel level = LogLevel::Info;
        std::string pattern;        // 空 = spdlog 默认格式
        bool console = true;        // 是否输出到控制台
        bool async_mode = false;    // true = 后台线程写日志（退出前须 shutdown）
    };

    // 按 Options 初始化全局 logger（命名 "libmini"）。返回是否成功。
    // C++11 兼容：默认配置与全量配置拆成两个重载
    static bool init();
    static bool init(const Options& options);

    // 关闭并释放全局 logger：flush + 停后台线程（async 模式必须调用，
    // 一般在 main 退出前或进程退出回调里）
    static void shutdown();

    // 全局 logger（未 init 时返回 null——调用方自行判空或先 init）
    static spdlog::logger* logger();

    // 运行期动态调级（对全局 logger 生效）
    static void set_level(LogLevel level);

    // 当前级别
    static LogLevel level();

    // 立即刷盘（sync 模式 = flush sinks；async 模式 = 入队 flush 命令）
    static void flush();
};

}  // namespace libmini

#endif  // LIBMINI_LOG_FACADE_H
