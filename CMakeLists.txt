cmake_minimum_required(VERSION 3.10)
project(ProtoRelay LANGUAGES CXX)

set(PROTORELAY_VERSION "0.1.0-dev" CACHE STRING "ProtoRelay semantic version")

# Prefer classic FindBoost module behavior to avoid Homebrew BoostConfig
# component resolution issues on macOS.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

# ===== 构建选项 =====
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Debug/Release" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

option(USE_SSL "Enable SSL/TLS support" ON)
option(ENABLE_DEBUG_LOGS "Enable debug logs for all modules" OFF)
option(BUILD_OBJECT_ONLY "Compile object files only" OFF)
option(ENABLE_INBOUND_DEDUP_CHECK "Enable inbound duplicate check" ON)
option(ENABLE_HDFS_WEB_STORAGE "Enable WebHDFS storage (requires libcurl)" ON)
option(ENABLE_S3_STORAGE "Enable S3/MinIO storage (requires libcurl, OpenSSL)" ON)

# ===== Feature flags — 必须始终定义为 0/1（help_text.cpp 等 #if 使用）=====
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

# Build info
string(TIMESTAMP PROTORELAY_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
set(PROTORELAY_BUILD_TARGET "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(PROTORELAY_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE PROTORELAY_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PROTORELAY_GIT_COMMIT)
    set(PROTORELAY_GIT_COMMIT "unknown")
endif()
configure_file("${CMAKE_SOURCE_DIR}/include/mail_system/back/version/build_info.h.in"
               "${CMAKE_BINARY_DIR}/generated/build_info.h" @ONLY)

if(BUILD_OBJECT_ONLY)
    add_compile_definitions(FMT_HEADER_ONLY=1)
endif()

# ===== C++ 标准 =====
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===== 编译 flags =====
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DDEBUG -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_DEBUG     "-O0 -g -DDEBUG -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG -march=native")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fdiagnostics-color=auto)
endif()

# ===== 日志级别 =====
if(ENABLE_DEBUG_LOGS)
    set(LOG_DEF 1)
else()
    set(LOG_DEF 0)
endif()
add_compile_definitions(
    ENABLE_SERVER_DEBUG_LOG=${LOG_DEF}
    ENABLE_NETWORK_DEBUG_LOG=${LOG_DEF}
    ENABLE_DATABASE_DEBUG_LOG=${LOG_DEF}
    ENABLE_DATABASE_QUERY_DEBUG_LOG=${LOG_DEF}
    ENABLE_SMTP_DEBUG_LOG=${LOG_DEF}
    ENABLE_SMTP_DETAIL_DEBUG_LOG=${LOG_DEF}
    ENABLE_SESSION_DEBUG_LOG=${LOG_DEF}
    ENABLE_THREAD_POOL_DEBUG_LOG=${LOG_DEF}
    ENABLE_FILE_IO_DEBUG_LOG=${LOG_DEF}
    ENABLE_AUTH_DEBUG_LOG=${LOG_DEF}
)

# ===== 依赖查找 =====
set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "" FORCE)
find_package(Boost REQUIRED)
set(SYSROOT_INCLUDE "${SYSROOT_INCLUDE}" CACHE PATH "Sysroot include path for cross-compilation")

if(USE_SSL)
    add_compile_definitions(USE_SSL)
    message(STATUS "SSL/TLS support ENABLED")
    if(BUILD_OBJECT_ONLY)
        find_package(OpenSSL)
    else()
        find_package(OpenSSL REQUIRED)
    endif()
    if(NOT OpenSSL_FOUND AND APPLE)
        if(EXISTS "/opt/homebrew/opt/openssl")
            set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl")
        elseif(EXISTS "/usr/local/opt/openssl")
            set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
        endif()
        find_package(OpenSSL REQUIRED)
    endif()
else()
    set(OPENSSL_FOUND FALSE)
    message(STATUS "SSL/TLS support DISABLED")
endif()

# spdlog - cross-compile uses sysroot
if(SYSROOT_INCLUDE)
    message(STATUS "Using sysroot spdlog/fmt: ${SYSROOT_INCLUDE}")
    if(NOT TARGET spdlog::spdlog)
        add_library(spdlog::spdlog INTERFACE IMPORTED)
        set_target_properties(spdlog::spdlog PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SYSROOT_INCLUDE}")
    endif()
    add_compile_definitions(SPDLOG_FMT_EXTERNAL=OFF)
else()
    find_package(spdlog REQUIRED)
endif()

# nlohmann_json
find_package(nlohmann_json REQUIRED)

# curl
if(ENABLE_HDFS_WEB_STORAGE)
    find_package(CURL REQUIRED)
endif()

