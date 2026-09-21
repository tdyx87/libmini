#ifndef LIBMINI_CONSOLE_H
#define LIBMINI_CONSOLE_H

#include <atomic>
#include <functional>

#include "libmini.h"

namespace libmini {

// 控制台退出信号统一封装：Ctrl+C / Ctrl+Break（Windows、POSIX）、
// SIGTERM/SIGHUP（POSIX）。典型用途：前台程序收到 Ctrl+C 后优雅收尾
// （刷盘、停服务、释放锁）再退出，与服务模式共用同一份清理逻辑。
//
//   ConsoleExit ce;
//   ce.set_handler([&]() { flush_and_stop(); });  // 在专用线程执行
//   while (!ce.stop_requested()) { serve_one(); }
//
// 语义：
//   - 回调在独立的等待线程上执行，不占用信号处理上下文，可安全使用锁/IO；
//   - stop_requested() 在信号到达后立即置位（任何线程可轮询）；
//   - 析构时自动恢复默认信号行为；同一进程内建议只用一个实例。
class LIBMINI_API ConsoleExit {
public:
    ConsoleExit();
    ~ConsoleExit();

    ConsoleExit(const ConsoleExit&) = delete;
    ConsoleExit& operator=(const ConsoleExit&) = delete;

    // 注册退出回调（只保留一个；重复设置会覆盖）。在信号到达后由
    // 内部等待线程调用一次
    void set_handler(std::function<void()> handler);

    // 信号是否已到达（处理回调可能尚未执行完）
    bool stop_requested() const { return stop_requested_.load(); }

    // 阻塞等待信号到达（等价 while(!stop_requested()) sleep）；
    // 返回后回调已执行完毕，可安全退出进程
    void wait();

    std::atomic<bool> stop_requested_{false};

    // 信号到达的统一入口（平台桥接函数调用）
    void on_signal();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_CONSOLE_H
