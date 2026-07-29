cmake_minimum_required(VERSION 3.10)
project(ProtoRelay LANGUAGES CXX)

set(PROTORELAY_VERSION "0.1.0-dev" CACHE STRING "ProtoRelay semantic version")

# Prefer classic FindBoost module behavior to avoid Homebrew BoostConfig
# component resolution issues on macOS.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

# ===== 构建配置 =====
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build: Debug/Release" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# 添加编译选项
option(USE_SSL "Enable SSL/TLS support" ON)
option(ENABLE_DEBUG_LOGS "Enable debug logs for all modules" OFF)
option(BUILD_OBJECT_ONLY "Compile object files only and skip final executable link" OFF)
option(ENABLE_INBOUND_DEDUP_CHECK "Enable inbound duplicate check before DB insert" ON)
option(ENABLE_HDFS_WEB_STORAGE "Enable WebHDFS storage provider (requires libcurl)" ON)
option(ENABLE_S3_STORAGE "Enable S3/MinIO storage provider (requires libcurl, OpenSSL)" ON)

if(USE_SSL)
    set(PROTORELAY_FEATURE_SSL 1)
else()
    set(PROTORELAY_FEATURE_SSL 0)
endif()

if(ENABLE_INBOUND_DEDUP_CHECK)
    set(PROTORELAY_FEATURE_INBOUND_DEDUP 1)
else()
    set(PROTORELAY_FEATURE_INBOUND_DEDUP 0)
endif()

if(BUILD_OBJECT_ONLY)
    set(PROTORELAY_FEATURE_OBJECT_ONLY 1)
else()
    set(PROTORELAY_FEATURE_OBJECT_ONLY 0)
endif()

if(ENABLE_HDFS_WEB_STORAGE)
    set(PROTORELAY_FEATURE_HDFS_WEB 1)
    add_compile_definitions(PROTORELAY_ENABLE_HDFS_WEB_STORAGE=1)
else()
    set(PROTORELAY_FEATURE_HDFS_WEB 0)
    add_compile_definitions(PROTORELAY_ENABLE_HDFS_WEB_STORAGE=0)
endif()

string(TIMESTAMP PROTORELAY_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
set(PROTORELAY_BUILD_TARGET "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(PROTORELAY_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE PROTORELAY_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PROTORELAY_GIT_COMMIT)
    set(PROTORELAY_GIT_COMMIT "unknown")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/include/mail_system/back/version/build_info.h.in"
    "${CMAKE_BINARY_DIR}/generated/build_info.h"
    @ONLY
)

# Object-only artifacts are often linked on a different machine where libfmt
# version may differ. Keep fmt implementation header-only to avoid ABI mismatch.
if(BUILD_OBJECT_ONLY)
    add_compile_definitions(FMT_HEADER_ONLY=1)
endif()

# Cross-compilation sysroot: inserted before all other include paths
set(SYSROOT_INCLUDE "${SYSROOT_INCLUDE}" CACHE PATH "Sysroot include path for cross-compilation")

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===== Debug 模式配置 =====
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG -fno-omit-frame-pointer" CACHE STRING "Flags used by C++ compiler during DEBUG builds")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUG -fno-omit-frame-pointer" CACHE STRING "Flags used by C compiler during DEBUG builds")

# ===== Release 模式配置 =====
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native" CACHE STRING "Flags used by C++ compiler during RELEASE builds")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -march=native" CACHE STRING "Flags used by C compiler during RELEASE builds")

# 全局编译选项
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-fdiagnostics-color=auto)
endif()

# ===== SSL 选项 =====
if(USE_SSL)
    add_compile_definitions(USE_SSL)
    message(STATUS "✓ SSL/TLS support ENABLED")
else()
    message(STATUS "✗ SSL/TLS support DISABLED")
endif()

