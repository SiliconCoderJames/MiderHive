add_library(miderhive_core STATIC
    util.cpp
    db/database.cpp
    embed/embedder.cpp
    services/agent_service.cpp
    services/knowledge_service.cpp
    services/skill_service.cpp
    services/memory_service.cpp
    services/message_service.cpp
    services/error_service.cpp
    services/usage_service.cpp
    services/audit_service.cpp
    platform.cpp
    http/server.cpp
)
target_include_directories(miderhive_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# 与 sqlite3_static / sqlite_vec_static 一致：静态编译进进程，不走扩展加载机制
target_compile_definitions(miderhive_core PUBLIC SQLITE_CORE)
if(MSVC)
    # getenv 等 CRT 安全告警（C4996）在本地单机场景为预期用法
    target_compile_definitions(miderhive_core PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
target_link_libraries(miderhive_core PUBLIC
    sqlite3_static
    sqlite_vec_static
    nlohmann_json::nlohmann_json
    httplib_hdr
)

# 把 schema.sql 嵌入二进制
# CMAKE_CONFIGURE_DEPENDS：file(READ) 不会让 CMake 感知输入变化，改了 schema.sql
# 却不重新 configure 的话，嵌入的仍是旧 schema（实测：加 FTS 表后启动报 no such table）。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/db/schema.sql)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/db/schema.sql SCHEMA_SQL_CONTENT)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/db/schema_sql.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/schema_sql.h @ONLY)
target_include_directories(miderhive_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)

# 版本号单一来源 → 生成 version.h（界面、/api/health、安装包共用）
# 放在 generated/core/ 下：以 generated 为 include 根，代码里写 #include "core/version.h"，
# 避免 "version.h" 这种通用名与 C++20 <version> 之类在同一 include 路径上撞名。
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/core)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h @ONLY)
