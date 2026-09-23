set(LIBMINI_SOURCES
    libmini.cpp
    utils/string_utils.cpp
    utils/string_algo.cpp
    utils/time_utils.cpp
    utils/stopwatch.cpp
    utils/file_utils.cpp
    utils/file_lock.cpp
    utils/dir_watcher.cpp
    utils/win_service.cpp
    utils/path_utils.cpp
    utils/thread_utils.cpp
    utils/json_utils.cpp
    utils/xml_utils.cpp
    utils/serialization.cpp
    utils/rpc.cpp
    utils/uuid.cpp
    utils/crc.cpp
    utils/encoding.cpp
    utils/random_utils.cpp
    utils/env.cpp    utils/digest.cpp
    utils/hmac.cpp
    utils/process.cpp    utils/base32.cpp
    utils/console.cpp
    utils/tcp.cpp
    utils/zip.cpp
    utils/aes_gcm.cpp
    utils/ini_config.cpp
    utils/gzip.cpp
    utils/async.cpp
    utils/args.cpp
    utils/sqlite.cpp
    utils/system_info.cpp
    utils/http_client.cpp
    utils/log_facade.cpp
)

if(LIBMINI_BUILD_SHARED)
    add_library(libmini SHARED ${LIBMINI_SOURCES})
    target_compile_definitions(libmini PRIVATE LIBMINI_EXPORTS)
    # 导出面 = 公开头里标注了 LIBMINI_API 的符号。公开 API 已全部补齐
    # 标注（自由函数/导出类），不放 WINDOWS_EXPORT_ALL_SYMBOLS 自动导出：
    # 那会把内部辅助、STL 实例化甚至第三方符号一起导出，DLL 面积膨胀
    # 且与依赖方符号冲突风险高
else()
    add_library(libmini STATIC ${LIBMINI_SOURCES})
    target_compile_definitions(libmini PRIVATE LIBMINI_STATIC)
endif()

# 导出时的目标名固定为 libmini::libmini（不受命名空间/别名影响）
add_library(libmini::libmini ALIAS libmini)

target_include_directories(libmini PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# 版本号：与 CPack/install(EXPORT) 一致（根 CMakeLists 定义）
if(NOT DEFINED LIBMINI_VERSION)
    set(LIBMINI_VERSION 1.0.0)
endif()
set_target_properties(libmini PROPERTIES
    VERSION   ${LIBMINI_VERSION}
    SOVERSION ${LIBMINI_VERSION}
    EXPORT_NAME libmini
)

# 静态库消费方需要 LIBMINI_STATIC（否则 libmini.h 把 LIBMINI_API 展开为 dllimport）
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(libmini PUBLIC LIBMINI_STATIC)
endif()

# 链接第三方库（均被实际使用：json/xml/rpc 日志/rpc 传输/crc+gzip/测试）
find_package(nlohmann_json REQUIRED)
find_package(pugixml REQUIRED)
find_package(spdlog REQUIRED)
find_package(httplib REQUIRED)

# cpp-httplib 的 conan 包给消费者强制注入 CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO
#（包的 package_info 硬编码，非选项）。该路径要求 Win8+ 的异步 DNS API
#（GetAddrInfoExCancel 等），MinGW 头文件下编不过。剥掉它回到阻塞式
# getaddrinfo 默认路径——MSVC/MinGW 通用，行为差异仅 DNS 解析内部实现
if(TARGET httplib::httplib)
    get_target_property(_httplib_defs httplib::httplib
        INTERFACE_COMPILE_DEFINITIONS)
    if(_httplib_defs)
        # 定义可能包在 $<$<CONFIG:...>:...> genex 里追加，需逐元素过滤
        set(_httplib_filtered "")
        foreach(_d ${_httplib_defs})
            if(NOT _d MATCHES "NON_BLOCKING_GETADDRINFO")
                list(APPEND _httplib_filtered "${_d}")
            endif()
        endforeach()
        set_target_properties(httplib::httplib PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${_httplib_filtered}")
    endif()
endif()
find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)
target_link_libraries(libmini PUBLIC
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    spdlog::spdlog    httplib::httplib
    ZLIB::ZLIB
    SQLite::SQLite3
)

if(WIN32)
    # bcrypt：aes_gcm 的 CNG 实现（MSVC 由 pragma comment 自动链接，
    # MinGW 不支持该 pragma，需显式链接）
    target_link_libraries(libmini PUBLIC ws2_32 bcrypt)
else()
    # OpenSSL：aes_gcm 的 EVP 实现。CONFIG 优先（conan 的 CMakeDeps 只在
    # generators 目录生成 config 文件，FindOpenSSL 模块模式搜不到包体）；
    # 找不到 config 再回退系统模块模式（OpenSSL::Crypto）
    find_package(OpenSSL CONFIG QUIET)
    if(NOT TARGET openssl::openssl)
        find_package(OpenSSL REQUIRED)
    endif()
    if(TARGET openssl::openssl)
        target_link_libraries(libmini PUBLIC openssl::openssl)
    else()
        target_link_libraries(libmini PUBLIC OpenSSL::Crypto)
    endif()

    # console 模块的 pthread_sigmask/sigwait（多数系统 libstdc++ 已带，
    # 显式声明保证 Linux/macOS 均链接正确）
    find_package(Threads REQUIRED)
    target_link_libraries(libmini PUBLIC Threads::Threads)
endif()

# ---------------- 安装规则（下游 find_package(libmini) 支持） ----------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 伞头文件 + 全部模块头（保持 utils/ 目录结构）
install(TARGETS libmini
    EXPORT libminiTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # DLL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}   # 静态库/导入库
    COMPONENT devel
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT devel
    FILES_MATCHING PATTERN "*.h"
)

# 导出 target 集合（libminiTargets.cmake）
install(EXPORT libminiTargets
    FILE libminiTargets.cmake
    NAMESPACE libmini::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmini
    COMPONENT devel
)

# libminiConfig.cmake：由模板生成，写版本号
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/libminiConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libminiConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmini
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/libminiConfigVersion.cmake
    VERSION ${LIBMINI_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libminiConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libminiConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmini
    COMPONENT devel
)