#ifndef LIBMINI_PROCESS_H
#define LIBMINI_PROCESS_H

#include <cstdint>
#include <string>
#include <vector>

#include "libmini.h"

namespace libmini {

// 子进程执行封装（对应 boost::process::system / python subprocess.run）。
// 典型用途：调用外部工具（ffmpeg、git、脚本）、启动辅助程序并收集输出。
//
//   ProcessResult r = run_process("git", {"status", "--short"});
//   if (r.exit_code == 0) { use(r.stdout_text); }
//
// Windows 实现走 CreateProcessW（UTF-8 命令行与程序名，输出重定向到管道），
// POSIX 走 fork/execvp。exec_program 只接受程序名+参数列表（无 shell 解析），
// 需要管道/重定向等 shell 特性时显式传 shell=true。

struct LIBMINI_API ProcessResult
{
    int exit_code = -1;          // 退出码；进程被杀时为 -1
    bool timed_out = false;      // 是否因超时被终止
    std::string stdout_text;     // 标准输出（原始字节，UTF-8 语义由被调程序决定）
    std::string stderr_text;     // 标准错误
};

// 一次性执行程序并等待结束。
// program：程序名或完整路径（不含 shell 元字符，逐参数传递，无注入风险）；
// args：参数列表（逐个原样传递）；
// timeout_ms：>0 时超时强杀进程并置 timed_out；0 = 无限等待；
// input_text：写入子进程 stdin 的数据（可选）。
LIBMINI_API ProcessResult run_process(
    const std::string& program,
    const std::vector<std::string>& args = std::vector<std::string>(),
    int timeout_ms = 0,
    const std::string& input_text = std::string());

// 经 shell 执行一行命令（cmd.exe /C 或 /bin/sh -c）。
// 支持管道、重定向等 shell 特性；命令内容来自调用方，注意注入风险。
LIBMINI_API ProcessResult run_shell(
    const std::string& command,
    int timeout_ms = 0);

}  // namespace libmini

#endif  // LIBMINI_PROCESS_H
