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
option(BUILD_TESTS "Build test and benchmark targets" ON)
option(ENABLE_FUZZING "Build libFuzzer harnesses, e.g. mime_fuzz (clang only)" OFF)
option(ENABLE_TSAN "Build FSM test targets with ThreadSanitizer (-fsanitize=thread -g -O1)" OFF)
option(ENABLE_ASAN "Build unit test targets with AddressSanitizer+UBSan (-fsanitize=address,undefined -g -O1)" OFF)
option(ENABLE_COVERAGE "Build with coverage instrumentation (clang llvm-cov / gcc lcov)" OFF)

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
# 代码仅使用 C++17 特性（if constexpr / std::optional / std::filesystem 等），
# 未使用任何 C++20 特性。为降低编译器版本要求，标准定为 C++17。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===== 编译 flags =====
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DDEBUG -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_DEBUG     "-O0 -g -DDEBUG -fno-omit-frame-pointer")
if(NOT DEFINED CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
endif()
if(NOT DEFINED CMAKE_C_FLAGS_RELEASE)
    set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG -march=native")
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fdiagnostics-color=auto)
endif()

# ===== 覆盖率 =====
# ENABLE_COVERAGE=ON 时打桩。clang 系产出 .profraw（llvm-profdata/llvm-cov 分析），
# gcc 产出 .gcda（lcov/genhtml 分析）。脚本见 test/scripts/coverage.sh。
# 用 flags 而非 add_link_options（后者要求 CMake 3.13+，本项目最低 3.10）。
if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        set(CMAKE_CXX_FLAGS   "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping -g -O0")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(CMAKE_CXX_FLAGS   "${CMAKE_CXX_FLAGS} --coverage -g -O0")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    else()
        message(WARNING "ENABLE_COVERAGE unsupported for compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
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

# ---- MariaDB Connector/C（可选引擎，dlopen 运行时加载 libmariadb，仅需头文件编译） ----
# 找包含 mariadb/mysql.h 的根目录（include/），使 #include <mariadb/mysql.h> 可解析
find_path(MARIADB_INCLUDE_DIR "mariadb/mysql.h"
          HINTS /opt/homebrew/opt/mariadb-connector-c /usr/local/opt/mariadb-connector-c /usr
          PATH_SUFFIXES include)
if(MARIADB_INCLUDE_DIR)
    set(HAVE_MARIADB 1)
    message(STATUS "MariaDB Connector/C headers found: ${MARIADB_INCLUDE_DIR}")
elseif(NOT BUILD_OBJECT_ONLY)
    message(STATUS "mariadb-connector-c headers not found — MariaDB 引擎不编译（achieve=mariadb 不可用）")
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
    src/framework/server_base.cpp
    src/framework/thread_pool/boost_thread_pool.cpp
    src/framework/thread_pool/io_thread_pool.cpp
    src/framework/thread_pool/io_context_registry.cpp
    src/mail_system/back/algorithm/snow.cpp
    src/mail_system/back/storage/distributed_file_storage_provider.cpp
    src/mail_system/back/storage/local_file_storage_provider.cpp
    src/framework/storage/local_file_write_stream.cpp
    src/mail_system/back/mailServer/outbound/outbox_repository.cpp
    src/mail_system/back/mailServer/outbound/outbound_utils.cpp
    src/mail_system/back/mailServer/outbound/mx_routing_utils.cpp
    src/mail_system/back/mailServer/outbound/cares_dns_resolver.cpp
    src/mail_system/back/mailServer/outbound/outbound_server.cpp
    src/mail_system/back/common/bcrypt.cpp
    src/mail_system/back/algorithm/smtp_utils.cpp
    src/mail_system/back/inbound/inbound_verifier.cpp
    src/mail_system/back/persist_storage/persistent_queue.cpp
    src/mail_system/back/router/table_shard_router.cpp
    # 显式模板实例化（避免每个翻译单元重复编译 ~4300 行模板代码）
    src/mail_system/back/mailServer/session/session_base_inst.cpp
    src/mail_system/back/mailServer/fsm/imaps/imap_utils.cpp
    src/mail_system/back/mailServer/session/smtps_session_inst.cpp
    src/mail_system/back/mailServer/session/imaps_session_inst.cpp
    src/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm_inst.cpp
    src/mail_system/back/mailServer/session/pop3_session_inst.cpp
)

