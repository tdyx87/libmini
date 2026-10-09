set(LIBMINI_SOURCES
    libmini.cpp
    utils/string_utils.cpp
    utils/string_algo.cpp
    utils/time_utils.cpp
    utils/stopwatch.cpp
    utils/file_utils.cpp
    utils/file_lock.cpp
    utils/metrics.cpp
    utils/mmap_file.cpp
    utils/dir_watcher.cpp
    utils/win_service.cpp
    utils/path_utils.cpp
    utils/thread_utils.cpp
    utils/json_utils.cpp
    utils/xml_utils.cpp
    utils/serialization.cpp
    utils/msgpack.cpp
    utils/proto_buf.cpp
    utils/rpc.cpp
    utils/uuid.cpp
    utils/crc.cpp
    utils/encoding.cpp
    utils/charset.cpp
    utils/grapheme.cpp
    utils/random_utils.cpp
    utils/secure_random.cpp
    utils/env.cpp    utils/digest.cpp
    utils/blake3.cpp
    utils/hmac.cpp
    utils/kdf.cpp
    utils/process.cpp    utils/base32.cpp
    utils/console.cpp
    utils/tcp.cpp
    utils/trace.cpp
    utils/zip.cpp
    utils/tar.cpp
    utils/zstd.cpp
    utils/aes_gcm.cpp
    utils/ini_config.cpp
    utils/gzip.cpp
    utils/async.cpp
    utils/args.cpp
    utils/csv.cpp
    utils/glob.cpp
    utils/sqlite.cpp
    utils/system_info.cpp
    utils/hardware_info.cpp
    utils/machine_fingerprint.cpp
    utils/http_client.cpp
    utils/http_server.cpp
    utils/log_facade.cpp
    utils/config_facade.cpp
    utils/net_addr.cpp
    utils/websocket.cpp
    utils/circuit_breaker.cpp
    utils/timer_wheel.cpp
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
    F:/conan/p/nlohm0567ffc90cfc1/p/include
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

# 字形簇属性表：默认用内置的启发式表（无外部依赖）；打开本选项换成
# ci/gen_grapheme_tables.py 从 UCD 生成的完整表（src/utils/grapheme_tables_full.inc，
# 随仓库提交，编译时不需要联网），与 UAX #29 逐条对齐。
# 作为 PUBLIC 定义传出，保证所有翻译单元与下游消费者看到同一份表。
if(LIBMINI_UNICODE_FULL_GRAPHEME)
    target_compile_definitions(libmini PUBLIC LIBMINI_UNICODE_FULL_GRAPHEME=1)
    message(STATUS "libmini: grapheme clusters use the full UAX #29 UCD tables")
else()
    message(STATUS "libmini: grapheme clusters use the built-in heuristic tables")
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
find_package(zstd REQUIRED)
target_link_libraries(libmini PUBLIC
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    spdlog::spdlog    httplib::httplib
    ZLIB::ZLIB
    SQLite::SQLite3
    zstd::libzstd_static
)

if(WIN32)
    # bcrypt：aes_gcm 的 CNG 实现（MSVC 由 pragma comment 自动链接，
    # MinGW 不支持该 pragma，需显式链接）
    # iphlpapi：hardware_info 的 GetAdaptersAddresses（网卡/MAC/IP）
    target_link_libraries(libmini PUBLIC ws2_32 bcrypt iphlpapi)
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
        # 模块模式下 ssl+crypto 都要：httplib 的 TLS 路径链 libssl
        target_link_libraries(libmini PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    endif()

    # console 模块的 pthread_sigmask/sigwait（多数系统 libstdc++ 已带，
    # 显式声明保证 Linux/macOS 均链接正确）
    find_package(Threads REQUIRED)
    target_link_libraries(libmini PUBLIC Threads::Threads)

    # charset 的 GBK ↔ Unicode 转换在非 Windows 平台走 iconv：glibc 内建
    # （Iconv::Iconv 是空接口目标，不引入额外链接），其余平台由 libiconv 提供。
    # Windows 侧用 Win32 代码页 API，不依赖 iconv
    find_package(Iconv REQUIRED)
    target_link_libraries(libmini PUBLIC Iconv::Iconv)
endif()

# ---------------- TLS（httplib 的 SSL 客户端/服务端） ----------------
# CPPHTTPLIB_OPENSSL_SUPPORT 打开 httplib 的 SSLClient/SSLServer。必须 PUBLIC：
# 消费者若直接包含 httplib.h，要与本库看到完全一致的类布局（内联版本漂移
# 是 ODR 雷）。非 Windows 恒有 conan openssl（上方 aes_gcm 段已 find 并链上）；
# Windows 查系统 OpenSSL（winget/Strawberry，可用 OPENSSL_ROOT_DIR 提示），
# 查不到就整体关闭——https 请求在运行期给出明确错误，相关测试自动跳过
set(_libmini_ssl OFF)
if(WIN32)
    find_package(OpenSSL CONFIG QUIET)
    if(NOT TARGET openssl::openssl)
        find_package(OpenSSL QUIET)  # 模块模式搜系统安装/OPENSSL_ROOT_DIR
    endif()
    if(TARGET openssl::openssl)
        target_link_libraries(libmini PUBLIC openssl::openssl)
        set(_libmini_ssl ON)
    elseif(TARGET OpenSSL::SSL)
        target_link_libraries(libmini PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        set(_libmini_ssl ON)
    endif()
else()
    # aes_gcm 段已链接；openssl::openssl 聚合 ssl+crypto，模块模式补 ssl
    if(TARGET openssl::openssl)
        set(_libmini_ssl ON)
    elseif(TARGET OpenSSL::SSL)
        target_link_libraries(libmini PUBLIC OpenSSL::SSL)
        set(_libmini_ssl ON)
    endif()
endif()
if(_libmini_ssl)
    target_compile_definitions(libmini PUBLIC
        CPPHTTPLIB_OPENSSL_SUPPORT
        LIBMINI_SSL_SUPPORT
    )
    message(STATUS "libmini: TLS/HTTPS enabled (OpenSSL found)")
else()
    message(STATUS "libmini: TLS/HTTPS disabled (OpenSSL not found; https reports a runtime error)")
endif()
# 供 libminiConfig.cmake.in 决定消费者是否需要 find_dependency(OpenSSL)：
# POSIX 恒链接（aes_gcm）；Windows 仅 TLS 启用时导出目标才引用 OpenSSL::
set(LIBMINI_WITH_OPENSSL ${_libmini_ssl})

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
    # 内部实现细节（共享的 utf8 解码器）不对外安装
    PATTERN "utils/utf8_codec.h" EXCLUDE
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