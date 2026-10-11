// 子进程句柄实现（见 child_process.h 的说明）。库模块：只依赖 STL 与平台 API，
// 不引入任何第三方依赖。

#include "child_process.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "win_utf.h"

#else

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

namespace libmini {

namespace {

// 把命令行参数拼成 Windows 风格的引号形式（POSIX 侧也只用它做展示）
std::string quote_arg(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '"') {
            out += "\\\"";
        } else {
            out += arg[i];
        }
    }
    out += "\"";
    return out;
}

}  // namespace

struct ChildProcess::Impl
{
    // ---- 通用状态 ----
    mutable std::mutex mutex;     // 保护 buffer / all / exit_code
    std::string buffer;           // 尚未被 take_output() 取走的新输出
    std::string all;              // 从启动至今的全部输出
    std::string command_line;
    long pid_value = -1;
    int exit_code_value = kNotExited;
    bool started_flag = false;
    bool finished = false;
    std::thread reader;

#if defined(_WIN32)
    HANDLE process = 0;
    HANDLE pipe_read = 0;
#else
    // POSIX 侧用管道 fd
    int pipe_fd = -1;
#endif

    ~Impl()
    {
#if defined(_WIN32)
        if (process != 0) {
            CloseHandle(process);
        }
        if (pipe_read != 0) {
            CloseHandle(pipe_read);
        }
#else
        if (pipe_fd >= 0) {
            close(pipe_fd);
        }
#endif
    }

    void append(const char* data, std::size_t count)
    {
        std::lock_guard<std::mutex> guard(mutex);
        buffer.append(data, count);
        all.append(data, count);
    }
};

ChildProcess::ChildProcess() : impl_(new Impl())
{
}