if(ENABLE_HDFS_WEB_STORAGE)
    list(APPEND COMMON_SRCS src/mail_system/back/storage/hdfs_web_storage_provider.cpp)
endif()
if(ENABLE_S3_STORAGE)
    add_compile_definitions(ENABLE_S3_STORAGE=1)
    list(APPEND COMMON_SRCS src/mail_system/back/storage/s3_storage_provider.cpp)
endif()
if(HAVE_MARIADB)
    list(APPEND COMMON_SRCS
        src/mail_system/back/db/mariadb_service.cpp
        src/mail_system/back/db/mariadb_pool.cpp)
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
    if(MARIADB_INCLUDE_DIR)
        target_include_directories(${NAME} PRIVATE SYSTEM ${MARIADB_INCLUDE_DIR})
        target_compile_definitions(${NAME} PRIVATE HAVE_MARIADB=1)
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
        test/server/smtps_test.cpp
    )
    setup_mail_target(smtpsServer_obj)
    add_library(imapsServer_obj OBJECT test/server/imaps_test.cpp)
    setup_mail_target(imapsServer_obj)
    add_library(mailServer_obj OBJECT test/server/mail_server_combined.cpp)
    setup_mail_target(mailServer_obj)
    add_library(webServer_obj OBJECT
        ${COMMON_SRCS}
        src/web_server/http_parser.cpp
        src/web_server/http_fsm_inst.cpp
        src/web_server/http_session_inst.cpp
        src/web_server/http_server.cpp
        test/server/web_server_main.cpp
    )
    setup_mail_target(webServer_obj)
else()
    add_executable(smtpsServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/server/smtps_test.cpp
    )
    setup_mail_target(smtpsServer)

    add_executable(imapsServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/server/imaps_test.cpp
    )
    setup_mail_target(imapsServer)

    add_executable(mailServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/server/mail_server_combined.cpp
    )
    setup_mail_target(mailServer)

    add_executable(pop3Server
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/pop3/pop3_server.cpp
        test/server/pop3_test.cpp
    )
    setup_mail_target(pop3Server)

    add_executable(webServer
        ${COMMON_SRCS}
        src/web_server/http_parser.cpp
        src/web_server/http_fsm_inst.cpp
        src/web_server/http_session_inst.cpp
        src/web_server/http_server.cpp
        test/server/web_server_main.cpp
    )
    setup_mail_target(webServer)
endif()