# ===== 日志级别配置 =====
# Debug 模式：默认启用所有 DEBUG 级别日志
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT DEFINED ENABLE_DEBUG_LOGS)
        set(ENABLE_DEBUG_LOGS ON CACHE BOOL "Enable debug logs for all modules" FORCE)
    endif()
    message(STATUS "📝 Debug mode: All debug logs ENABLED")
endif()

# Release/Testing 模式：默认禁用 DEBUG 级别日志，只保留 INFO
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(NOT DEFINED ENABLE_DEBUG_LOGS)
        set(ENABLE_DEBUG_LOGS OFF CACHE BOOL "Enable debug logs for all modules" FORCE)
    endif()
    message(STATUS "📝 Release mode: Debug logs DISABLED (INFO level only)")
endif()

# 根据选项设置编译定义
if(ENABLE_DEBUG_LOGS)
    add_compile_definitions(
        ENABLE_SERVER_DEBUG_LOG=1
        ENABLE_NETWORK_DEBUG_LOG=1
        ENABLE_DATABASE_DEBUG_LOG=1
        ENABLE_DATABASE_QUERY_DEBUG_LOG=1
        ENABLE_SMTP_DEBUG_LOG=1
        ENABLE_SMTP_DETAIL_DEBUG_LOG=1
        ENABLE_SESSION_DEBUG_LOG=1
        ENABLE_THREAD_POOL_DEBUG_LOG=1
        ENABLE_FILE_IO_DEBUG_LOG=1
        ENABLE_AUTH_DEBUG_LOG=1
    )
else()
    add_compile_definitions(
        ENABLE_SERVER_DEBUG_LOG=0
        ENABLE_NETWORK_DEBUG_LOG=0
        ENABLE_DATABASE_DEBUG_LOG=0
        ENABLE_DATABASE_QUERY_DEBUG_LOG=0
        ENABLE_SMTP_DEBUG_LOG=0
        ENABLE_SMTP_DETAIL_DEBUG_LOG=0
        ENABLE_SESSION_DEBUG_LOG=0
        ENABLE_THREAD_POOL_DEBUG_LOG=0
        ENABLE_FILE_IO_DEBUG_LOG=0
        ENABLE_AUTH_DEBUG_LOG=0
    )
endif()

# -----------------------------
# 依赖查找（尽量自动化、跨平台）
# -----------------------------
# Homebrew Boost 1.90+ may ship BoostConfig without per-component config files
# (e.g. boost_systemConfig.cmake), so prefer classic FindBoost module mode.
set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "Disable BoostConfig package mode" FORCE)

# This project uses Boost headers (asio/beast/etc.) and std::thread; avoid
# requiring optional Boost binary components that may be absent in Homebrew.
find_package(Boost REQUIRED)
if(SYSROOT_INCLUDE)
    # 交叉编译：使用 sysroot 的 spdlog/fmt 头文件，不依赖 homebrew
    message(STATUS "Using sysroot spdlog/fmt: ${SYSROOT_INCLUDE}")
    set(spdlog_FOUND TRUE)
    set(spdlog_INCLUDE_DIRS "${SYSROOT_INCLUDE}")
    set(spdlog_LIBRARIES "")  # header-only
    add_compile_definitions(SPDLOG_FMT_EXTERNAL=OFF)  # 使用 spdlog 自带的 bundled fmt
    # 创建 header-only 接口目标，替代 find_package(spdlog)
    if(NOT TARGET spdlog::spdlog)
        add_library(spdlog::spdlog INTERFACE IMPORTED)
        set_target_properties(spdlog::spdlog PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SYSROOT_INCLUDE}"
        )
    endif()
else()
    find_package(spdlog REQUIRED)
endif()
if(ENABLE_HDFS_WEB_STORAGE)
    find_package(CURL REQUIRED)
endif()

if(USE_SSL)
    if(BUILD_OBJECT_ONLY)
        find_package(OpenSSL)
        if(NOT OpenSSL_FOUND)
            message(WARNING "OpenSSL not found in object-only mode. Build will fail if OpenSSL headers are included by sources.")
        endif()
    else()
        find_package(OpenSSL REQUIRED)
    endif()
