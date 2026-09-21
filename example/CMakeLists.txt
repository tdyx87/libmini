# libmini 功能演示程序（控制台，用 Args 解析自己的入口参数）
add_executable(example demo_libmini.cpp)
target_include_directories(example PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(example PRIVATE LIBMINI_STATIC)
endif()
target_link_libraries(example PRIVATE libmini)