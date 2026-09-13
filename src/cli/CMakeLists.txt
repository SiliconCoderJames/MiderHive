add_executable(agent-cli main.cpp)
target_link_libraries(agent-cli PRIVATE miderhive_core)
miderhive_add_version_resource(agent-cli "agent-cli" "MiderHive agent CLI" "agent-cli.exe")

add_executable(platformd serverd.cpp)
target_link_libraries(platformd PRIVATE miderhive_core)
miderhive_add_version_resource(platformd "platformd" "MiderHive platform daemon" "platformd.exe")

# MCP 服务器：stdio 子进程，把平台协作面以 MCP 工具暴露给支持 MCP 的 AI agent
add_executable(miderhive-mcp mcp_main.cpp)
target_link_libraries(miderhive-mcp PRIVATE miderhive_core)
miderhive_add_version_resource(miderhive-mcp "miderhive-mcp" "MiderHive MCP server (stdio)"
                               "miderhive-mcp.exe")

# 三个命令行程序与工作台装在同一目录：ZIP 与 MSI 都从 cmake --install 的暂存目录取材，
# 装完后 agent-cli / platformd / miderhive-mcp 可直接在 PATH 上调用（MSI 会把它加入用户 PATH）。
install(TARGETS agent-cli platformd miderhive-mcp RUNTIME DESTINATION . COMPONENT Runtime)