else()
    # 如果禁用SSL，不查找OpenSSL但仍然定义相关变量以避免CMake错误
    set(OPENSSL_FOUND FALSE)
    set(OPENSSL_INCLUDE_DIR "")
    set(OPENSSL_LIBRARIES "")
endif()

# If OpenSSL not found, on macOS try common Homebrew prefix and re-run find
if(USE_SSL AND NOT OpenSSL_FOUND AND APPLE)
    message(STATUS "OpenSSL not found by CMake; attempting Homebrew prefix...")
    if(EXISTS "/opt/homebrew/opt/openssl")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl" CACHE PATH "OpenSSL root (Homebrew)")
    elseif(EXISTS "/usr/local/opt/openssl")
        set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl" CACHE PATH "OpenSSL root (Homebrew Intel)")
    endif()
    find_package(OpenSSL REQUIRED)
endif()
find_package(Threads REQUIRED)

# ===== MySQL 库查找 (自动推导路径) =====
find_package(PkgConfig)
set(MYSQL_INCLUDE_DIRS "")
set(MYSQL_LIBRARIES "")
set(CARES_INCLUDE_DIRS "")
set(CARES_LIBRARIES "")
set(CARES_LIBRARY_DIRS "")

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_CARES QUIET libcares)
    if(NOT PC_CARES_FOUND)
        pkg_check_modules(PC_CARES QUIET cares)
    endif()
    if(PC_CARES_FOUND)
        set(CARES_INCLUDE_DIRS ${PC_CARES_INCLUDE_DIRS})
        set(CARES_LIBRARY_DIRS ${PC_CARES_LIBRARY_DIRS})
        if(PC_CARES_LINK_LIBRARIES)
            set(CARES_LIBRARIES ${PC_CARES_LINK_LIBRARIES})
        else()
            set(CARES_LIBRARIES ${PC_CARES_LIBRARIES})
        endif()
        message(STATUS "✓ Found c-ares via pkg-config")
    endif()
endif()

