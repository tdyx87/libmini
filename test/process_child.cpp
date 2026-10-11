// run_process 回归测试的辅助进程：向 stdout / stderr 各写指定字节数后按指定码退出。
//
// 用法：process_child <stdout_bytes> <stderr_bytes> <exit_code> [sleep_ms]
//
// 输出量取「超过管道缓冲区」（Windows 匿名管道约 4KB、POSIX 64KB）的量级，
// 用于验证父进程是在子进程运行期间就排空管道，而不是等进程结束后再读——后者
// 会让子进程阻塞在写操作上，父进程空等到超时。
//
// 可选的 sleep_ms 让进程先按住不动，供 ChildProcess 的「常驻进程 + kill()」测试
// 留出一个能确定杀到的窗口（此时还没写出任何东西）。
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char* argv[])
{
    if (argc < 4) {
        return 2;  // 用法错误
    }
    const long stdout_bytes = std::strtol(argv[1], NULL, 10);
    const long stderr_bytes = std::strtol(argv[2], NULL, 10);
    const int exit_code = static_cast<int>(std::strtol(argv[3], NULL, 10));
    const long sleep_ms = argc >= 5 ? std::strtol(argv[4], NULL, 10) : 0;

    if (sleep_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    if (stdout_bytes > 0) {
        std::cout << std::string(static_cast<std::size_t>(stdout_bytes), 'O');
        std::cout.flush();
    }
    if (stderr_bytes > 0) {
        std::cerr << std::string(static_cast<std::size_t>(stderr_bytes), 'E');
        std::cerr.flush();
    }
    return exit_code;
}
