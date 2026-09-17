cmake_minimum_required(VERSION 3.25)
project(MiderHive VERSION 1.1.1 LANGUAGES C CXX)

# ---- 版本号：默认取 project(VERSION)，发布流水线用 -DMIDERHIVE_VERSION=v1.2.3 覆盖 ----
# 缓存陷阱：set(... CACHE) 在重配置时不会覆盖既有缓存值——project(VERSION) 升级后，
# 开发构建树会一直用首次配置时写进去的旧默认值（实测自报 1.0.1，而全新配置的发布
# 树是新的）。靠 _MIDERHIVE_LAST_PROJECT_VERSION 识别"缓存里存的还是旧 project 默认
# 值"的场景并强制跟随新版；用户 -D 显式覆盖（值不同于旧默认）不受影响。注意不能用
# "版本变化就 unset" 的写法：-D 的值在本 configure 一开始就已写入缓存，无条件 unset
# 会把首次配置时的 -D 覆盖整个吞掉（实测：全新发布构建被打上 project 版本而非 tag 版本）。
set(MIDERHIVE_VERSION "${PROJECT_VERSION}" CACHE STRING "发布版本（可带 v 前缀与预发布后缀）")
if(DEFINED _MIDERHIVE_LAST_PROJECT_VERSION
   AND NOT _MIDERHIVE_LAST_PROJECT_VERSION STREQUAL "${PROJECT_VERSION}"
   AND MIDERHIVE_VERSION STREQUAL "${_MIDERHIVE_LAST_PROJECT_VERSION}")
    set(MIDERHIVE_VERSION "${PROJECT_VERSION}" CACHE STRING "发布版本（可带 v 前缀与预发布后缀）" FORCE)
endif()
set(_MIDERHIVE_LAST_PROJECT_VERSION "${PROJECT_VERSION}" CACHE INTERNAL "跟踪 project(VERSION) 变化，见上")
# 传参校验：PowerShell 5.1 会把未加引号的 -DMIDERHIVE_VERSION=1.0.0 截断成 "...=1"，
# 于是静默产出一个错误版本号（界面/API/安装包全错）。这类失败模式最难发现，
# 因此这里宁可 configure 直接失败，并给出正确写法。
# v/V 前缀都接受（大小写随后统一归一），但前缀之后必须是 主.次.修订[...]
if(NOT MIDERHIVE_VERSION MATCHES "^[vV]?[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR
        "MIDERHIVE_VERSION 必须是 [vV]?主.次.修订 形式，当前收到 '${MIDERHIVE_VERSION}'。\n"
        "提示：PowerShell 5.1 会截断未加引号的 -DMIDERHIVE_VERSION=1.0.0，"
        "请写成 \"-DMIDERHIVE_VERSION=1.0.0\"（整体加引号），或直接用 scripts/package.ps1。")
endif()
# ---- 统一口径：无论传入 v1.0.4 / V1.0.4 / 1.0.4，内部一律归一为无前缀形式 ----
# version.h(kPlatformVersion)、/api/health、MCP serverInfo、界面 "v%1" 拼接共用
# 这一个形式。否则发布构建把原始 tag（带 v）一路传进来、开发构建取 project(VERSION)
# （不带 v），同一份界面就会拼出 "vv1.0.3"，跳过版本这类字符串比较也会两边口径漂移。
# （更新器的数值比较本就容忍前缀；这里把其余消费端一并统一，"共用"优于各处"区分"。）
string(SUBSTRING "${MIDERHIVE_VERSION}" 0 1 _ver_first)
if(_ver_first STREQUAL "v" OR _ver_first STREQUAL "V")
    string(SUBSTRING "${MIDERHIVE_VERSION}" 1 -1 MIDERHIVE_VERSION)
