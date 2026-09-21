cmake_minimum_required(VERSION 3.15)

project(libmini)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set UTF-8 encoding for MSVC
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 编译器告警：默认 -Wall 级别（可见但不阻塞），LIBMINI_WARNINGS_AS_ERRORS=ON
# 时升格为错误（CI 的 warnings-strict 变体开启）。MSVC /W4 对齐 GCC/Clang 的
# -Wall -Wextra 覆盖面
add_compile_options(
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>
)
if(LIBMINI_WARNINGS_AS_ERRORS)
    add_compile_options(
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>
    )
endif()

# Windows API 目标版本：MinGW 默认 _WIN32_WINNT 停留在 XP/7 时代，
# cpp-httplib 0.28+ 要求 Win10 API 集（_WIN32_WINNT >= 0x0A00，检查在
# httplib.h:36，报错文案有误导）。必须在首个 Windows.h 之前定义，
# 故用全局编译定义前置
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

option(LIBMINI_BUILD_SHARED "Build libmini as a shared library" OFF)
option(LIBMINI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(BUILD_TESTING "Build tests" ON)

# 库版本号（src/CMakeLists 的 set_target_properties 与下方 CPack 共用）
set(LIBMINI_VERSION 1.0.0)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE})

add_subdirectory(src)
add_subdirectory(example)
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)
    add_subdirectory(test)
endif()

# ---------------- CPack 打包规则 ----------------
set(CPACK_PACKAGE_NAME "libmini")
set(CPACK_PACKAGE_VENDOR "libmini project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "libmini: lightweight C++ toolkit (strings/files/json/xml/rpc/async/crypto/zip/sqlite)")
set(CPACK_PACKAGE_VERSION ${LIBMINI_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "libmini ${LIBMINI_VERSION}")
# 包文件名：架构按指针宽度判定（交叉编译时 CMAKE_SYSTEM_PROCESSOR 是主机架构，会误报）
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(LIBMINI_PKG_ARCH x86)
else()
    set(LIBMINI_PKG_ARCH x64)
endif()
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${LIBMINI_PKG_ARCH}")

# 组件：runtime（DLL）+ devel（头文件/静态库/CMake config）
set(CPACK_COMPONENTS_ALL runtime devel)
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Runtime (DLL)")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "libmini shared library")
set(CPACK_COMPONENT_DEVEL_DISPLAY_NAME "Development (headers + libs + CMake config)")
set(CPACK_COMPONENT_DEVEL_DESCRIPTION
    "Public headers, static/import libraries and CMake package config; depends on runtime")
set(CPACK_COMPONENT_DEVEL_DEPENDS runtime)

# 绿色压缩包为主生成器（NSIS/WiX 按需再加）
set(CPACK_GENERATOR "ZIP;TGZ")

include(CPack)