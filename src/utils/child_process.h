#ifndef LIBMINI_CHILD_PROCESS_H
#define LIBMINI_CHILD_PROCESS_H

// 子进程句柄（库模块）：管理「常驻服务」型子进程（启动/轮询/停止/增量读输出）。
//
// 为什么不用同库 utils/process.h 的 run_process()：那是阻塞式的「跑完拿到结果」，
// 适合自检与一次性命令；本类要的是「起来之后一直活着、随时能问它还在不在、随时
// 能叫停」，并且输出要边跑边看。两者是不同形状的 API，硬用阻塞版就得配一个线程，
// 而且中途无法叫停。
//
//   libmini::ChildProcess child;
//   if (!child.start("my_service", {"--port", "8080"})) { ... }
//   while (child.running()) {
//       const std::string fresh = child.take_output();   // 增量，不会重复给
//       render(fresh);
//       if (user_wants_stop) { child.kill(); }
//   }
//   const int code = child.exit_code();   // try_finish() 返回 true 后才有意义
//
// 平台：Windows 用 CreateProcessW + 一根合并了 stdout/stderr 的匿名管道 + 读线程；
// POSIX 用 fork/execvp + pipe + 读线程 + waitpid(WNOHANG)。没有平台时（理论上）
// start() 直接失败，调用方照常显示错误。

#include <cstddef>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {

class LIBMINI_API ChildProcess
{
public:
    ChildProcess();
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    // 可移动：读线程捕获的是内部实现体而不是 this，所以搬走后旧对象只是空壳
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    // 启动子进程。work_dir 为空表示继承当前目录。失败填 error 并返回 false。
    bool start(const std::string& program,
               const std::vector<std::string>& args = std::vector<std::string>(),
               const std::string& work_dir = std::string(), std::string* error = 0);

    // 进程是否还活着（非阻塞；会顺手回收一次退出状态）。
    // 标 const：「查询进程状态」在逻辑上不改变本对象的契约，缓存退出码属于内部实现。
    bool running() const;

    // 若已退出，返回 true 并给出退出码（幂等；未退出返回 false）
    bool try_finish(int* exit_code) const;
    // 退出码；未退出或还没回收时为 kNotExited
    int exit_code() const;
    static const int kNotExited = -1000000;

    // 请求终止（Windows: TerminateProcess；POSIX: SIGTERM，1.5s 后 SIGKILL）
    bool kill();
    // 等待退出（超时毫秒；返回是否已退出）
    bool wait(int timeout_ms);

    // 取走并清空「自上次取走以来」的新输出（增量，日志视图用）
    std::string take_output();
    // 从启动至今的全部输出
    std::string all_output() const;
    // 全部输出按行切分（截取最后 max_lines 行）
    std::vector<std::string> lines(std::size_t max_lines) const;
    // 清空已累积的输出（含增量缓冲）；不影响到进程本身
    void clear_output();

    // 系统进程号（未启动为 -1）
    long pid() const;
    // 启动用的命令行文本（用于界面展示）
    const std::string& command_line() const;
    bool started() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_CHILD_PROCESS_H
