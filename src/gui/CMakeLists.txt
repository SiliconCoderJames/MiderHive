add_executable(miderhive WIN32
    main.cpp
    mainwindow.cpp
    settings_dialog.cpp
    connect_dialog.cpp
    welcome_dialog.cpp
    update_checker.cpp
    app.rc
    panels/dashboard_panel.cpp
    panels/usage_panel.cpp
    panels/knowledge_panel.cpp
    panels/skills_panel.cpp
    panels/memory_panel.cpp
    panels/messages_panel.cpp
    panels/errors_panel.cpp
    panels/logs_panel.cpp
)
target_link_libraries(miderhive PRIVATE miderhive_core Qt6::Widgets Qt6::Network Qt6::Svg)
miderhive_add_version_resource(miderhive "miderhive" "MiderHive workbench" "miderhive.exe")
set_target_properties(miderhive PROPERTIES AUTOMOC ON AUTORCC ON)
qt_add_resources(miderhive "theme"
    PREFIX "/theme"
    FILES qss/dark.qss)
qt_add_resources(miderhive "brand"
    PREFIX "/brand"
    FILES ../../docs/assets/logo.png)

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
