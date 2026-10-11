# libmini 测试聚合器：把 test/ 下的各 gtest 套件集成到一个程序里运行，
# 提供终端实时进度与浏览器仪表板（复用库本体的 HttpServer）。
#
# 定位是开发工具而非测试：不注册 add_test（避免 ctest 递归调用自己）。它依赖
# test/ 里的测试目标，故根 CMakeLists 只在 BUILD_TESTING 打开时加入本目录。
add_executable(test_runner
    test_runner.cpp
    tr_model.cpp
    tr_dashboard.cpp)
target_include_directories(test_runner PRIVATE ${CMAKE_SOURCE_DIR}/tools ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(test_runner PRIVATE LIBMINI_STATIC)
endif()

# 各套件的可执行文件路径：用生成器表达式在生成期解析为绝对路径，运行期不依赖
# 工作目录（与 test/CMakeLists.txt 注入 test_sqlite_child 路径同一手法）。
target_compile_definitions(test_runner PRIVATE
    LIBMINI_TR_SUITE_libmini="$<TARGET_FILE:test_libmini>"
    LIBMINI_TR_SUITE_rpc="$<TARGET_FILE:test_rpc>"
    LIBMINI_TR_SUITE_utils="$<TARGET_FILE:test_utils>"
    LIBMINI_TR_SUITE_common="$<TARGET_FILE:test_common>"
    LIBMINI_TR_SUITE_args="$<TARGET_FILE:test_args>")

# 构建聚合器时顺带把这些测试目标也构建出来，否则首次运行会找不到可执行文件
add_dependencies(test_runner test_libmini test_rpc test_utils test_common test_args)

target_link_libraries(test_runner PRIVATE libmini)
