# ---- GUI 源编成静态库：工作台 exe 与 GUI 全链路自测 exe（gui_selftest）共用 ----
# 自测跑的就是发布代码本身（真实 MainWindow/WelcomeDialog/SettingsDialog + 同一套面板），
# 而不是另写一份复刻流程——"自测通过"与"用户看到的行为"永远同源。
# 注意：Qt 资源（:/theme、:/brand）挂在 exe 上而不是静态库上——静态库里的 rcc 初始化
# 对象可能被链接器丢弃；exe 直连自己的资源对象没有这个问题。
add_library(miderhive_gui STATIC
    mainwindow.cpp
    settings_dialog.cpp
    connect_dialog.cpp
    welcome_dialog.cpp
    update_checker.cpp
    panels/dashboard_panel.cpp
    panels/usage_panel.cpp
    panels/knowledge_panel.cpp
    panels/skills_panel.cpp
    panels/memory_panel.cpp
    panels/messages_panel.cpp
    panels/errors_panel.cpp
    panels/logs_panel.cpp
)
target_link_libraries(miderhive_gui PUBLIC miderhive_core Qt6::Widgets Qt6::Network Qt6::Svg)
set_target_properties(miderhive_gui PROPERTIES AUTOMOC ON)

add_executable(miderhive WIN32
    main.cpp
    app.rc
)
target_link_libraries(miderhive PRIVATE miderhive_gui)
miderhive_add_version_resource(miderhive "miderhive" "MiderHive workbench" "miderhive.exe")
set_target_properties(miderhive PROPERTIES AUTOMOC ON AUTORCC ON)
qt_add_resources(miderhive "theme"
    PREFIX "/theme"
    FILES qss/dark.qss)
target_sources(miderhive PRIVATE brand.qrc)  # :/brand/logo.png（qt_add_resources 对源外文件会生成坏 alias，见 brand.qrc 注释）

# ---- GUI 全链路自测（离屏）：引导 → 上线 → 欢迎记忆 → 密钥丢失 → 横幅修复 → 设置重入 ----
# 由 scripts/verify_gui_selftest.py 拉起（提供隔离的 MIDERHIVE_HOME/PORT、offscreen
# 平台与 Qt 运行时 PATH）。控制台子系统：报告走 stdout 供脚本断言退出码。
if(BUILD_TESTS)
    add_executable(gui_selftest
        selftest_main.cpp
    )
    target_link_libraries(gui_selftest PRIVATE miderhive_gui)
    set_target_properties(gui_selftest PROPERTIES AUTOMOC ON AUTORCC ON)
    qt_add_resources(gui_selftest "theme"
        PREFIX "/theme"
        FILES qss/dark.qss)
    target_sources(gui_selftest PRIVATE brand.qrc)
endif()

# ---- 可复现的 Qt 运行时部署 ----
# 此前项目里没有任何 windeployqt 调用，Qt6*.dll 与 platforms/tls/... 插件是人工拷进
# 构建目录的：换台机器或清空 build 就装不出能跑的包。
# 现在 `cmake --install <build> --config Release --prefix <stage>` 产出扁平且可直接运行的
# 目录（exe + Qt DLL + 插件目录 + MSVC 运行库同层），ZIP 与 MSI 都从它取材。
# 不用 qt_generate_deploy_app_script 的原因：它按 Qt 的安装布局输出 bin/ 与 plugins/
# 多级结构，还会带上 include/ 与 lib/cmake 等开发文件，不适合直接打包分发。
if(WIN32)
    install(TARGETS miderhive RUNTIME DESTINATION . COMPONENT Runtime)
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake_exe}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin}" REQUIRED)

    # MSVC 运行库：用 CMake 官方机制定位可再分发文件（不依赖 VCToolsRedistDir 环境变量，
    # 那个变量只在 VS 开发者环境里才有）。SKIP 掉 CMake 自带的 install 规则，
    # 改由我们挂到 Runtime 组件上，保证 `cmake --install --component Runtime` 一次装齐。
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                DESTINATION . COMPONENT Runtime)
        list(LENGTH CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS _crt_n)
        message(STATUS "MSVC runtime to ship: ${_crt_n} file(s)")
    else()
        message(WARNING
            "CMake 未能定位 MSVC 可再分发运行库：目标机将需要预装 VC++ Redistributable。"
            "发行打包前请确认暂存目录里存在 vcruntime140.dll。")
    endif()

    # --no-translations  : 界面文案走自研 i18n，不用 Qt .qm
    # --no-opengl-sw / --no-system-d3d-compiler : Widgets 光栅渲染用不到，可省近 20MB
    install(CODE "
        message(STATUS \"windeployqt -> \${CMAKE_INSTALL_PREFIX}\")
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                    --release --no-translations --no-opengl-sw --no-system-d3d-compiler
                    --dir \"\${CMAKE_INSTALL_PREFIX}\"
                    \"\${CMAKE_INSTALL_PREFIX}/miderhive.exe\"
            RESULT_VARIABLE _wdq_rc)
        if(NOT _wdq_rc EQUAL 0)
            message(FATAL_ERROR \"windeployqt 失败，退出码 \${_wdq_rc}\")
        endif()
    " COMPONENT Runtime)
endif()

# 第三方许可与通知随包分发（Qt 为 LGPLv3，二进制分发必须随附许可文本）
install(FILES
    ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
    ${CMAKE_SOURCE_DIR}/LICENSE
    DESTINATION licenses
    COMPONENT Runtime)
