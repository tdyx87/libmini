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
#include <fcntl.h>
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

    STARTUPINFOEXW si = {};
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = err_write;

    // 用 PROC_THREAD_ATTRIBUTE_HANDLE_LIST 把可继承句柄白名单收窄到我们自己的
    // 三个端点。bInheritHandles=TRUE 会把父进程里所有可继承句柄都塞给子进程，
    // 包括「另一个线程正在用的 run_process 管道写端」与监听套接字——那时两个
    // 并发调用会各自持有对方的写端，谁也读不到 EOF，表现为并发退化成串行甚至
    // 互相等待到超时。
    HANDLE inherit_handles[3] = {in_read, out_write, err_write};
    SIZE_T attr_size = 0;
    bool attr_initialized = false;
    bool handle_list_ready = false;
    ::InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    if (attr_size > 0) {
        si.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            ::HeapAlloc(::GetProcessHeap(), 0, attr_size));
        if (si.lpAttributeList != NULL &&
            ::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)) {
            attr_initialized = true;
            handle_list_ready =
                ::UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                            inherit_handles, sizeof(inherit_handles),
                                            NULL, NULL) != FALSE;
        }
    }
    // 拿不到句柄列表时退回普通继承（标准流仍由 STARTF_USESTDHANDLES 指定）
    si.StartupInfo.cb = static_cast<DWORD>(
        handle_list_ready ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW));

    PROCESS_INFORMATION pi = {};
    // lpApplicationName 传 NULL：由 Windows 按命令行首 token 搜索
    // （支持裸名 "cmd.exe" 与 PATH 查找；传裸名作 lpApplicationName 会失败）
    const BOOL ok = ::CreateProcessW(
        NULL,
        const_cast<LPWSTR>(wcmdline.c_str()), // 完整命令行（可执行路径需引号）
        NULL, NULL, TRUE,
        handle_list_ready ? EXTENDED_STARTUPINFO_PRESENT : 0, NULL, NULL,
        &si.StartupInfo, &pi);

    if (si.lpAttributeList != NULL) {
        if (attr_initialized) {
            ::DeleteProcThreadAttributeList(si.lpAttributeList);
        }
        ::HeapFree(::GetProcessHeap(), 0, si.lpAttributeList);
    }

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

    // stdin 数据写入：独立线程，避免大输入与进程等待互相阻塞。
    // 该线程会活到 run_process 返回之后，故 input_text 必须按值捕获——按引用
    // 捕获会在调用方销毁实参后悬空。
    std::thread writer([in_write, input_text]() {
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

    // stdout / stderr 各起一个读取线程：管道缓冲区写满后子进程会阻塞在写操作上，
    // 若父进程先等进程结束再排水，两边就会互相等待直到超时（任何输出超过一个
    // 管道缓冲区——Windows 上约 4KB——的子进程都会死锁）
    std::string out_text;
    std::string err_text;
    std::thread out_reader([out_read, &out_text]() { out_text = read_pipe(out_read); });
    std::thread err_reader([err_read, &err_text]() { err_text = read_pipe(err_read); });

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

    // 子进程结束后写端关闭，读取线程随之读到 EOF 并返回
    out_reader.join();
    err_reader.join();
    result.stdout_text.swap(out_text);
    result.stderr_text.swap(err_text);
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
    // 六个端点全部置 FD_CLOEXEC：exec 后子进程只会留下自己那三个标准流
    // （dup2 复制出来的描述符不带 cloexec），不会顺手继承别的线程正在用的
    // 管道端点——否则并发调用会各自持有对方的写端而读不到 EOF
    for (int i = 0; i < 2; ++i) {
        ::fcntl(in_pipe[i], F_SETFD, FD_CLOEXEC);
        ::fcntl(out_pipe[i], F_SETFD, FD_CLOEXEC);
        ::fcntl(err_pipe[i], F_SETFD, FD_CLOEXEC);
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
    // 注意：lambda 不能直接捕获数组元素（C++11 限制），复制成标量；
    // 线程会活到 run_process 返回之后，故 input_text 按值捕获
    const int in_write_fd = in_pipe[1];
    if (!input_text.empty()) {
        std::thread writer([in_write_fd, input_text]() {
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

    // stdout / stderr 各起一个读取线程，边跑边排空：管道写满后子进程会阻塞在
    // 写操作上，若等到进程结束再 drain，两边会互相等待直到超时
    const int out_read_fd = out_pipe[0];
    const int err_read_fd = err_pipe[0];
    std::string out_text;
    std::string err_text;
    std::thread out_reader([out_read_fd, &out_text]() { out_text = drain_fd(out_read_fd); });
    std::thread err_reader([err_read_fd, &err_text]() { err_text = drain_fd(err_read_fd); });

    // 带超时等待：超时则 SIGKILL
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
    // exec 失败惯例退出码 127：映射为「启动失败」-1，跨平台语义一致
    if (result.exit_code == 127) {
        result.exit_code = -1;
    }
    // 子进程结束后写端关闭，读取线程随之读到 EOF 并返回
    out_reader.join();
    err_reader.join();
    result.stdout_text.swap(out_text);
    result.stderr_text.swap(err_text);
    return result;
}

ProcessResult run_shell(const std::string& command, int timeout_ms)
{
    return run_process("/bin/sh", {"-c", command}, timeout_ms);
}

#endif  // _WIN32

}  // namespace libmini