find_package(Threads REQUIRED)

# ===== MySQL / c-ares =====
find_package(PkgConfig)
set(MYSQL_INCLUDE_DIRS "" MYSQL_LIBRARIES "")
set(CARES_INCLUDE_DIRS "" CARES_LIBRARIES "" CARES_LIBRARY_DIRS "")

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_CARES QUIET libcares)
    if(NOT PC_CARES_FOUND)
        pkg_check_modules(PC_CARES QUIET cares)
    endif()
    if(PC_CARES_FOUND)
        set(CARES_INCLUDE_DIRS ${PC_CARES_INCLUDE_DIRS})
        set(CARES_LIBRARY_DIRS ${PC_CARES_LIBRARY_DIRS})
        set(CARES_LIBRARIES ${PC_CARES_LIBRARIES})
        message(STATUS "c-ares found via pkg-config")
    endif()
endif()

if(NOT CARES_LIBRARIES)
    find_path(CARES_INCLUDE_DIR ares.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(CARES_LIBRARY NAMES cares c-ares PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(CARES_INCLUDE_DIR AND CARES_LIBRARY)
        set(CARES_INCLUDE_DIRS ${CARES_INCLUDE_DIR})
        set(CARES_LIBRARIES ${CARES_LIBRARY})
        get_filename_component(CARES_LIBRARY_DIRS ${CARES_LIBRARY} DIRECTORY)
        message(STATUS "c-ares found: ${CARES_LIBRARY}")
    elseif(NOT BUILD_OBJECT_ONLY)
        message(FATAL_ERROR "c-ares not found. Install: brew install c-ares")
    endif()
endif()

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_MYSQL QUIET mysqlclient libmysqlclient)
    if(PC_MYSQL_FOUND)
        set(MYSQL_INCLUDE_DIRS ${PC_MYSQL_INCLUDE_DIRS})
        set(MYSQL_LIBRARIES ${PC_MYSQL_LIBRARIES})
        message(STATUS "mysqlclient via pkg-config: ${PC_MYSQL_VERSION}")
    endif()
endif()

if(NOT MYSQL_LIBRARIES)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(MYSQL_SEARCH /opt/homebrew/opt/mysql-client /opt/homebrew)
    elseif(APPLE)
        set(MYSQL_SEARCH /usr/local/opt/mysql /usr/local)
    else()
        set(MYSQL_SEARCH /usr /usr/local /opt/mysql)
    endif()
    find_path(MYSQL_INCLUDE_DIR mysql.h PATHS ${MYSQL_SEARCH} PATH_SUFFIXES include include/mysql)
    find_library(MYSQL_CLIENT_LIB NAMES mysqlclient libmysqlclient PATHS ${MYSQL_SEARCH} PATH_SUFFIXES lib lib64)
    if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT_LIB)
        set(MYSQL_INCLUDE_DIRS "${MYSQL_INCLUDE_DIR}")
        set(MYSQL_LIBRARIES "${MYSQL_CLIENT_LIB}")
        message(STATUS "MySQL found: ${MYSQL_CLIENT_LIB}")
    elseif(NOT BUILD_OBJECT_ONLY AND APPLE AND EXISTS "/opt/homebrew/lib/libmysqlclient.dylib")
        set(MYSQL_LIBRARIES "/opt/homebrew/lib/libmysqlclient.dylib")
        link_directories(/opt/homebrew/lib)
    elseif(NOT BUILD_OBJECT_ONLY)
        message(WARNING "mysqlclient not found. Set MYSQL_INCLUDE_DIRS / MYSQL_LIBRARIES")
    endif()
endif()

# ===== 链接库列表 =====
set(LINK_LIBRARIES Threads::Threads ${MYSQL_LIBRARIES} ${CARES_LIBRARIES} spdlog::spdlog)

if(ENABLE_HDFS_WEB_STORAGE)
    if(TARGET CURL::libcurl)
        list(APPEND LINK_LIBRARIES CURL::libcurl)
    elseif(CURL_LIBRARIES)
        list(APPEND LINK_LIBRARIES ${CURL_LIBRARIES})
    endif()
endif()

if(USE_SSL)
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        list(APPEND LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
    elseif(OPENSSL_LIBRARIES)
        list(APPEND LINK_LIBRARIES ${OPENSSL_LIBRARIES})
    else()
        message(FATAL_ERROR "USE_SSL=ON but OpenSSL not found")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    list(APPEND LINK_LIBRARIES stdc++fs)
endif()

# ===== 公共源文件 =====
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
    # 显式模板实例化（避免每个翻译单元重复编译 ~4300 行模板代码）
    src/mail_system/back/mailServer/session/session_base_inst.cpp
    src/mail_system/back/mailServer/session/smtps_session_inst.cpp
    src/mail_system/back/mailServer/session/imaps_session_inst.cpp
    src/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm_inst.cpp
)