ChildProcess::~ChildProcess()
{
    if (impl_ == 0) {
        return;  // 已被搬走的空壳
    }
    // 析构不再等待：先祈终止，再让读线程自然收尾，避免卡住界面线程
    if (impl_->started_flag && !impl_->finished) {
        kill();
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    delete impl_;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(other.impl_)
{
    other.impl_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    // 先把自己正在托管的进程收干净，再接管对方的实现体
    if (impl_ != 0) {
        if (impl_->started_flag && !impl_->finished) {
            kill();
        }
        if (impl_->reader.joinable()) {
            impl_->reader.join();
        }
        delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = 0;
    return *this;
}

bool ChildProcess::start(const std::string& program, const std::vector<std::string>& args,
                  const std::string& work_dir, std::string* error)
{
    if (impl_->started_flag) {
        if (error != 0) {
            *error = "该句柄已经启动过一个进程";
        }
        return false;
    }

    std::string command = quote_arg(program);
    for (std::size_t i = 0; i < args.size(); ++i) {
        command += " " + quote_arg(args[i]);
    }
    impl_->command_line = command;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = 0;
    HANDLE write_end = 0;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        if (error != 0) {
            *error = "CreatePipe 失败";
        }
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);  // 读端只留在父进程

    STARTUPINFOW si;
    std::memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = 0;
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));

    // CreateProcessW 允许改命令行缓冲，因此给一份可写拷贝
    std::wstring wide_command = libmini::internal::utf8_to_wide(command);
    std::vector<wchar_t> command_buffer(wide_command.begin(), wide_command.end());
    command_buffer.push_back(L'\0');
    const std::wstring wide_dir = libmini::internal::utf8_to_wide(work_dir);

    const BOOL ok = CreateProcessW(0, &command_buffer[0], 0, 0, TRUE, 0, 0,
                                   work_dir.empty() ? 0 : wide_dir.c_str(), &si, &pi);
    CloseHandle(write_end);  // 父进程不再需要写端，否则读不到 EOF
    if (!ok) {
        CloseHandle(read_end);
        if (error != 0) {
            *error = "CreateProcessW 失败（GetLastError=" +
                     std::to_string(static_cast<int>(GetLastError())) + "）";
        }
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
    impl_->pipe_read = read_end;
    impl_->pid_value = static_cast<long>(pi.dwProcessId);
    impl_->started_flag = true;

    // 读线程捕获实现体而不是 this：这样 ChildProcess 被搬走/赋值后线程仍然指向有效内存
    Impl* context = impl_;
    impl_->reader = std::thread([context, read_end]() {
        char buf[4096];
        DWORD got = 0;
        for (;;) {
            if (!ReadFile(read_end, buf, sizeof(buf), &got, 0) || got == 0) {
                break;
            }
            context->append(buf, static_cast<std::size_t>(got));
        }
    });
    return true;

#else
    int fds[2];
    if (pipe(fds) != 0) {
        if (error != 0) {
            *error = std::string("pipe 失败: ") + std::strerror(errno);
        }
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        if (error != 0) {
            *error = std::string("fork 失败: ") + std::strerror(errno);
        }
        return false;
    }
    if (child == 0) {
        // 子进程：stdout/stderr 接到管道写端
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (!work_dir.empty()) {
            if (chdir(work_dir.c_str()) != 0) {
                _exit(127);
            }
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (std::size_t i = 0; i < args.size(); ++i) {
            argv.push_back(const_cast<char*>(args[i].c_str()));
        }
        argv.push_back(0);
        execvp(program.c_str(), &argv[0]);
        _exit(127);
    }
    close(fds[1]);
    impl_->pipe_fd = fds[0];
    impl_->pid_value = static_cast<long>(child);
    impl_->started_flag = true;

    Impl* context = impl_;
    const int pipe_fd = impl_->pipe_fd;
    impl_->reader = std::thread([context, child, pipe_fd]() {
        char buf[4096];
        for (;;) {
            const ssize_t got = read(pipe_fd, buf, sizeof(buf));
            if (got <= 0) {
                break;
            }
            context->append(buf, static_cast<std::size_t>(got));
        }
        int status = 0;
        // 读端 EOF 后子进程基本已退出；这里阻塞等一次并记下退出码
        if (waitpid(child, &status, 0) == child) {
            int code = 0;
            if (WIFEXITED(status)) {
                code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                code = 128 + WTERMSIG(status);
            }
            std::lock_guard<std::mutex> guard(context->mutex);
            context->exit_code_value = code;
            context->finished = true;
        }
    });
    return true;
#endif
}

bool ChildProcess::try_finish(int* exit_code) const
{
    if (!impl_->started_flag) {
        return false;
    }
#if defined(_WIN32)
    if (!impl_->finished) {
        const DWORD state = WaitForSingleObject(impl_->process, 0);
        if (state == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(impl_->process, &code);
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->exit_code_value = static_cast<int>(code);
            impl_->finished = true;
        }
    }
#endif
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->finished) {
        return false;
    }
    if (exit_code != 0) {
        *exit_code = impl_->exit_code_value;
    }
    return true;
}

bool ChildProcess::running() const
{
    return impl_->started_flag && !try_finish(0);
}

int ChildProcess::exit_code() const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->exit_code_value;
}

bool ChildProcess::kill()
{
    if (!impl_->started_flag || try_finish(0)) {
        return false;
    }
#if defined(_WIN32)
    const BOOL ok = TerminateProcess(impl_->process, 0xFFFFu);
    return ok != FALSE;
#else
    if (::kill(static_cast<pid_t>(impl_->pid_value), SIGTERM) != 0) {
        return false;
    }
    // 给它一点体面时间，然后强硬收尾
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        if (try_finish(0)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    ::kill(static_cast<pid_t>(impl_->pid_value), SIGKILL);
    return true;
#endif
}

bool ChildProcess::wait(int timeout_ms)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (try_finish(0)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::string ChildProcess::take_output()
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    std::string out;
    out.swap(impl_->buffer);
    return out;
}

std::string ChildProcess::all_output() const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->all;
}

void ChildProcess::clear_output()
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->buffer.clear();
    impl_->all.clear();
}

std::vector<std::string> ChildProcess::lines(std::size_t max_lines) const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < impl_->all.size(); ++i) {
        const char c = impl_->all[i];
        if (c == '\n') {
            if (!current.empty() && current[current.size() - 1] == '\r') {
                current.erase(current.size() - 1);
            }
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    if (max_lines > 0 && out.size() > max_lines) {
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(out.size() - max_lines));
    }
    return out;
}

long ChildProcess::pid() const
{
    return impl_->pid_value;
}

const std::string& ChildProcess::command_line() const
{
    return impl_->command_line;
}

bool ChildProcess::started() const
{
    return impl_->started_flag;
}

}  // namespace libmini