endif()
string(REGEX REPLACE "^v" "" _ver_raw "${MIDERHIVE_VERSION}")
# 允许 "1.2.3-rc1"：MSI 的 ProductVersion 必须是纯 x.y.z 数字形式
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" MIDERHIVE_VERSION_NUMERIC "${_ver_raw}")
string(REPLACE "." ";" _ver_parts "${MIDERHIVE_VERSION_NUMERIC}")
list(GET _ver_parts 0 MIDERHIVE_VERSION_MAJOR)
list(GET _ver_parts 1 MIDERHIVE_VERSION_MINOR)
list(GET _ver_parts 2 MIDERHIVE_VERSION_PATCH)
message(STATUS "MiderHive version: ${MIDERHIVE_VERSION} (installer: ${MIDERHIVE_VERSION_NUMERIC})")

# ---- exe 的 VERSIONINFO 资源：每个可执行文件一份，配置期由 version.rc.in 生成 ----
# 此前四个 exe 都没有版本资源：文件属性 → 详细信息里的产品版本/文件版本全是空白，
# 资源管理器"产品版本"列与部署/遥测工具同样读不到。版本号仍以 version.h 为单一来源，
# 这里只是把同一个值写进 PE 资源，避免又出现"三处版本各说各话"。
function(miderhive_add_version_resource target internal_name file_desc original_name)
    set(MIDERHIVE_INTERNAL_NAME "${internal_name}")
    set(MIDERHIVE_FILE_DESC "${file_desc}")
    set(MIDERHIVE_ORIGINAL_NAME "${original_name}")
    set(_rc "${CMAKE_BINARY_DIR}/generated/version_${target}.rc")
    configure_file(${CMAKE_SOURCE_DIR}/version.rc.in ${_rc} @ONLY)
    target_sources(${target} PRIVATE ${_rc})
endfunction()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# 源码为 UTF-8（含中文注释）；MSVC 默认按 ANSI 代码页解析会破坏注释后的换行
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(BUILD_GUI   "Build the Qt workbench" ON)
option(BUILD_CLI   "Build agent-cli and platformd" ON)
option(BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

# 第三方依赖一律不启用它们自己的 install 规则：否则 `cmake --install` 会把
# cpp-httplib 的头文件、cmake config 与文档一起拷进发行目录（曾污染出 include/、
# lib/cmake/、share/ 三棵目录树）。依赖以源码内联进本工程，不对外分发它们的开发文件。
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

# ---- third-party dependencies ---------------------------------------------
# 网络韧性：scripts/fetch-deps.ps1 可把 GitHub 依赖预取到 vendor/（带重试）。
# vendor 目录存在且用户未显式指定时才启用本地缓存：
#   - 不用 FORCE，避免覆盖 -DFETCHCONTENT_SOURCE_DIR_<NAME>=... 的显式取值
#   - 仅在缓存变量为空时设置，vendor/ 删除后不会残留悬空路径
if(EXISTS ${CMAKE_SOURCE_DIR}/vendor/sqlite-vec AND NOT FETCHCONTENT_SOURCE_DIR_SQLITE_VEC)
    set(FETCHCONTENT_SOURCE_DIR_SQLITE_VEC ${CMAKE_SOURCE_DIR}/vendor/sqlite-vec CACHE PATH "local vendor")
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib AND NOT FETCHCONTENT_SOURCE_DIR_HTTPLIB)
    set(FETCHCONTENT_SOURCE_DIR_HTTPLIB ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib CACHE PATH "local vendor")
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/vendor/json AND NOT FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON)
    set(FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON ${CMAKE_SOURCE_DIR}/vendor/json CACHE PATH "local vendor")
endif()

# SQLite official amalgamation (pinned)
# The amalgamation archive extracts into a versioned top-level directory
# (sqlite-amalgamation-<version>/), so the version is kept in one place and
# reused for both the download URL and the on-disk source path.
set(SQLITE_AMALGAMATION_VERSION 3460100)
set(SQLITE_AMALGAMATION_DIR "sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}")
set(SQLITE_AMALGAMATION_URL
    "https://www.sqlite.org/2024/sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}.zip")