if(ENABLE_HDFS_WEB_STORAGE)
    list(APPEND COMMON_SRCS src/mail_system/back/storage/hdfs_web_storage_provider.cpp)
endif()
if(ENABLE_S3_STORAGE)
    add_compile_definitions(ENABLE_S3_STORAGE=1)
    list(APPEND COMMON_SRCS src/mail_system/back/storage/s3_storage_provider.cpp)
endif()

# ===== 输出目录 =====
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")

# ================================================================
# setup_mail_target — 为 target 注入公共 include/lib
# ================================================================
function(setup_mail_target NAME)
    target_include_directories(${NAME} PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(${NAME} PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_include_directories(${NAME} PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(${NAME} PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    target_compile_features(${NAME} PRIVATE cxx_std_17)

    if(SYSROOT_INCLUDE)
        target_include_directories(${NAME} BEFORE PRIVATE SYSTEM ${SYSROOT_INCLUDE})
    endif()
    if(spdlog_FOUND)
        target_include_directories(${NAME} PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    if(MYSQL_INCLUDE_DIRS)
        target_include_directories(${NAME} PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    if(CARES_INCLUDE_DIRS)
        target_include_directories(${NAME} PRIVATE SYSTEM ${CARES_INCLUDE_DIRS})
    endif()
    if(CARES_LIBRARY_DIRS AND NOT BUILD_OBJECT_ONLY)
        target_link_directories(${NAME} PRIVATE ${CARES_LIBRARY_DIRS})
    endif()
    if(USE_SSL)
        if(DEFINED OPENSSL_INCLUDE_DIRS AND OPENSSL_INCLUDE_DIRS)
            target_include_directories(${NAME} PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIRS})
        elseif(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
            target_include_directories(${NAME} PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIR})
        endif()
    endif()
    if(ENABLE_HDFS_WEB_STORAGE AND CURL_FOUND)
        target_include_directories(${NAME} PRIVATE SYSTEM ${CURL_INCLUDE_DIRS})
    endif()
    if(NOT BUILD_OBJECT_ONLY)
        target_link_libraries(${NAME} PRIVATE ${LINK_LIBRARIES})
    endif()
    if(ENABLE_INBOUND_DEDUP_CHECK)
        target_compile_definitions(${NAME} PRIVATE ENABLE_INBOUND_DEDUP_CHECK=1)
    else()
        target_compile_definitions(${NAME} PRIVATE ENABLE_INBOUND_DEDUP_CHECK=0)
    endif()
endfunction()

# ================================================================
# smtpsServer — SMTP + IMAP 合并入口
# ================================================================
if(BUILD_OBJECT_ONLY)
    add_library(smtpsServer_obj OBJECT
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/smtps_test.cpp
    )
    setup_mail_target(smtpsServer_obj)
    add_library(imapsServer_obj OBJECT test/imaps_test.cpp)
    setup_mail_target(imapsServer_obj)
    add_library(mailServer_obj OBJECT test/mail_server_combined.cpp)
    setup_mail_target(mailServer_obj)
else()
    add_executable(smtpsServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/smtps_test.cpp
    )
    setup_mail_target(smtpsServer)

    add_executable(imapsServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/imaps_test.cpp
    )
    setup_mail_target(imapsServer)

    add_executable(mailServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/mail_server_combined.cpp
    )
    setup_mail_target(mailServer)
endif()

# ================================================================
# 测试 / Benchmark
# ================================================================
if(NOT BUILD_OBJECT_ONLY)
    add_executable(test_inbound_verifier test/test_inbound_verifier.cpp)
    setup_mail_target(test_inbound_verifier)

    add_executable(fsm_bench
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/bench/fsm_bench.cpp
    )
    target_include_directories(fsm_bench PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fsm_bench)

    add_executable(outbound_smtp_test
        ${COMMON_SRCS}
        test/unit/outbound_smtp_bench.cpp
    )
    target_include_directories(outbound_smtp_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(outbound_smtp_test)

    add_executable(smtps_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/unit/smtps_fsm_test.cpp
    )
    target_include_directories(smtps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(smtps_fsm_test)

    add_executable(imaps_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/unit/imaps_fsm_test.cpp
    )
    target_include_directories(imaps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(imaps_fsm_test)

    add_executable(smtp_client test/bench/smtp_client.cpp)
    target_compile_features(smtp_client PRIVATE cxx_std_17)

    install(TARGETS smtpsServer imapsServer mailServer RUNTIME DESTINATION bin)
endif()
