#include <iostream>
#include "libmini.h"

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