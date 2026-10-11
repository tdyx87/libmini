# 基础模块测试（string/time/file/path/thread/json/xml/serialization）
find_package(GTest REQUIRED)
# gtest 1.15 头文件要求 C++14（MSVC 默认标准放行，GCC/Clang 严格）。
# 仅本目录生效：库本体保持 C++11
set(CMAKE_CXX_STANDARD 14)
add_executable(test_libmini test_libmini.cpp)
target_include_directories(test_libmini PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(test_libmini PRIVATE LIBMINI_STATIC)
endif()
target_link_libraries(test_libmini PRIVATE libmini GTest::gtest)
add_test(NAME libmini_test COMMAND test_libmini)

# SWF Shape Viewer：独立 Win32 图形调试程序（不参与 CTest）。
# windows.h 专属，非 Windows 平台强制关闭（防止 CI 首编踩坑）
option(LIBMINI_BUILD_SWF_VIEWER "Build the SWF shape viewer debug tool" ON)
if(NOT WIN32)
    set(LIBMINI_BUILD_SWF_VIEWER OFF)
endif()
if(LIBMINI_BUILD_SWF_VIEWER)
    add_executable(swf_viewer WIN32 swf_viewer.cpp)
    target_compile_definitions(swf_viewer PRIVATE UNICODE _UNICODE)
endif()

# RPC 模块测试
add_executable(test_rpc test_rpc.cpp)
target_include_directories(test_rpc PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(test_rpc PRIVATE LIBMINI_STATIC)
endif()
target_link_libraries(test_rpc PRIVATE libmini GTest::gtest)
add_test(NAME rpc_test COMMAND test_rpc)

# 通用工具模块测试（string_algo / lexical_cast / uuid / crc / encoding / stopwatch / scope_guard）
add_executable(test_utils test_utils.cpp)
target_include_directories(test_utils PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(test_utils PRIVATE LIBMINI_STATIC)
endif()
target_link_libraries(test_utils PRIVATE libmini GTest::gtest)
add_test(NAME utils_test COMMAND test_utils)

# UAX #29 符合性测试的数据文件：Unicode 官方 GraphemeBreakTest.txt 随仓库提交，
# 这里注入绝对路径（与下面子进程路径同理），免得依赖 ctest 的工作目录。
# 用 cache 变量是为了能换成别的（例如新版 UCD）的副本，而不必改这个文件。
set(LIBMINI_GRAPHEME_TEST_DATA_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/data/GraphemeBreakTest.txt"
    CACHE FILEPATH "GraphemeBreakTest.txt used by the UAX #29 conformance test")
target_compile_definitions(test_utils PRIVATE
    LIBMINI_GRAPHEME_TEST_DATA="${LIBMINI_GRAPHEME_TEST_DATA_FILE}")

# 第二批常用模块测试（optional / random / env / digest / ini / gzip）
add_executable(test_common test_common.cpp)
target_include_directories(test_common PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(test_common PRIVATE LIBMINI_STATIC)
endif()
target_link_libraries(test_common PRIVATE libmini GTest::gtest)
add_test(NAME common_test COMMAND test_common)

# 命令行解析测试
add_executable(test_args test_args.cpp)
target_include_directories(test_args PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(test_args PRIVATE LIBMINI_STATIC)
endif()
target_link_libraries(test_args PRIVATE libmini GTest::gtest)
add_test(NAME args_test COMMAND test_args)

# 跨进程写锁行为测试所需的辅助进程（被 run_process 启动，不走 gtest）
add_executable(test_sqlite_child test_sqlite_child.cpp)
target_include_directories(test_sqlite_child PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(test_sqlite_child PRIVATE LIBMINI_STATIC)
endif()
target_link_libraries(test_sqlite_child PRIVATE libmini)

# 跨进程写锁测试需要启动上面的辅助进程：用生成器表达式把其绝对路径注入测试，
# 避免依赖 ctest 的工作目录（相对路径在 IDE/不同 ctest 调用下都能工作）
target_compile_definitions(test_libmini PRIVATE
    LIBMINI_SQLITE_CHILD_EXE="$<TARGET_FILE:test_sqlite_child>")
add_dependencies(test_libmini test_sqlite_child)

# run_process 大输出回归测试的辅助进程：专门写出超过管道缓冲区的内容，
# 验证父进程边跑边排水（旧实现会在此死锁）
add_executable(test_process_child process_child.cpp)

target_compile_definitions(test_libmini PRIVATE
    LIBMINI_PROCESS_CHILD_EXE="$<TARGET_FILE:test_process_child>")
add_dependencies(test_libmini test_process_child)