FetchContent_Declare(sqlite
    URL ${SQLITE_AMALGAMATION_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# sqlite-vec: vector search extension, compiled statically into the app
FetchContent_Declare(sqlite_vec
    GIT_REPOSITORY https://github.com/asg017/sqlite-vec.git
    GIT_TAG v0.1.6)
# nlohmann/json
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
# cpp-httplib (header-only HTTP server/client)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.16.3)

FetchContent_MakeAvailable(sqlite sqlite_vec nlohmann_json httplib)

# The amalgamation zip root is a versioned directory; some CMake versions
# flatten it on extraction, so resolve the real source dir at configure time.
if(EXISTS "${sqlite_SOURCE_DIR}/sqlite3.c")
    set(SQLITE_SRC_DIR "${sqlite_SOURCE_DIR}")
else()
    set(SQLITE_SRC_DIR "${sqlite_SOURCE_DIR}/${SQLITE_AMALGAMATION_DIR}")
endif()

add_library(sqlite3_static STATIC ${SQLITE_SRC_DIR}/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC ${SQLITE_SRC_DIR})
# FTS5：关键词检索的 trigram 全文索引依赖它。amalgamation 默认不带，必须显式打开；
# PUBLIC 让包含 sqlite3.h 的翻译单元与库的宏口径保持一致。
target_compile_definitions(sqlite3_static PUBLIC SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(sqlite3_static PRIVATE /W0)
else()
    target_compile_options(sqlite3_static PRIVATE -w)
endif()

# sqlite-vec statically linked into the same binary that owns the connection.
# Its sqlite-vec.h is a template with ${VERSION} placeholders -- generate it.
file(READ ${sqlite_vec_SOURCE_DIR}/sqlite-vec.h.tmpl _vec_h_tmpl)
set(_d "$")
string(REPLACE "${_d}{VERSION}"       "0.1.6"       _vec_h_tmpl "${_vec_h_tmpl}")
string(REPLACE "${_d}{VERSION_MAJOR}" "0"           _vec_h_tmpl "${_vec_h_tmpl}")
string(REPLACE "${_d}{VERSION_MINOR}" "1"           _vec_h_tmpl "${_vec_h_tmpl}")
string(REPLACE "${_d}{VERSION_PATCH}" "6"           _vec_h_tmpl "${_vec_h_tmpl}")
string(REPLACE "${_d}{DATE}"          "2024-08-21"  _vec_h_tmpl "${_vec_h_tmpl}")
string(REPLACE "${_d}{SOURCE}"        "sqlite-vec.c" _vec_h_tmpl "${_vec_h_tmpl}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/sqlite-vec.h "${_vec_h_tmpl}")

add_library(sqlite_vec_static STATIC ${sqlite_vec_SOURCE_DIR}/sqlite-vec.c)
# 注意：sqlite_vec_SOURCE_DIR 不能进 PUBLIC include 路径 —— 目录里的无扩展名
# VERSION 文件会在 C++20 下遮蔽标准头 <version>。头文件从 generated 目录取。
target_include_directories(sqlite_vec_static PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${SQLITE_SRC_DIR})
target_compile_definitions(sqlite_vec_static PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
if(MSVC)
    target_compile_options(sqlite_vec_static PRIVATE /W0)
else()
    target_compile_options(sqlite_vec_static PRIVATE -w)
endif()

add_library(httplib_hdr INTERFACE)
target_include_directories(httplib_hdr INTERFACE ${httplib_SOURCE_DIR})
if(WIN32)
    target_link_libraries(httplib_hdr INTERFACE ws2_32)
endif()
# cpp-httplib 在非 Windows 平台依赖 pthreads
find_package(Threads REQUIRED)
target_link_libraries(httplib_hdr INTERFACE Threads::Threads)

add_subdirectory(src/core)

if(BUILD_CLI)
    add_subdirectory(src/cli)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_GUI)
    find_package(Qt6 QUIET COMPONENTS Widgets Network Svg)
    if(Qt6_FOUND)
        add_subdirectory(src/gui)
    else()
        message(WARNING "Qt6 not found -- workbench GUI skipped. "
                "Point CMAKE_PREFIX_PATH to your Qt kit (e.g. C:/Qt/6.8.3/msvc2022_64).")
    endif()
endif()
