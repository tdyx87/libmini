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