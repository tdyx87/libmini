#include <iostream>
#include "libmini.h"

#ifdef SIGPIPE
#include <csignal>
#endif

namespace {
#ifdef SIGPIPE
// macOS / Linux 上进程全局忽略 SIGPIPE，使第三方代码（含 HTTP 传输、第三方
// 插件）在写入已关闭的 socket 时不再被信号杀掉。库自有传输（rpc/tcp/websocket）
// 已经靠 SO_NOSIGPIPE 做了每-socket 保护，此处是对第三方代码、子进程继承 fd、
// 以及库初始化前建立的 fd 的补充。Windows 上 SIGPIPE 不存在，跳过。
struct ignore_sigpipe {
    ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }
};
static ignore_sigpipe g_ignore_sigpipe;
#endif
}  // anonymous namespace

// 示例函数
void hello() {
    std::cout << "Hello from libmini!" << std::endl;
}

std::string getHelloMessage() {
    return "Hello from libmini!";
}

void logMessage(ILogger* logger, const std::string& message) {
    if (logger) {
        logger->log(message);
    }
}