# ================================================================
# 测试 / Benchmark（BUILD_TESTS=OFF 时跳过）
# ================================================================
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    add_executable(test_inbound_verifier
        test/unit/test_inbound_verifier.cpp
        src/mail_system/back/inbound/inbound_verifier.cpp
    )
    setup_mail_target(test_inbound_verifier)
    target_include_directories(test_inbound_verifier PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")

    add_executable(sql_queries_test
        test/unit/sql_queries_test.cpp
        src/mail_system/back/db/sql_queries.cpp
    )
    setup_mail_target(sql_queries_test)

    add_executable(mime_parser_test
        test/unit/mime_parser_test.cpp
        src/mail_system/back/mailServer/fsm/imaps/imap_utils.cpp
    )
    target_include_directories(mime_parser_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mime_parser_test)

    add_executable(mail_body_writer_test
        test/unit/mail_body_writer_test.cpp
        src/framework/storage/local_file_write_stream.cpp
        src/mail_system/back/storage/local_file_storage_provider.cpp
    )
    target_include_directories(mail_body_writer_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mail_body_writer_test)

    add_executable(buffered_upload_stream_test
        test/unit/buffered_upload_stream_test.cpp
    )
    target_include_directories(buffered_upload_stream_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(buffered_upload_stream_test)

    add_executable(io_error_test
        test/unit/io_error_test.cpp
    )
    target_include_directories(io_error_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(io_error_test)

    add_executable(async_storage_provider_test
        test/unit/async_storage_provider_test.cpp
    )
    target_include_directories(async_storage_provider_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(async_storage_provider_test)

    # ---- framework 层单元测试 ----
    # 纯逻辑 / 文件 IO / 线程池：只依赖 framework 头 + 对应 .cpp
    add_executable(intrusion_detector_test test/unit/intrusion_detector_test.cpp)
    target_include_directories(intrusion_detector_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(intrusion_detector_test)

    add_executable(server_config_test test/unit/server_config_test.cpp)
    target_include_directories(server_config_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(server_config_test)

    add_executable(mapped_file_test test/unit/mapped_file_test.cpp)
    target_include_directories(mapped_file_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mapped_file_test)

    add_executable(bcrypt_test test/unit/bcrypt_test.cpp src/mail_system/back/common/bcrypt.cpp)
    target_include_directories(bcrypt_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(bcrypt_test)

    add_executable(thread_pool_test
        src/framework/thread_pool/io_thread_pool.cpp
        src/framework/thread_pool/io_context_registry.cpp
        src/framework/thread_pool/boost_thread_pool.cpp
        test/unit/thread_pool_test.cpp
    )
    target_include_directories(thread_pool_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(thread_pool_test)

    # MariaDB 非阻塞 async 回归（阶段 2）：连接池 available 在批量 async 查询后回基线
    if(HAVE_MARIADB)
        add_executable(mariadb_async_test ${COMMON_SRCS} test/unit/mariadb_async_test.cpp)
        target_include_directories(mariadb_async_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        setup_mail_target(mariadb_async_test)
        add_test(NAME mariadb_async_test COMMAND mariadb_async_test)
    endif()

    # 会话/FSM/服务基座：TestSession::close() 引用 ServerBase 成员符号
    # （server_base.cpp），须链 COMMON_SRCS（同 smtps/imaps_fsm_test 模式）
    add_executable(fsm_base_test ${COMMON_SRCS} test/unit/fsm_base_test.cpp)
    target_include_directories(fsm_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fsm_base_test)

    add_executable(fast_fsm_base_test ${COMMON_SRCS} test/unit/fast_fsm_base_test.cpp)
    target_include_directories(fast_fsm_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fast_fsm_base_test)

    add_executable(session_base_test ${COMMON_SRCS} test/unit/session_base_test.cpp)
    target_include_directories(session_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(session_base_test)

    add_executable(server_base_test ${COMMON_SRCS} test/unit/server_base_test.cpp)
    target_include_directories(server_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(server_base_test)

    add_executable(outbound_utils_test ${COMMON_SRCS} test/unit/outbound_utils_test.cpp)
    target_include_directories(outbound_utils_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(outbound_utils_test)

    # MetricsServer 核心 API 不变量测试（counter/gauge/histogram 语义）。
    # 不链 COMMON_SRCS：MetricsServer 是 header-only，只依赖 boost::asio。
    add_executable(metrics_core_test test/unit/metrics_core_test.cpp)
    target_include_directories(metrics_core_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(metrics_core_test)

    add_executable(fsm_bench
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/bench/fsm/fsm_bench.cpp
    )
    target_include_directories(fsm_bench PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fsm_bench)

    add_executable(smtps_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/unit/smtps_fsm_test.cpp
    )
    target_include_directories(smtps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(smtps_fsm_test)

    # 异步/并发 FSM 测试（Mock DNS/DB 延迟回调 + 真实 IO 线程）
    add_executable(smtps_fsm_concurrency_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/unit/smtps_fsm_concurrency_test.cpp
    )
    target_include_directories(smtps_fsm_concurrency_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(smtps_fsm_concurrency_test)
    if(ENABLE_TSAN)
        target_compile_options(smtps_fsm_test PRIVATE -fsanitize=thread -g -O1)
        target_link_options(smtps_fsm_test PRIVATE -fsanitize=thread)
        target_compile_options(smtps_fsm_concurrency_test PRIVATE -fsanitize=thread -g -O1)
        target_link_options(smtps_fsm_concurrency_test PRIVATE -fsanitize=thread)
    endif()


    add_executable(imaps_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/unit/imaps_fsm_test.cpp
    )
    target_include_directories(imaps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(imaps_fsm_test)

    add_executable(pop3_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/pop3/pop3_server.cpp
        test/unit/pop3_fsm_test.cpp
    )
    target_include_directories(pop3_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(pop3_fsm_test)

    add_executable(smtp_client test/bench/smtp/smtp_client.cpp)
    target_compile_features(smtp_client PRIVATE cxx_std_17)
    add_executable(imap_client test/bench/imap/imap_client.cpp)
    target_compile_features(imap_client PRIVATE cxx_std_17)
    add_executable(pop3_client test/bench/pop3/pop3_client.cpp)
    target_compile_features(pop3_client PRIVATE cxx_std_17)

    add_executable(dispatch_bench test/bench/fsm/dispatch_bench.cpp)
    target_compile_features(dispatch_bench PRIVATE cxx_std_17)

    add_executable(outbound_smoke test/unit/outbound_smoke.cpp)
    setup_mail_target(outbound_smoke)
    target_include_directories(outbound_smoke PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
endif()

# ================================================================
# CTest 集成
# ================================================================
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    enable_testing()
    add_test(NAME smtps_fsm_test   COMMAND smtps_fsm_test)
    add_test(NAME smtps_fsm_concurrency_test COMMAND smtps_fsm_concurrency_test)
    add_test(NAME imaps_fsm_test   COMMAND imaps_fsm_test)
    add_test(NAME pop3_fsm_test    COMMAND pop3_fsm_test)
    add_test(NAME inbound_verifier COMMAND test_inbound_verifier)
    add_test(NAME outbound_smoke   COMMAND outbound_smoke)
    add_test(NAME sql_queries_test COMMAND sql_queries_test)
    add_test(NAME mime_parser_test COMMAND mime_parser_test)
    add_test(NAME mail_body_writer_test COMMAND mail_body_writer_test)
    add_test(NAME buffered_upload_stream_test COMMAND buffered_upload_stream_test)
    add_test(NAME io_error_test COMMAND io_error_test)
    add_test(NAME async_storage_provider_test COMMAND async_storage_provider_test)
    add_test(NAME fsm_base_test COMMAND fsm_base_test)
    add_test(NAME fast_fsm_base_test COMMAND fast_fsm_base_test)
    add_test(NAME intrusion_detector_test COMMAND intrusion_detector_test)
    add_test(NAME server_config_test COMMAND server_config_test)
    add_test(NAME mapped_file_test COMMAND mapped_file_test)
    add_test(NAME thread_pool_test COMMAND thread_pool_test)
    add_test(NAME session_base_test COMMAND session_base_test)
    add_test(NAME server_base_test COMMAND server_base_test)
    add_test(NAME outbound_utils_test COMMAND outbound_utils_test)
    add_test(NAME metrics_core_test COMMAND metrics_core_test)
    add_test(NAME bcrypt_test COMMAND bcrypt_test)

    # 聚合 target：新增单测只需在上面 add_executable/add_test，
    # CI 与本地都构建这一个 target，避免测试清单漂移（此前 CI 的
    # 显式 target 列表缺了新增的 4 个测试，ctest 因二进制缺失而红）
    add_custom_target(unit_tests DEPENDS
        smtps_fsm_test
        smtps_fsm_concurrency_test
        imaps_fsm_test
        pop3_fsm_test
        test_inbound_verifier
        outbound_smoke
        sql_queries_test
        mime_parser_test
        mail_body_writer_test
        buffered_upload_stream_test
        io_error_test
        async_storage_provider_test
        fsm_base_test
        fast_fsm_base_test
        intrusion_detector_test
        server_config_test
        mapped_file_test
        thread_pool_test
        session_base_test
        server_base_test
        outbound_utils_test
        metrics_core_test
        bcrypt_test
    )
    # ASan 对全部单测生效：string_view 越界索引不一定 segfault，
    # 没有地址消毒的模糊测试发现不了这类堆/栈越界
    if(ENABLE_ASAN)
        set(_asan_tests smtps_fsm_test smtps_fsm_concurrency_test imaps_fsm_test
            pop3_fsm_test
            test_inbound_verifier outbound_smoke sql_queries_test mime_parser_test
            mail_body_writer_test buffered_upload_stream_test io_error_test
            async_storage_provider_test fsm_base_test fast_fsm_base_test
            intrusion_detector_test server_config_test mapped_file_test
            thread_pool_test session_base_test server_base_test
            outbound_utils_test metrics_core_test bcrypt_test)
        foreach(_t IN LISTS _asan_tests)
            # vptr 排除：它给多态向下转型插 typeinfo 引用，跨翻译单元链接不了
            target_compile_options(${_t} PRIVATE -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -g -O1)
            target_link_options(${_t} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()

endif()

if(NOT BUILD_OBJECT_ONLY)
    install(TARGETS smtpsServer imapsServer mailServer RUNTIME DESTINATION bin)
endif()

# ---- Fuzz harnesses（libFuzzer，默认关闭） ----
# 注意：Apple 自带 clang 不带 libFuzzer 运行时（链接报 libclang_rt.fuzzer_osx.a
# not found）。macOS 上用 Homebrew LLVM：
#   CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
#     cmake -B build-fuzz -DENABLE_FUZZING=ON -DBUILD_TESTS=OFF
# Linux CI 的 clang 直接可用。
if(ENABLE_FUZZING)
    # fuzzer + ASan/UBSan 是标准组合：光靠 segfault 抓不住 string_view 的
    # 越界读（很多落在分配器余量里）
    set(FUZZ_SANITIZERS fuzzer-no-link,address,undefined)
    set(FUZZ_NOSAN vptr)

    add_executable(mime_fuzz test/fuzz/mime_fuzz.cpp)
    target_include_directories(mime_fuzz PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(mime_fuzz PRIVATE -fsanitize=${FUZZ_SANITIZERS} -fno-sanitize=${FUZZ_NOSAN} -g -O1)
    target_link_options(mime_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    # SMTP DATA 解析核心。刻意不走 setup_mail_target：它会带进 SDK 的
    # -isystem（CURL/MySQL 的 macOS 路径），破坏 Homebrew clang 下 libc++
    # 头的搜索链。手动只挂实际需要的 include + spdlog 链接。
    add_executable(smtp_fuzz
        test/fuzz/smtp_data_fuzz.cpp
        src/mail_system/back/algorithm/smtp_utils.cpp)
    target_include_directories(smtp_fuzz PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        SYSTEM ${nlohmann_json_INCLUDE_DIRS}
        SYSTEM ${Boost_INCLUDE_DIRS})
    if(spdlog_FOUND AND spdlog_INCLUDE_DIRS)
        target_include_directories(smtp_fuzz PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
    endif()
    target_link_libraries(smtp_fuzz PRIVATE spdlog::spdlog)
    target_compile_options(smtp_fuzz PRIVATE -fsanitize=${FUZZ_SANITIZERS} -fno-sanitize=${FUZZ_NOSAN} -g -O1)
    target_link_options(smtp_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    foreach(_ft mime_fuzz smtp_fuzz)
    if(APPLE)
        # Homebrew LLVM 的 fuzzer 运行时依赖它自带的（更新的）libc++；
        # 不显式指过去的话 ld 会用 Xcode SDK 的旧 libc++，符号对不上。
        # HOMEBREW_LLVM_PREFIX 可按环境覆盖。
        set(HOMEBREW_LLVM_PREFIX "/opt/homebrew/opt/llvm" CACHE PATH "Homebrew LLVM prefix")
        # -nostdlib++ 去掉隐式 -lc++，再显式链 Homebrew 的 libc++：
        # 隐式那个会解析到 Xcode SDK 的旧版（缺 __hash_memory 等新符号），
        # 而 -L 搜索顺序竞争不过 SDK 路径，干脆用绝对路径
        target_link_options(mime_fuzz PRIVATE
            -nostdlib++
            -Wl,-rpath,${HOMEBREW_LLVM_PREFIX}/lib/c++)
        target_link_libraries(${_ft} PRIVATE ${HOMEBREW_LLVM_PREFIX}/lib/c++/libc++.1.dylib)
    endif()
    endforeach()
    message(STATUS "Fuzz harnesses enabled: mime_fuzz, smtp_fuzz")
endif()