if(NOT CARES_LIBRARIES)
    find_path(CARES_INCLUDE_DIR ares.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(CARES_LIBRARY NAMES cares c-ares PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(CARES_INCLUDE_DIR AND CARES_LIBRARY)
        set(CARES_INCLUDE_DIRS ${CARES_INCLUDE_DIR})
        set(CARES_LIBRARIES ${CARES_LIBRARY})
        get_filename_component(CARES_LIBRARY_DIRS ${CARES_LIBRARY} DIRECTORY)
        message(STATUS "✓ Found c-ares library: ${CARES_LIBRARY}")
    else()
        if(BUILD_OBJECT_ONLY)
            if(CARES_INCLUDE_DIR)
                set(CARES_INCLUDE_DIRS ${CARES_INCLUDE_DIR})
                message(WARNING "c-ares library not found in object-only mode; compile-only will continue with headers.")
            else()
                message(WARNING "c-ares headers not found in object-only mode. Build may fail if ares.h is included by sources.")
            endif()
        else()
            message(FATAL_ERROR "c-ares not found. Please install c-ares (e.g. brew install c-ares).")
        endif()
    endif()
endif()

# 第一步：尝试 pkg-config
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_MYSQL QUIET mysqlclient libmysqlclient)
    if(PC_MYSQL_FOUND)
        message(STATUS "✓ Found mysqlclient via pkg-config: ${PC_MYSQL_VERSION}")
        set(MYSQL_INCLUDE_DIRS ${PC_MYSQL_INCLUDE_DIRS})
        set(MYSQL_LIBRARIES ${PC_MYSQL_LIBRARIES})
    endif()
endif()

# 第二步：若 pkg-config 未找到，尝试自动推导
if(NOT MYSQL_LIBRARIES)
    # 智能路径选择
    set(MYSQL_SEARCH_PATHS)
    
    # 苹果硅 Mac (Apple Silicon)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        list(APPEND MYSQL_SEARCH_PATHS "/opt/homebrew/opt/mysql-client")
        list(APPEND MYSQL_SEARCH_PATHS "/opt/homebrew")
    # Intel Mac
    elseif(APPLE)
        list(APPEND MYSQL_SEARCH_PATHS "/usr/local/opt/mysql")
        list(APPEND MYSQL_SEARCH_PATHS "/usr/local")
    endif()
    
    # Linux 通用路径
    list(APPEND MYSQL_SEARCH_PATHS "/usr" "/usr/local" "/opt/mysql")
    
    find_path(MYSQL_INCLUDE_DIR mysql.h
        PATHS ${MYSQL_SEARCH_PATHS}
        PATH_SUFFIXES include include/mysql
    )
    
    find_library(MYSQL_CLIENT_LIB
        NAMES mysqlclient libmysqlclient
        PATHS ${MYSQL_SEARCH_PATHS}
        PATH_SUFFIXES lib lib64
    )
    
    if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT_LIB)
        set(MYSQL_INCLUDE_DIRS "${MYSQL_INCLUDE_DIR}")
        set(MYSQL_LIBRARIES "${MYSQL_CLIENT_LIB}")
        message(STATUS "✓ Found MySQL: ${MYSQL_CLIENT_LIB}")
    else()
        if(BUILD_OBJECT_ONLY)
            if(MYSQL_INCLUDE_DIR)
                set(MYSQL_INCLUDE_DIRS "${MYSQL_INCLUDE_DIR}")
                message(WARNING "mysqlclient library not found in object-only mode; compile-only will continue with headers.")
            else()
                message(WARNING "mysql headers not found in object-only mode. Build may fail if mysql.h is included by sources.")
            endif()
        else()
            message(WARNING "✗ mysqlclient not found. Set manually: -DMYSQL_INCLUDE_DIRS=<path> -DMYSQL_LIBRARIES=<path>")
        endif()
    endif()
endif()

# -----------------------------
# 源文件列表
# -----------------------------
set(COMMON_SRCS
    src/mail_system/back/cli/help_text.cpp
    src/mail_system/back/db/distributed_mysql_pool.cpp
    src/mail_system/back/db/mysql_pool.cpp
    src/mail_system/back/db/mysql_service.cpp
    src/mail_system/back/db/sql_queries.cpp
    src/mail_system/back/mailServer/server_base.cpp
    src/mail_system/back/thread_pool/boost_thread_pool.cpp
    src/mail_system/back/thread_pool/io_thread_pool.cpp
    src/mail_system/back/algorithm/snow.cpp
    src/mail_system/back/storage/distributed_file_storage_provider.cpp
    src/mail_system/back/storage/local_file_storage_provider.cpp
    src/mail_system/back/outbound/outbox_repository.cpp
    src/mail_system/back/outbound/smtp_outbound_client.cpp
    src/mail_system/back/outbound/outbound_utils.cpp
    src/mail_system/back/outbound/mx_routing_utils.cpp
    src/mail_system/back/outbound/cares_dns_resolver.cpp
    src/mail_system/back/common/bcrypt.cpp
    src/mail_system/back/algorithm/smtp_utils.cpp
    src/mail_system/back/inbound/inbound_verifier.cpp
    src/mail_system/back/persist_storage/persistent_queue.cpp
    src/mail_system/back/router/table_shard_router.cpp
    # 显式模板实例化（避免每个翻译单元重复编译模板实现）
    src/mail_system/back/mailServer/session/session_base_inst.cpp
    src/mail_system/back/mailServer/session/smtps_session_inst.cpp
    src/mail_system/back/mailServer/session/imaps_session_inst.cpp
    src/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm_inst.cpp
)

