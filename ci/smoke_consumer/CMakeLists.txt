cmake_minimum_required(VERSION 3.15)
project(libmini_smoke_consumer CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
    # libmini 静态包按 conan profile 的 runtime_type 分发：Release → MT，
    # Debug → MTd。消费方必须匹配同一运行库变体，否则 LNK2038
    #（_free_dbg/_CrtDbgReport 未解析等，见 run #6 windows-debug 实测）
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
endif()

find_package(libmini REQUIRED CONFIG)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE libmini::libmini)
