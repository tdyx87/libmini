#include "process.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "win_utf.h"
#else
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace libmini {

namespace {

#ifdef _WIN32

// 转义单个参数为 Windows 命令行片段（处理空格与引号，CRT 解析规则）
std::string quote_arg(const std::string& arg)
{
    // 无空格无引号且非空：原样
    if (!arg.empty() &&
        arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            out.push_back(c);
            continue;
        }
        if (c == '"') {
            // 引号前每个反斜杠都要双写，引号本身转义为 \"
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');  // 结尾引号前的反斜杠双写
    out.push_back('"');
    return out;
}

// 读管道到字符串（阻塞直到写端关闭；子进程结束后写端随之关闭）
std::string read_pipe(HANDLE h)
{
    std::string out;
    char buf[4096];
    DWORD n = 0;
    for (;;) {
        if (!::ReadFile(h, buf, sizeof(buf), &n, NULL) || n == 0) {
            break;
        }
        out.append(buf, n);
    }
    ::CloseHandle(h);
    return out;
}

#endif  // _WIN32

}  // namespace

#ifdef _WIN32

ProcessResult run_process(const std::string& program,
                          const std::vector<std::string>& args,
                          int timeout_ms,
                          const std::string& input_text)
{
    ProcessResult result;

    // 命令行：程序名在前，参数逐个转义拼接
    std::string cmdline = quote_arg(program);
    for (const std::string& a : args) {
        cmdline.push_back(' ');
        cmdline += quote_arg(a);
    }
    const std::wstring wcmdline = internal::utf8_to_wide(cmdline);
    const std::wstring wprogram = internal::utf8_to_wide(program);

    // 三对管道：stdin / stdout / stderr
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;      // 子进程继承使用的端点
    sa.lpSecurityDescriptor = NULL;

    HANDLE in_read = NULL, in_write = NULL;
    HANDLE out_read = NULL, out_write = NULL;
    HANDLE err_read = NULL, err_write = NULL;
    if (!::CreatePipe(&in_read, &in_write, &sa, 0) ||
        !::CreatePipe(&out_read, &out_write, &sa, 0) ||
        !::CreatePipe(&err_read, &err_write, &sa, 0)) {
        return result;  // exit_code = -1
    }
    // 本进程持有的端点不可被子进程继承
    ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = err_write;

    PROCESS_INFORMATION pi = {};
    // lpApplicationName 传 NULL：由 Windows 按命令行首 token 搜索
    // （支持裸名 "cmd.exe" 与 PATH 查找；传裸名作 lpApplicationName 会失败）
    const BOOL ok = ::CreateProcessW(
        NULL,
        const_cast<LPWSTR>(wcmdline.c_str()), // 完整命令行（可执行路径需引号）
        NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);

    // 父进程侧不用的端点立即关闭（子进程拿到唯一的继承端点）
    ::CloseHandle(in_read);
    ::CloseHandle(out_write);
    ::CloseHandle(err_write);

    if (!ok) {
        ::CloseHandle(in_write);
        ::CloseHandle(out_read);
        ::CloseHandle(err_read);
        return result;  // exit_code = -1
    }
    ::CloseHandle(pi.hThread);

    // stdin 数据写入：独立线程，避免大输入与进程等待互相阻塞
    std::thread writer([in_write, &input_text]() {
        if (!input_text.empty()) {
            DWORD written = 0;
            std::size_t off = 0;
            while (off < input_text.size()) {
                if (!::WriteFile(in_write, input_text.data() + off,
                                 static_cast<DWORD>(input_text.size() - off),
                                 &written, NULL) ||
                    written == 0) {
                    break;  // 管道断开（子进程退出/崩溃）
                }
                off += written;
            }
        }
        ::CloseHandle(in_write);
    });
    writer.detach();

    const DWORD wait_ms =
        timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, wait_ms);
    if (wr == WAIT_TIMEOUT) {
        result.timed_out = true;
        result.exit_code = -1;
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    } else {
        DWORD code = 1;
        ::GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = static_cast<int>(code);
    }
    ::CloseHandle(pi.hProcess);

    // 进程结束后排水输出（写端已关，read_pipe 会自然返回）
    result.stdout_text = read_pipe(out_read);
    result.stderr_text = read_pipe(err_read);
    return result;
}

ProcessResult run_shell(const std::string& command, int timeout_ms)
{
    return run_process("cmd.exe", {"/C", command}, timeout_ms);
}

#else  // POSIX

namespace {

// drain 到 EOF 后关闭 fd
std::string drain_fd(int fd)
{
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

}  // namespace

ProcessResult run_process(const std::string& program,
                          const std::vector<std::string>& args,
                          int timeout_ms,
                          const std::string& input_text)
{
    ProcessResult result;

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 ||
        ::pipe(err_pipe) != 0) {
        return result;
    }

    // 参数列表转 argv（execvp 需要的 char*，内容不会被修改）
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(NULL);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        return result;
    }
    if (pid == 0) {
        // 子进程：重定向标准流后 exec
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        ::execvp(program.c_str(), argv.data());
        _exit(127);  // exec 失败
    }

    // 父进程：关闭不用的端点
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    // stdin 写入（独立线程）。
    // 注意：lambda 不能直接捕获数组元素（C++11 限制），复制成标量
    const int in_write_fd = in_pipe[1];
    if (!input_text.empty()) {
        std::thread writer([in_write_fd, &input_text]() {
            std::size_t off = 0;
            while (off < input_text.size()) {
                const ssize_t n = ::write(
                    in_write_fd, input_text.data() + off,
                    input_text.size() - off);
                if (n <= 0) {
                    break;
                }
                off += static_cast<std::size_t>(n);
            }
            ::close(in_write_fd);
        });
        writer.detach();
    } else {
        ::close(in_pipe[1]);
    }

    // 带超时等待：先 poll 输出管道避免缓冲区满死锁，超时则 SIGKILL
    const bool use_timeout = timeout_ms > 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    int status = 0;
    bool timed_out = false;
    for (;;) {
        const pid_t wr = ::waitpid(pid, &status, WNOHANG);
        if (wr == pid) {
            break;
        }
        if (wr < 0) {
            break;  // 等待错误
        }
        if (use_timeout &&
            std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    result.timed_out = timed_out;
    result.exit_code = timed_out
                           ? -1
                           : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    result.stdout_text = drain_fd(out_pipe[0]);
    result.stderr_text = drain_fd(err_pipe[0]);
    return result;
}

ProcessResult run_shell(const std::string& command, int timeout_ms)
{
    return run_process("/bin/sh", {"-c", command}, timeout_ms);
}

#endif  // _WIN32

}  // namespace libmini