if(ENABLE_HDFS_WEB_STORAGE)
    list(APPEND COMMON_SRCS
        src/mail_system/back/storage/hdfs_web_storage_provider.cpp
    )
endif()
if(ENABLE_S3_STORAGE)
    add_compile_definitions(ENABLE_S3_STORAGE=1)
    list(APPEND COMMON_SRCS
        src/mail_system/back/storage/s3_storage_provider.cpp
    )
endif()

set(SMTP_SRCS
    ${COMMON_SRCS}
    src/mail_system/back/mailServer/smtps/smtps_server.cpp
    src/mail_system/back/mailServer/imaps/imaps_server.cpp
    test/smtps_test.cpp
)

# ===== 输出目录配置 =====
# 可执行文件放到项目根的 test 目录，中间产物放到当前构建目录
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}")

file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Build output: executables → ${TEST_OUTPUT_DIR}")
message(STATUS "Build output: intermediate → ${CMAKE_BINARY_DIR}")

# 构建目标：默认可执行；object-only 模式只编译 .o 不链接。
if(BUILD_OBJECT_ONLY)
    add_library(smtpsServer_obj OBJECT ${SMTP_SRCS})
    set(SMTP_TARGET smtpsServer_obj)
    message(STATUS "Object-only mode enabled: final executable link is skipped")
else()
    add_executable(smtpsServer ${SMTP_SRCS})
    set(SMTP_TARGET smtpsServer)
endif()

# 对象库模式：额外入口点（IMAP/组合服务器），只编译不链接
if(BUILD_OBJECT_ONLY)
    add_library(imapsServer_obj OBJECT test/imaps_test.cpp)
    add_library(mailServer_obj OBJECT test/mail_server_combined.cpp)
endif()

# ---- 单元测试（交叉编译 object-only 模式下跳过） ----
if(NOT BUILD_OBJECT_ONLY)
    add_executable(test_inbound_verifier test/test_inbound_verifier.cpp)
    target_include_directories(test_inbound_verifier PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(test_inbound_verifier PRIVATE "${CMAKE_BINARY_DIR}/generated")
    if(SYSROOT_INCLUDE)
        target_include_directories(test_inbound_verifier BEFORE PRIVATE SYSTEM ${SYSROOT_INCLUDE})
    endif()
    if(spdlog_FOUND)
        target_include_directories(test_inbound_verifier PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_link_libraries(test_inbound_verifier PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    target_compile_features(test_inbound_verifier PRIVATE cxx_std_20)
endif()

# 全局输出目录 - 可执行文件到test目录，其他到build目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")

# 每个 target 的输出属性
if(NOT BUILD_OBJECT_ONLY)
    set_target_properties(${SMTP_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    )
endif()

# -----------------------------
# 包含目录，以 SYSTEM 标记外部库头以抑制警告
# -----------------------------
# 项目头
target_include_directories(${SMTP_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_include_directories(${SMTP_TARGET} PRIVATE "${CMAKE_BINARY_DIR}/generated")

# spdlog/fmt — 交叉编译时用 sysroot 头文件覆盖 homebrew
if(SYSROOT_INCLUDE)
    target_include_directories(${SMTP_TARGET} BEFORE PRIVATE SYSTEM ${SYSROOT_INCLUDE})
endif()
if(spdlog_FOUND)
    target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
endif()

# nlohmann/json
find_package(nlohmann_json REQUIRED)
target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})

# Boost
if(Boost_FOUND)
    # BOOST 提供的变量通常包含 include dirs
    target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "Boost not found. Install Boost or set BOOST_ROOT / BOOST_INCLUDEDIR.")
endif()

# MySQL（若找到则加入）
if(MYSQL_INCLUDE_DIRS)
    target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
endif()

if(CARES_INCLUDE_DIRS)
    target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${CARES_INCLUDE_DIRS})
endif()

if(CARES_LIBRARY_DIRS)
    if(NOT BUILD_OBJECT_ONLY)
        target_link_directories(${SMTP_TARGET} PRIVATE ${CARES_LIBRARY_DIRS})
    endif()
endif()

# OpenSSL include（仅在未使用导入目标时作为兜底）
if(USE_SSL)
    if(DEFINED OPENSSL_INCLUDE_DIRS AND OPENSSL_INCLUDE_DIRS)
        target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIRS})
    elseif(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
        target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIR})
    endif()
