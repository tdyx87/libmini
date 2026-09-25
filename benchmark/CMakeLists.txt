# 基准工具（不进 CTest；example 同款链接方式）
add_executable(tcp_bench tcp_bench.cpp)
target_include_directories(tcp_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(LIBMINI_BUILD_SHARED)
    target_compile_definitions(tcp_bench PRIVATE LIBMINI_BUILD_SHARED)
else()
    target_compile_definitions(tcp_bench PRIVATE LIBMINI_STATIC)
endif()

target_link_libraries(tcp_bench PRIVATE libmini)
