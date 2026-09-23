#include "log_facade.h"

#include <mutex>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "file_utils.h"
#include "path_utils.h"

namespace libmini {

namespace {

spdlog::level::level_enum to_spdlog(LogLevel level)
{
    switch (level) {
        case LogLevel::Trace:    return spdlog::level::trace;
        case LogLevel::Debug:    return spdlog::level::debug;
        case LogLevel::Info:     return spdlog::level::info;
        case LogLevel::Warn:     return spdlog::level::warn;
        case LogLevel::Error:    return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
        case LogLevel::Off:      return spdlog::level::off;
    }
    return spdlog::level::info;
}

LogLevel from_spdlog(spdlog::level::level_enum level)
{
    switch (level) {
        case spdlog::level::trace:    return LogLevel::Trace;
        case spdlog::level::debug:    return LogLevel::Debug;
        case spdlog::level::info:     return LogLevel::Info;
        case spdlog::level::warn:     return LogLevel::Warn;
        case spdlog::level::err:      return LogLevel::Error;
        case spdlog::level::critical: return LogLevel::Critical;
        default:                      return LogLevel::Off;
    }
}

constexpr const char* kLoggerName = "libmini";

std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_logger;

}  // namespace

bool LogFacade::init()
{
    return init(Options());
}

bool LogFacade::init(const Options& options)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    try {
        // 重复 init：先释放旧 logger（其 flush/停线程随析构完成）
        if (g_logger) {
            g_logger->flush();
            g_logger.reset();
            spdlog::details::registry::instance().drop(kLoggerName);
        }

        std::vector<spdlog::sink_ptr> sinks;
        if (options.console) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }
        if (!options.file_path.empty()) {
            // 目录不存在时先建（logs/app.log 常见姿势）
            const std::string dir = dirname(options.file_path);
            if (!dir.empty() && !is_directory(dir)) {
                make_directories(dir);
            }
            if (options.max_size_mb > 0 && options.max_files > 0) {
                sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    options.file_path,
                    static_cast<std::size_t>(options.max_size_mb) * 1024 * 1024,
                    static_cast<std::size_t>(options.max_files)));
            } else {
                // 参数非法（0 值）时退化为单文件追加，不吞配置错误
                sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    options.file_path, /*truncate=*/false));
            }
        }
        if (sinks.empty()) {
            return false;  // console=false 且无文件路径：配置无效
        }

        std::shared_ptr<spdlog::logger> logger;
        if (options.async_mode) {
            spdlog::init_thread_pool(8192, 1);
            const auto tp = std::make_shared<spdlog::async_logger>(
                kLoggerName, sinks.begin(), sinks.end(),
                spdlog::thread_pool(), spdlog::async_overflow_policy::block);
            logger = tp;
        } else {
            logger = std::make_shared<spdlog::logger>(kLoggerName,
                                                      sinks.begin(), sinks.end());
        }
        if (!options.pattern.empty()) {
            logger->set_pattern(options.pattern);
        }
        logger->set_level(to_spdlog(options.level));
        logger->flush_on(spdlog::level::warn);  // warn 及以上自动刷盘

        spdlog::register_logger(logger);  // 注册 + 初始化（内部处理 async 线程池引用）
        g_logger = std::move(logger);
        return true;
    } catch (const std::exception&) {
        // 文件打不开等 sink 构造异常：保证不影响调用方进程
        g_logger.reset();
        return false;
    }
}

void LogFacade::shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logger) {
        g_logger->flush();
        g_logger.reset();
        spdlog::details::registry::instance().drop(kLoggerName);
    }
    spdlog::shutdown();  // 停 async 线程池（若曾初始化）
}

spdlog::logger* LogFacade::logger()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_logger.get();
}

void LogFacade::set_level(LogLevel level)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logger) {
        g_logger->set_level(to_spdlog(level));
    }
}

LogLevel LogFacade::level()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_logger) {
        return LogLevel::Off;
    }
    return from_spdlog(g_logger->level());
}

void LogFacade::flush()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logger) {
        g_logger->flush();
    }
}

}  // namespace libmini