endif()

if(ENABLE_HDFS_WEB_STORAGE AND CURL_FOUND)
    target_include_directories(${SMTP_TARGET} PRIVATE SYSTEM ${CURL_INCLUDE_DIRS})
endif()

# -----------------------------
# 链接依赖（显式链接，避免 link_directories）
# -----------------------------
if(NOT BUILD_OBJECT_ONLY AND NOT MYSQL_LIBRARIES)
    if(APPLE)
        # If mysql client lib still not found, add Homebrew lib dir to link search and try -lmysqlclient as fallback
        message(STATUS "ℹ MYSQL_LIBRARIES empty — on macOS adding /opt/homebrew/lib to link search")
        if(EXISTS "/opt/homebrew/lib")
            link_directories(/opt/homebrew/lib)
            # Use full path instead of -l flag for MySQL client
            set(MYSQL_LIBRARIES "/opt/homebrew/lib/libmysqlclient.dylib")
        endif()
    endif()
endif()

# 根据SSL选项链接不同的库
set(LINK_LIBRARIES Threads::Threads ${MYSQL_LIBRARIES} ${CARES_LIBRARIES} spdlog::spdlog)

if(ENABLE_HDFS_WEB_STORAGE AND TARGET CURL::libcurl)
    list(APPEND LINK_LIBRARIES CURL::libcurl)
elseif(ENABLE_HDFS_WEB_STORAGE AND CURL_LIBRARIES)
    list(APPEND LINK_LIBRARIES ${CURL_LIBRARIES})
endif()

# GCC < 9 may require explicit filesystem runtime library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    list(APPEND LINK_LIBRARIES stdc++fs)
endif()

