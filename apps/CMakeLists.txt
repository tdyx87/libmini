# libmini 示例应用：把库拼成 6 个能直接跑的独立程序。
# 每个应用都带 --demo 自检模式（绑 0 端口 + 临时目录），因此可在 CI 上重复执行，
# 并注册成 ctest 用例实时验证「库与应用都还活着」。
#
# 它们只是库的消费者：不安装、不参与 CPack；用 -DLIBMINI_BUILD_APPS=OFF 可整体关闭。
# 原生 UI 栈（native_ui / child_process / ui_panels）已经是库模块，由 libmini
# 提供头与实现，这里只放应用自己的共享层 app_common。
# 非 Windows 平台 native_ui 没有窗口后端（但保留布局代码），
# 因此三平台 CI 不必为 GUI 单独开路。
set(APP_TARGETS
    jobsvc
    sysmon
    lanshare
    apitest
    passvault
    svcframe
    launcher)

add_library(app_common STATIC
    app_common.cpp)
target_include_directories(app_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
if(NOT LIBMINI_BUILD_SHARED)
    target_compile_definitions(app_common PUBLIC LIBMINI_STATIC)
endif()
target_link_libraries(app_common PUBLIC libmini)
if(WIN32)
    # 原生 UI 的 user32/gdi32 由 libmini 自己带上；这里只剩启动器「打开浏览器」
    # 用的 shell32
    target_link_libraries(app_common PUBLIC shell32)
endif()

foreach(app IN LISTS APP_TARGETS)
    add_executable(${app} ${app}.cpp)
    target_link_libraries(${app} PRIVATE app_common)
    if(BUILD_TESTING)
        add_test(NAME apps_${app}_demo COMMAND ${app} --demo)
        set_tests_properties(apps_${app}_demo PROPERTIES TIMEOUT 300)
    endif()
endforeach()

# 原生窗口自检（Windows 专属）：真的建窗、跑固定帧数、截图、断言面板连上了被托管的
# 服务。非 Windows 平台不注册（窗口后端不存在）；即使注册了也只是打印「跳过」并返回 0。
if(WIN32 AND BUILD_TESTING)
    add_test(NAME apps_launcher_ui COMMAND $<TARGET_FILE:launcher> --ui-selftest)
    set_tests_properties(apps_launcher_ui PROPERTIES TIMEOUT 300)
    foreach(app IN ITEMS jobsvc sysmon lanshare svcframe apitest passvault)
        add_test(NAME apps_${app}_ui COMMAND ${app} --ui-selftest)
        set_tests_properties(apps_${app}_ui PROPERTIES TIMEOUT 300)
    endforeach()
endif()
