cmake_minimum_required(VERSION 3.15)
project(libmini_smoke_consumer CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
    # libmini 静态包按 MT 运行库分发（conan static profile），
    # 消费方必须匹配同一运行库，否则 LNK2038（见 README「安装与分发」）
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

find_package(libmini REQUIRED CONFIG)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE libmini::libmini)
