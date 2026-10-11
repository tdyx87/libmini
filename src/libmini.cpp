#include <iostream>
#include "libmini.h"

// 进程级初始化（POSIX 下忽略 SIGPIPE）放在 libmini_init.cpp：它必须保证被
// 拉进链接，故单独成一个目标文件并在 src/CMakeLists.txt 里强制拉入。详见
// 那个文件的顶部注释。

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