if(USE_SSL)
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        list(APPEND LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
    elseif(OPENSSL_LIBRARIES)
        message(WARNING "OpenSSL imported targets unavailable, fallback to OPENSSL_LIBRARIES")
        list(APPEND LINK_LIBRARIES ${OPENSSL_LIBRARIES})
    else()
        message(FATAL_ERROR "USE_SSL=ON but neither OpenSSL imported targets nor OPENSSL_LIBRARIES are available")
    endif()
endif()

    target_link_libraries(${SMTP_TARGET} PRIVATE ${LINK_LIBRARIES})

# 对象库入口点的 include 目录
if(BUILD_OBJECT_ONLY)
    foreach(_obj_target imapsServer_obj mailServer_obj)
        target_include_directories(${_obj_target} PRIVATE "${CMAKE_SOURCE_DIR}/include")
        target_include_directories(${_obj_target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
        if(SYSROOT_INCLUDE)
            target_include_directories(${_obj_target} BEFORE PRIVATE SYSTEM ${SYSROOT_INCLUDE})
        endif()
        if(spdlog_FOUND)
            target_include_directories(${_obj_target} PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
        endif()
        target_include_directories(${_obj_target} PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
        target_include_directories(${_obj_target} PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
        if(MYSQL_INCLUDE_DIRS)
            target_include_directories(${_obj_target} PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
        endif()
        target_compile_features(${_obj_target} PRIVATE cxx_std_17)
    endforeach()
endif()

# ===== IMAP 独立服务器 =====
if(NOT BUILD_OBJECT_ONLY)
    set(IMAP_TEST_SRCS
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/imaps_test.cpp
    )
    add_executable(imapsServer ${IMAP_TEST_SRCS})
    set_target_properties(imapsServer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    )
    target_include_directories(imapsServer PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(imapsServer PRIVATE "${CMAKE_BINARY_DIR}/generated")
    if(spdlog_FOUND)
        target_include_directories(imapsServer PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_include_directories(imapsServer PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(imapsServer PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    if(MYSQL_INCLUDE_DIRS)
        target_include_directories(imapsServer PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    if(CARES_INCLUDE_DIRS)
        target_include_directories(imapsServer PRIVATE SYSTEM ${CARES_INCLUDE_DIRS})
    endif()
    if(CARES_LIBRARY_DIRS)
        target_link_directories(imapsServer PRIVATE ${CARES_LIBRARY_DIRS})
    endif()
    if(USE_SSL)
        if(DEFINED OPENSSL_INCLUDE_DIRS AND OPENSSL_INCLUDE_DIRS)
            target_include_directories(imapsServer PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIRS})
        elseif(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
            target_include_directories(imapsServer PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIR})
        endif()
    endif()
    target_compile_features(imapsServer PRIVATE cxx_std_17)
    target_link_libraries(imapsServer PRIVATE ${LINK_LIBRARIES})
    if(ENABLE_INBOUND_DEDUP_CHECK)
        target_compile_definitions(imapsServer PRIVATE ENABLE_INBOUND_DEDUP_CHECK=1)
    else()
        target_compile_definitions(imapsServer PRIVATE ENABLE_INBOUND_DEDUP_CHECK=0)
    endif()

    # ===== 组合服务器 (SMTP + IMAP) =====
    set(COMBINED_SRCS
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/mail_server_combined.cpp
    )
    add_executable(mailServer ${COMBINED_SRCS})
    set_target_properties(mailServer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    )
    target_include_directories(mailServer PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(mailServer PRIVATE "${CMAKE_BINARY_DIR}/generated")
    if(spdlog_FOUND)
        target_include_directories(mailServer PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_include_directories(mailServer PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(mailServer PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    if(MYSQL_INCLUDE_DIRS)
        target_include_directories(mailServer PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    if(CARES_INCLUDE_DIRS)
        target_include_directories(mailServer PRIVATE SYSTEM ${CARES_INCLUDE_DIRS})
    endif()
    if(CARES_LIBRARY_DIRS)
        target_link_directories(mailServer PRIVATE ${CARES_LIBRARY_DIRS})
    endif()
    if(USE_SSL)
        if(DEFINED OPENSSL_INCLUDE_DIRS AND OPENSSL_INCLUDE_DIRS)
            target_include_directories(mailServer PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIRS})
        elseif(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
            target_include_directories(mailServer PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIR})
        endif()
    endif()
    target_compile_features(mailServer PRIVATE cxx_std_17)
    target_link_libraries(mailServer PRIVATE ${LINK_LIBRARIES})
    if(ENABLE_INBOUND_DEDUP_CHECK)
        target_compile_definitions(mailServer PRIVATE ENABLE_INBOUND_DEDUP_CHECK=1)
    else()
        target_compile_definitions(mailServer PRIVATE ENABLE_INBOUND_DEDUP_CHECK=0)
    endif()
endif()

# ===== FSM Mock Benchmark (纯业务逻辑吞吐) =====
if(NOT BUILD_OBJECT_ONLY)
    set(BENCH_SRCS
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/bench/fsm_bench.cpp
    )
    add_executable(fsm_bench ${BENCH_SRCS})
    set_target_properties(fsm_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    )
    target_include_directories(fsm_bench PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(fsm_bench PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    target_include_directories(fsm_bench PRIVATE "${CMAKE_BINARY_DIR}/generated")
    if(spdlog_FOUND)
        target_include_directories(fsm_bench PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_include_directories(fsm_bench PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(fsm_bench PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    if(MYSQL_INCLUDE_DIRS)
        target_include_directories(fsm_bench PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    target_compile_features(fsm_bench PRIVATE cxx_std_17)
    target_link_libraries(fsm_bench PRIVATE ${LINK_LIBRARIES})
endif()

# ===== Outbound SMTP FSM Unit Test (mock 远端服务器) =====
if(NOT BUILD_OBJECT_ONLY)
    set(OUTBOUND_TEST_SRCS
        ${COMMON_SRCS}
        test/unit/outbound_smtp_bench.cpp
    )
    add_executable(outbound_smtp_test ${OUTBOUND_TEST_SRCS})
    set_target_properties(outbound_smtp_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    )
    target_include_directories(outbound_smtp_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(outbound_smtp_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_include_directories(outbound_smtp_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    if(spdlog_FOUND)
        target_include_directories(outbound_smtp_test PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_include_directories(outbound_smtp_test PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(outbound_smtp_test PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    if(MYSQL_INCLUDE_DIRS)
        target_include_directories(outbound_smtp_test PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    target_compile_features(outbound_smtp_test PRIVATE cxx_std_17)
    target_link_libraries(outbound_smtp_test PRIVATE ${LINK_LIBRARIES})
endif()

# ===== Inbound SMTP FSM unit tests =====
if(NOT BUILD_OBJECT_ONLY)
    set(INBOUND_FSM_TEST_SRCS
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/unit/smtps_fsm_test.cpp
    )
    add_executable(smtps_fsm_test ${INBOUND_FSM_TEST_SRCS})
    set_target_properties(smtps_fsm_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    )
    target_include_directories(smtps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(smtps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    target_include_directories(smtps_fsm_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
    if(spdlog_FOUND)
        target_include_directories(smtps_fsm_test PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_include_directories(smtps_fsm_test PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(smtps_fsm_test PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    if(MYSQL_INCLUDE_DIRS)
        target_include_directories(smtps_fsm_test PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    target_compile_features(smtps_fsm_test PRIVATE cxx_std_17)
    target_link_libraries(smtps_fsm_test PRIVATE ${LINK_LIBRARIES})
endif()

# ===== IMAP FSM unit tests =====
if(NOT BUILD_OBJECT_ONLY)
    set(IMAP_FSM_TEST_SRCS
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/unit/imaps_fsm_test.cpp
    )
    add_executable(imaps_fsm_test ${IMAP_FSM_TEST_SRCS})
    set_target_properties(imaps_fsm_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    )
    target_include_directories(imaps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(imaps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    target_include_directories(imaps_fsm_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
    if(spdlog_FOUND)
        target_include_directories(imaps_fsm_test PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_include_directories(imaps_fsm_test PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(imaps_fsm_test PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    if(MYSQL_INCLUDE_DIRS)
        target_include_directories(imaps_fsm_test PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    target_compile_features(imaps_fsm_test PRIVATE cxx_std_17)
    target_link_libraries(imaps_fsm_test PRIVATE ${LINK_LIBRARIES})
endif()

# ===== SMTP benchmark client (raw sockets, no asio) =====
if(NOT BUILD_OBJECT_ONLY)
    add_executable(smtp_client test/bench/smtp_client.cpp)
    set_target_properties(smtp_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")
    target_compile_features(smtp_client PRIVATE cxx_std_17)
endif()

# 目标编译选项
target_compile_features(${SMTP_TARGET} PRIVATE cxx_std_17)

if(ENABLE_INBOUND_DEDUP_CHECK)
    target_compile_definitions(${SMTP_TARGET} PRIVATE ENABLE_INBOUND_DEDUP_CHECK=1)
else()
    target_compile_definitions(${SMTP_TARGET} PRIVATE ENABLE_INBOUND_DEDUP_CHECK=0)
endif()

# 可选：安装规则（不强制）
if(NOT BUILD_OBJECT_ONLY)
    install(TARGETS smtpsServer RUNTIME DESTINATION bin)
    install(TARGETS imapsServer RUNTIME DESTINATION bin)
    install(TARGETS mailServer RUNTIME DESTINATION bin)
endif()