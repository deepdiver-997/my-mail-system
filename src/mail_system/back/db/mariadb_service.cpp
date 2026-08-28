#include "mail_system/back/db/mariadb_service.h"
#include "mail_system/back/common/logger.h"
#include "framework/thread_pool/io_context_registry.h"
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace mail_system {

// ====================================================================
// MariaDbDriver —— dlopen/dlsym 绑定层
// ====================================================================

MariaDbDriver& MariaDbDriver::instance() {
    static MariaDbDriver s_instance;
    return s_instance;
}

template <typename T>
bool MariaDbDriver::sym(T* out, const char* name) {
    void* p = dlsym(handle_, name);
    if (!p) {
        const char* dlerr = dlerror();
        error_ += std::string(name) + ": " + (dlerr ? dlerr : "unknown") + "; ";
        return false;
    }
    // dlsym 返回 void*，转函数指针在标准 C++ 里是条件支持；POSIX 保证可用。
    // 用 memcpy 规避 strict-aliasing/pedantic 告警。out 是 T*（指向函数指针），
    // memcpy 的 void* 参数能接受。
    std::memcpy(out, &p, sizeof(p));
    return true;
}

template <typename T>
bool MariaDbDriver::sym_optional(T* out, const char* name) {
    void* p = dlsym(handle_, name);
    if (!p) return true;   // 可选符号缺失不视为加载失败
    std::memcpy(out, &p, sizeof(p));
    return true;
}

bool MariaDbDriver::ensure_loaded() {
    if (loaded_) return true;
    if (handle_) return false;   // 已尝试过但失败

    // 加载候选名：macOS 优先（含 keg-only 绝对路径），Linux 兜底。
    // RTLD_LOCAL 是关键——libmariadb 内部的 mysql_* 引用保持局部，不与已链接的
    // libmysqlclient 符号互渗。keg-only 的 brew mariadb-connector-c 不在默认
    // dyld 搜索路径里，必须给绝对路径兜底。
    static const char* kCandidates[] = {
        "/opt/homebrew/opt/mariadb-connector-c/lib/libmariadb.3.dylib",
        "/usr/local/opt/mariadb-connector-c/lib/libmariadb.3.dylib",
        "libmariadb.3.dylib",
        "libmariadb.dylib",
        "libmariadb.so.3",
        "libmariadb.so",
    };
    for (const char* c : kCandidates) {
        void* h = dlopen(c, RTLD_NOW | RTLD_LOCAL);
        if (h) { handle_ = h; break; }
    }
    if (!handle_) {
        const char* dlerr = dlerror();
        error_ = "dlopen libmariadb failed: " + std::string(dlerr ? dlerr : "unknown");
        LOG_DATABASE_ERROR("MariaDbDriver: {}", error_);
        return false;
    }

    error_.clear();
    bool ok = true;
    ok &= sym(&mysql_init,                    "mysql_init");
    ok &= sym(&mysql_options,                 "mysql_options");
    ok &= sym(&mysql_real_connect,            "mysql_real_connect");
    ok &= sym(&mysql_set_character_set,       "mysql_set_character_set");
    ok &= sym(&mysql_close,                   "mysql_close");
    ok &= sym(&mysql_ping,                    "mysql_ping");
    ok &= sym(&mysql_query,                   "mysql_query");
    ok &= sym(&mysql_select_db,               "mysql_select_db");
    ok &= sym(&mysql_store_result,            "mysql_store_result");
    ok &= sym(&mysql_field_count,             "mysql_field_count");
    ok &= sym(&mysql_num_fields,              "mysql_num_fields");
    ok &= sym(&mysql_fetch_fields,            "mysql_fetch_fields");
    ok &= sym(&mysql_fetch_row,               "mysql_fetch_row");
    ok &= sym(&mysql_fetch_lengths,           "mysql_fetch_lengths");
    ok &= sym(&mysql_num_rows,                "mysql_num_rows");
    ok &= sym(&mysql_free_result,             "mysql_free_result");
    ok &= sym(&mysql_errno,                   "mysql_errno");
    ok &= sym(&mysql_error,                   "mysql_error");
    ok &= sym(&mysql_stmt_init,               "mysql_stmt_init");
    ok &= sym(&mysql_stmt_prepare,            "mysql_stmt_prepare");
    ok &= sym(&mysql_stmt_bind_param,         "mysql_stmt_bind_param");
    ok &= sym(&mysql_stmt_execute,            "mysql_stmt_execute");
    ok &= sym(&mysql_stmt_result_metadata,    "mysql_stmt_result_metadata");
    ok &= sym(&mysql_stmt_field_count,        "mysql_stmt_field_count");
    ok &= sym(&mysql_stmt_store_result,       "mysql_stmt_store_result");
    ok &= sym(&mysql_stmt_bind_result,        "mysql_stmt_bind_result");
    ok &= sym(&mysql_stmt_fetch,              "mysql_stmt_fetch");
    ok &= sym(&mysql_stmt_fetch_column,       "mysql_stmt_fetch_column");
    ok &= sym(&mysql_stmt_close,              "mysql_stmt_close");
    ok &= sym(&mysql_stmt_free_result,        "mysql_stmt_free_result");
    ok &= sym(&mysql_stmt_error,              "mysql_stmt_error");
    ok &= sym(&mysql_stmt_affected_rows,      "mysql_stmt_affected_rows");
    ok &= sym(&mysql_real_escape_string,      "mysql_real_escape_string");
    if (!ok) {
        LOG_DATABASE_ERROR("MariaDbDriver: dlsym 缺必需符号: {}", error_);
        return false;
    }
    // 阶段 2：非阻塞 API（可选，缺则 has_nonblocking()=false → async_* 回退同步）
    sym_optional(&mysql_stmt_prepare_start,   "mysql_stmt_prepare_start");
    sym_optional(&mysql_stmt_prepare_cont,    "mysql_stmt_prepare_cont");
    sym_optional(&mysql_stmt_execute_start,   "mysql_stmt_execute_start");
    sym_optional(&mysql_stmt_execute_cont,    "mysql_stmt_execute_cont");
    sym_optional(&mysql_stmt_store_result_start, "mysql_stmt_store_result_start");
    sym_optional(&mysql_stmt_store_result_cont,  "mysql_stmt_store_result_cont");
    sym_optional(&mysql_get_socket,           "mysql_get_socket");
    sym_optional(&mysql_get_timeout_value_ms, "mysql_get_timeout_value_ms");

    loaded_ = true;
    LOG_DATABASE_INFO("MariaDbDriver: libmariadb loaded ({} symbol(s) bound, nonblocking={})",
                      40, has_nonblocking());
    return true;
}

// ====================================================================
// MariaDbResult
// ====================================================================

MariaDbResult::MariaDbResult(MYSQL_RES* result)
    : m_result(result), m_rowCount(0), m_columnCount(0) {
    if (m_result) {
        load_result_data();
        MariaDbDriver::instance().mysql_free_result(m_result);
        m_result = nullptr;
    }
}

MariaDbResult::MariaDbResult(std::vector<std::string> colNames,
                             std::vector<std::vector<std::string>> rows,
                             size_t rowCount, size_t colCount)
    : m_result(nullptr)
    , m_columnNames(std::move(colNames))
    , m_rows(std::move(rows))
    , m_rowCount(rowCount)
    , m_columnCount(colCount) {}

MariaDbResult::~MariaDbResult() {
    if (m_result) {
        MariaDbDriver::instance().mysql_free_result(m_result);
        m_result = nullptr;
    }
}

void MariaDbResult::load_result_data() {
    auto& D = MariaDbDriver::instance();
    m_rowCount = static_cast<size_t>(D.mysql_num_rows(m_result));
    m_columnCount = D.mysql_num_fields(m_result);

    MYSQL_FIELD* fields = D.mysql_fetch_fields(m_result);
    for (size_t i = 0; i < m_columnCount; ++i) {
        m_columnNames.push_back(fields[i].name);
    }

    MYSQL_ROW row;
    while ((row = D.mysql_fetch_row(m_result))) {
        std::vector<std::string> rowData;
        unsigned long* lengths = D.mysql_fetch_lengths(m_result);
        for (size_t i = 0; i < m_columnCount; ++i) {
            if (row[i]) {
                rowData.push_back(std::string(row[i], lengths[i]));
            } else {
                rowData.push_back("");
            }
        }
        m_rows.push_back(std::move(rowData));
    }
}

size_t MariaDbResult::get_row_count() const { return m_rowCount; }
size_t MariaDbResult::get_column_count() const { return m_columnCount; }
std::vector<std::string> MariaDbResult::get_column_names() const { return m_columnNames; }

std::map<std::string, std::string> MariaDbResult::get_row(size_t row_index) const {
    std::map<std::string, std::string> rowMap;
    if (row_index < m_rowCount) {
        for (size_t i = 0; i < m_columnCount; ++i) {
            rowMap[m_columnNames[i]] = m_rows[row_index][i];
        }
    }
    return rowMap;
}

std::vector<std::map<std::string, std::string>> MariaDbResult::get_all_rows() const {
    std::vector<std::map<std::string, std::string>> allRows;
    for (size_t i = 0; i < m_rowCount; ++i) {
        allRows.push_back(get_row(i));
    }
    return allRows;
}

std::string MariaDbResult::get_value(size_t row_index, const std::string& column_name) const {
    if (row_index >= m_rowCount) return "";
    for (size_t i = 0; i < m_columnCount; ++i) {
        if (m_columnNames[i] == column_name) {
            return m_rows[row_index][i];
        }
    }
    return "";
}

// ====================================================================
// MariaDBConnection
// ====================================================================

MariaDBConnection::MariaDBConnection()
    : m_mysql(nullptr), m_port(3306), m_connected(false) {
}

MariaDBConnection::~MariaDBConnection() {
    disconnect();
}

bool MariaDBConnection::ensure_driver() {
    return MariaDbDriver::instance().ensure_loaded();
}

void MariaDBConnection::init_mysql() {
    auto& D = MariaDbDriver::instance();
    m_mysql = D.mysql_init(nullptr);
    if (!m_mysql) {
        throw std::runtime_error("Failed to initialize MariaDB connection");
    }
    int timeout = 5;
    D.mysql_options(m_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    // 启用非阻塞模式（必须：非阻塞 *_start/*_cont 依赖此选项，缺了会空指针崩溃）。
    // 阻塞调用与阻塞/非阻塞调用可自由混用，仅要求一条在途 op 必须先跑完。
    D.mysql_options(m_mysql, MYSQL_OPT_NONBLOCK, nullptr);
}

void MariaDBConnection::set_connection_params(
    const std::string& host, const std::string& user, const std::string& password,
    const std::string& database, unsigned int port) {
    m_host = host;
    m_user = user;
    m_password = password;
    m_database = database;
    m_port = port;
}

bool MariaDBConnection::connect() {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_connected) {
        LOG_DATABASE_DEBUG("MariaDB already connected to {}:{}", m_host, m_port);
        return true;
    }
    if (!ensure_driver()) {
        LOG_DATABASE_ERROR("MariaDB driver not loaded: {}", MariaDbDriver::instance().error());
        return false;
    }

    auto& D = MariaDbDriver::instance();
    if (!m_mysql) {
        init_mysql();
    }

    LOG_DATABASE_DEBUG("MariaDB connecting: Host={}, Port={}, User={}, Database={}",
                       m_host, m_port, m_user, m_database);

    if (D.mysql_real_connect(m_mysql, m_host.c_str(), m_user.c_str(), m_password.c_str(),
                             m_database.c_str(), m_port, nullptr, 0) != nullptr) {
        m_connected = true;
        D.mysql_set_character_set(m_mysql, "utf8mb4");
        LOG_DATABASE_DEBUG("MariaDB connected successfully");
        return true;
    }

    LOG_DATABASE_ERROR("MariaDB connection error: {} (errno: {})",
                       D.mysql_error(m_mysql), D.mysql_errno(m_mysql));
    return false;
}

void MariaDBConnection::disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 先清缓存 stmt：它们绑定连接，连接关了句柄作废，必须关掉防泄漏
    clear_stmt_cache();
    if (m_mysql && MariaDbDriver::instance().mysql_close) {
        MariaDbDriver::instance().mysql_close(m_mysql);
        m_mysql = nullptr;
    }
    m_connected = false;
}

bool MariaDBConnection::is_connected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected && m_mysql;
}

std::shared_ptr<IDBResult> MariaDBConnection::query(const std::string& sql) {
    LOG_DB_QUERY_DEBUG("MariaDBConnection::query() called");
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_connected || !m_mysql) {
        lock.unlock();
        if (!connect()) return nullptr;
        lock.lock();
    }

    auto& D = MariaDbDriver::instance();
    if (D.mysql_query(m_mysql, sql.c_str()) != 0) {
        const auto err = D.mysql_errno(m_mysql);
        if (err == 1046 && !m_database.empty()) {
            LOG_DB_QUERY_WARN("No database selected, trying USE {} and retry query", m_database);
            if (D.mysql_select_db(m_mysql, m_database.c_str()) == 0 &&
                D.mysql_query(m_mysql, sql.c_str()) == 0) {
                LOG_DB_QUERY_DEBUG("Retry query succeeded after selecting database {}", m_database);
            } else {
                LOG_DB_QUERY_ERROR("MariaDB query error: {} (errno: {})",
                                   D.mysql_error(m_mysql), D.mysql_errno(m_mysql));
                return nullptr;
            }
        } else {
            LOG_DB_QUERY_ERROR("MariaDB query error: {} (errno: {})",
                               D.mysql_error(m_mysql), D.mysql_errno(m_mysql));
            return nullptr;
        }
    }

    MYSQL_RES* result = D.mysql_store_result(m_mysql);
    if (!result) {
        if (D.mysql_field_count(m_mysql) == 0) {
            LOG_DB_QUERY_DEBUG("No result set (INSERT, UPDATE, DELETE, etc.)");
            return nullptr;
        }
        LOG_DB_QUERY_ERROR("MariaDB store result error: {}", D.mysql_error(m_mysql));
        return nullptr;
    }

    return std::make_shared<MariaDbResult>(result);
}

std::shared_ptr<IDBResult> MariaDBConnection::query(
    const std::string& sql, const std::vector<std::string>& params) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_connected || !m_mysql) {
        lock.unlock();
        if (!connect()) return nullptr;
        lock.lock();
    }

    auto& D = MariaDbDriver::instance();
    MYSQL_STMT* stmt = D.mysql_stmt_init(m_mysql);
    if (!stmt) {
        LOG_DB_QUERY_ERROR("MariaDB stmt init error: {}", D.mysql_error(m_mysql));
        return nullptr;
    }
    struct StmtGuard {
        MariaDbDriver& D; MYSQL_STMT* s;
        ~StmtGuard() { if (s) D.mysql_stmt_close(s); }
    } guard{D, stmt};

    if (D.mysql_stmt_prepare(stmt, sql.c_str(), sql.length()) != 0) {
        LOG_DB_QUERY_ERROR("MariaDB stmt prepare error: {}", D.mysql_stmt_error(stmt));
        return nullptr;
    }

    size_t param_count = params.size();
    std::vector<MYSQL_BIND> binds;
    std::vector<unsigned long> lengths;
    std::vector<char> nulls;
    std::vector<std::string> param_copies;

    if (param_count > 0) {
        binds.resize(param_count);
        lengths.resize(param_count);
        nulls.resize(param_count, 0);
        param_copies = params;

        for (size_t i = 0; i < param_count; ++i) {
            binds[i].buffer_type = MYSQL_TYPE_STRING;
            binds[i].buffer = (void*)param_copies[i].c_str();
            binds[i].buffer_length = param_copies[i].length();
            binds[i].length = &lengths[i];
            lengths[i] = param_copies[i].length();
            binds[i].is_null = &nulls[i];
        }

        if (D.mysql_stmt_bind_param(stmt, binds.data()) != 0) {
            LOG_DB_QUERY_ERROR("MariaDB stmt bind param error: {}", D.mysql_stmt_error(stmt));
            return nullptr;
        }
    }

    if (D.mysql_stmt_execute(stmt) != 0) {
        LOG_DB_QUERY_ERROR("MariaDB stmt execute error: {}", D.mysql_stmt_error(stmt));
        return nullptr;
    }

    {
        MYSQL_RES* meta = D.mysql_stmt_result_metadata(stmt);
        if (!meta) {
            if (D.mysql_stmt_field_count(stmt) == 0) return nullptr;
            LOG_DB_QUERY_ERROR("MariaDB stmt result metadata error: {}", D.mysql_stmt_error(stmt));
            return nullptr;
        }

        size_t num_fields = D.mysql_num_fields(meta);
        std::vector<std::string> colNames;
        MYSQL_FIELD* fields = D.mysql_fetch_fields(meta);
        for (size_t i = 0; i < num_fields; ++i) {
            colNames.push_back(fields[i].name);
        }

        if (D.mysql_stmt_store_result(stmt) != 0) {
            LOG_DB_QUERY_ERROR("MariaDB stmt store result error: {}", D.mysql_stmt_error(stmt));
            D.mysql_free_result(meta);
            return nullptr;
        }

        std::vector<std::vector<std::string>> rows;
        std::vector<MYSQL_BIND> result_binds(num_fields);
        std::vector<std::vector<char>> buffers(num_fields);
        std::vector<unsigned long> result_lengths(num_fields);
        std::vector<char> result_nulls(num_fields);

        for (size_t i = 0; i < num_fields; ++i) {
            buffers[i].resize(fields[i].max_length > 0 ? fields[i].max_length : 256);
            memset(&result_binds[i], 0, sizeof(MYSQL_BIND));
            result_binds[i].buffer_type = MYSQL_TYPE_STRING;
            result_binds[i].buffer = buffers[i].data();
            result_binds[i].buffer_length = buffers[i].size();
            result_binds[i].length = &result_lengths[i];
            result_binds[i].is_null = &result_nulls[i];
        }

        if (D.mysql_stmt_bind_result(stmt, result_binds.data()) != 0) {
            LOG_DB_QUERY_ERROR("MariaDB stmt bind result error: {}", D.mysql_stmt_error(stmt));
            D.mysql_free_result(meta);
            return nullptr;
        }

        int fetch_rc;
        while ((fetch_rc = D.mysql_stmt_fetch(stmt)) == 0) {
            std::vector<std::string> row_data(num_fields);
            for (size_t i = 0; i < num_fields; ++i) {
                if (result_nulls[i]) {
                    row_data[i] = "";
                } else {
                    if (result_lengths[i] > buffers[i].size()) {
                        buffers[i].resize(result_lengths[i]);
                        result_binds[i].buffer = buffers[i].data();
                        result_binds[i].buffer_length = buffers[i].size();
                        D.mysql_stmt_fetch_column(stmt, &result_binds[i], i, 0);
                    }
                    row_data[i] = std::string(buffers[i].data(), result_lengths[i]);
                }
            }
            rows.push_back(std::move(row_data));
        }

        if (fetch_rc != MYSQL_NO_DATA) {
            LOG_DB_QUERY_ERROR("MariaDB stmt fetch error: {}", D.mysql_stmt_error(stmt));
            D.mysql_free_result(meta);
            return nullptr;
        }

        D.mysql_free_result(meta);
        return std::make_shared<MariaDbResult>(
            std::move(colNames), std::move(rows), rows.size(), num_fields);
    }
}

bool MariaDBConnection::execute(const std::string& sql) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_connected || !m_mysql) {
        lock.unlock();
        if (!connect()) return false;
        lock.lock();
    }

    auto& D = MariaDbDriver::instance();
    if (D.mysql_query(m_mysql, sql.c_str()) != 0) {
        const auto err = D.mysql_errno(m_mysql);
        if (err == 1046 && !m_database.empty()) {
            LOG_DB_QUERY_WARN("No database selected, trying USE {} and retry execute", m_database);
            if (D.mysql_select_db(m_mysql, m_database.c_str()) == 0 &&
                D.mysql_query(m_mysql, sql.c_str()) == 0) {
                return true;
            }
        }
        LOG_DB_QUERY_ERROR("MariaDB execute error: {} (errno: {})",
                           D.mysql_error(m_mysql), D.mysql_errno(m_mysql));
        return false;
    }
    return true;
}

bool MariaDBConnection::execute(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<ParamType> types(params.size(), ParamType::String);
    return execute(sql, params, types);
}

bool MariaDBConnection::execute(const std::string& sql, const std::vector<std::string>& params,
                                const std::vector<ParamType>& types) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_connected || !m_mysql) {
        lock.unlock();
        if (!connect()) return false;
        lock.lock();
    }

    auto& D = MariaDbDriver::instance();
    MYSQL_STMT* stmt = D.mysql_stmt_init(m_mysql);
    if (!stmt) {
        LOG_DB_QUERY_ERROR("MariaDB stmt init error: {}", D.mysql_error(m_mysql));
        return false;
    }
    struct StmtGuard {
        MariaDbDriver& D; MYSQL_STMT* s;
        ~StmtGuard() { if (s) D.mysql_stmt_close(s); }
    } guard{D, stmt};

    if (D.mysql_stmt_prepare(stmt, sql.c_str(), sql.length()) != 0) {
        LOG_DB_QUERY_ERROR("MariaDB stmt prepare error: {}", D.mysql_stmt_error(stmt));
        return false;
    }

    size_t param_count = params.size();
    std::vector<MYSQL_BIND> binds;
    std::vector<unsigned long> lengths;
    std::vector<char> nulls;
    std::vector<int> int_values;
    std::vector<std::string> param_copies;

    if (param_count > 0) {
        binds.resize(param_count);
        lengths.resize(param_count);
        nulls.resize(param_count, 0);
        int_values.resize(param_count, 0);
        param_copies = params;

        for (size_t i = 0; i < param_count; ++i) {
            ParamType type = (i < types.size()) ? types[i] : ParamType::String;
            if (type == ParamType::Int) {
                int_values[i] = std::stoi(params[i]);
                binds[i].buffer_type = MYSQL_TYPE_LONG;
                binds[i].buffer = &int_values[i];
                binds[i].buffer_length = sizeof(int_values[i]);
                binds[i].length = nullptr;
            } else {
                binds[i].buffer_type = MYSQL_TYPE_STRING;
                binds[i].buffer = (void*)param_copies[i].c_str();
                binds[i].buffer_length = param_copies[i].length();
                binds[i].length = &lengths[i];
                lengths[i] = param_copies[i].length();
            }
            binds[i].is_null = &nulls[i];
        }

        if (D.mysql_stmt_bind_param(stmt, binds.data()) != 0) {
            LOG_DB_QUERY_ERROR("MariaDB stmt bind param error: {}", D.mysql_stmt_error(stmt));
            return false;
        }
    }

    if (D.mysql_stmt_execute(stmt) != 0) {
        LOG_DB_QUERY_ERROR("MariaDB stmt execute error: {}", D.mysql_stmt_error(stmt));
        return false;
    }

    my_ulonglong affected_rows = D.mysql_stmt_affected_rows(stmt);
    LOG_DB_QUERY_DEBUG("MariaDB stmt affected rows: {}", affected_rows);
    return true;
}

bool MariaDBConnection::begin_transaction() { return execute("START TRANSACTION"); }
bool MariaDBConnection::commit() { return execute("COMMIT"); }
bool MariaDBConnection::rollback() { return execute("ROLLBACK"); }

std::string MariaDBConnection::get_last_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_mysql && MariaDbDriver::instance().mysql_error) {
        return MariaDbDriver::instance().mysql_error(m_mysql);
    }
    return "MariaDB connection not initialized";
}

std::string MariaDBConnection::escape_string(const std::string& str) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_mysql) return str;

    char* escaped = new char[str.length() * 2 + 1];
    MariaDbDriver::instance().mysql_real_escape_string(
        m_mysql, escaped, str.c_str(), str.length());
    std::string result(escaped);
    delete[] escaped;
    return result;
}

// ====================================================================
// 保活（Phase 3）：mysql_ping —— COM_PING，不跑查询、不产生结果集、
// 不碰 prepared stmt 状态。池 checkout 校验走它（替代 SELECT 1）。
// ====================================================================
bool MariaDBConnection::ping() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected || !m_mysql) return false;
    return MariaDbDriver::instance().mysql_ping(m_mysql) == 0;
}

// ====================================================================
// prepared stmt 缓存（Phase 3）：每连接 SQL→MYSQL_STMT*，省 prepare 往返。
// 访问约定：以下 helper 只在持 m_mutex 时调用（async step / disconnect 都持锁）。
// ====================================================================

// 缓存命中。命中返回的 stmt 归缓存所有（析构不 close）。
MYSQL_STMT* MariaDBConnection::lookup_stmt(const std::string& sql) {
    auto it = m_stmtCache.find(sql);
    return it != m_stmtCache.end() ? it->second : nullptr;
}

// 入缓存。超限整体清空重建（批量 IN 变长 SQL 会撑大 key 集合，宁可偶尔重 prepare
// 也不让缓存无限涨）。
void MariaDBConnection::store_stmt(const std::string& sql, MYSQL_STMT* stmt) {
    if (m_stmtCache.size() >= kMaxCachedStmts) clear_stmt_cache();
    m_stmtCache[sql] = stmt;
}

// 关掉全部缓存 stmt 并清空。
void MariaDBConnection::clear_stmt_cache() {
    auto& D = MariaDbDriver::instance();
    for (auto& kv : m_stmtCache) {
        if (kv.second && D.mysql_stmt_close) D.mysql_stmt_close(kv.second);
    }
    m_stmtCache.clear();
}

// ====================================================================
// 阶段 2：非阻塞状态机（io 线程 async_wait / worker 阻塞 poll）
// ====================================================================
// libmariadb 非阻塞协议是请求-响应：*_start 首次发起，返回 0=已完成（*ret 存阻塞版
// 返回值）或 MYSQL_WAIT_READ/WRITE/EXCEPT/TIMEOUT 位掩码（需等 socket/超时）；
// *_cont(stmt, 上轮掩码) 在事件就绪后续，返回语义同 *_start。一个 stmt 查询的完整
// 阶段：prepare → bind → execute →（有结果集）store_result → bind_result + fetch
// （store 完成后行数据全在客户端内存，fetch 不碰网络）。
//
// 连接被 FSM 链独占（ScopedConnection 持链）→ 每连接单飞行是结构性保证；op 状态只在
// 驱动线程推进。io 线程路径等 socket 时把控制权交还 io 事件循环（io_context.async_wait），
// worker 线程路径阻塞 poll（worker 本就干阻塞活）。
// 一条非阻塞查询/执行的驱动状态机。run() 迭代推进 step()：Wait → 调度续作（io
// async_wait / worker 阻塞 poll）后交还或继续；Done/Fail → 锁外跑 finish（回调可能
// 重入连接，不能在持锁时调）。worker 路径是迭代循环而非递归——大结果集 store_result
// 分多块拉取也不会爆栈。
// 必须是 MariaDBConnection 的嵌套结构体（header 里已前置声明 struct AsyncStmtOp），
// 否则访问不了 private m_mutex/m_mysql；read_stmt_rows 等纯工具函数以 static 留在
// mail_system 命名空间。
struct MariaDBConnection::AsyncStmtOp : std::enable_shared_from_this<AsyncStmtOp> {
    MariaDBConnection* conn;                    // 非拥有（owner 保活）
    std::shared_ptr<MariaDBConnection> owner;   // 保活到 op 结束
    bool want_result;                           // true=query false=execute
    std::string sql;
    std::vector<std::string> params;

    MYSQL_STMT* stmt = nullptr;
    bool stmt_is_cached = false;                // stmt 归连接缓存所有（析构不 close）
    bool prepare_started = false;
    bool prepare_done = false;
    bool bound = false;
    bool execute_started = false;
    bool execute_done = false;
    bool store_started = false;
    bool store_done = false;
    int last_wait = 0;                          // 最近一次 _start/_cont 返回的掩码

    // bind 参数缓冲：prepare 完成后绑定一次，execute 期间保持存活
    std::vector<MYSQL_BIND> binds;
    std::vector<unsigned long> bind_lengths;
    std::vector<char> bind_nulls;
    std::vector<std::string> param_copies;

    std::shared_ptr<IDBResult> result;          // query 完成产出
    bool exec_ok = false;                       // execute 完成产出

    // 完成回调（result, ok）。⚠ 绝不能捕获 op 自身——op 持有 done，done 再捕获 op
    // 就是 shared_ptr 循环，op（连同捕获了 ScopedConnection 的用户 cb）永远不释放，
    // 连接池会被"查完但未归还"的连接打爆。由 run() 在 op 存活时调用，result/ok
    // 以参数传入，不读 op 成员。
    std::function<void(std::shared_ptr<IDBResult>, bool)> done;

    void complete_fail() { if (done) done(nullptr, false); }

    // 持锁调用：缓存 stmt 执行/读取出错（连接死或 stmt 状态坏）→ 整体清缓存。
    // 全部缓存 stmt 都依赖同一连接，连接挂了必然一起失效；宁可下次重 prepare。
    // 清完置 stmt=nullptr，析构不再 close（clear_stmt_cache 已统一关）。
    void invalidate_cached() {
        if (stmt_is_cached) {
            conn->clear_stmt_cache();
            stmt = nullptr;
            stmt_is_cached = false;
        }
    }

    ~AsyncStmtOp();

    enum class Step { Wait, Done, Fail };
    struct StepResult { Step s; int mask; };

    StepResult step();
    void run();
    void schedule_async_wait(int mask);
    bool blocking_wait(int mask);
};

MariaDBConnection::AsyncStmtOp::~AsyncStmtOp() {
    auto& D = MariaDbDriver::instance();
    // 缓存命中的 stmt 归连接缓存所有（连接 disconnect/clear_stmt_cache 时统一关），
    // 这里只关非缓存的临时 stmt（prepare 失败/未入缓存的）。
    if (stmt && !stmt_is_cached) {
        if (D.mysql_stmt_close) D.mysql_stmt_close(stmt);
    }
    stmt = nullptr;
}

// store 完成后逐行读进内存（纯内存，不碰网络）；失败返回 nullptr。
// 与同步 query() 的结果读取完全一致（列缓冲 + 截断重拉 + NULL 处理）。
static std::shared_ptr<IDBResult> read_stmt_rows(MYSQL_STMT* stmt, MariaDbDriver& D) {
    MYSQL_RES* meta = D.mysql_stmt_result_metadata(stmt);
    if (!meta) {
        LOG_DB_QUERY_ERROR("MariaDB async: stmt result metadata error: {}",
                           D.mysql_stmt_error(stmt));
        return nullptr;
    }
    size_t num_fields = D.mysql_num_fields(meta);
    std::vector<std::string> colNames;
    MYSQL_FIELD* fields = D.mysql_fetch_fields(meta);
    for (size_t i = 0; i < num_fields; ++i) colNames.push_back(fields[i].name);

    std::vector<std::vector<std::string>> rows;
    std::vector<MYSQL_BIND> rbinds(num_fields);
    std::vector<std::vector<char>> buffers(num_fields);
    std::vector<unsigned long> rlens(num_fields);
    std::vector<char> rnulls(num_fields);

    for (size_t i = 0; i < num_fields; ++i) {
        buffers[i].resize(fields[i].max_length > 0 ? fields[i].max_length : 256);
        memset(&rbinds[i], 0, sizeof(MYSQL_BIND));
        rbinds[i].buffer_type = MYSQL_TYPE_STRING;
        rbinds[i].buffer = buffers[i].data();
        rbinds[i].buffer_length = buffers[i].size();
        rbinds[i].length = &rlens[i];
        rbinds[i].is_null = &rnulls[i];
    }

    if (D.mysql_stmt_bind_result(stmt, rbinds.data()) != 0) {
        LOG_DB_QUERY_ERROR("MariaDB async: stmt bind result error: {}",
                           D.mysql_stmt_error(stmt));
        D.mysql_free_result(meta);
        return nullptr;
    }

    int fetch_rc;
    while ((fetch_rc = D.mysql_stmt_fetch(stmt)) == 0) {
        std::vector<std::string> row_data(num_fields);
        for (size_t i = 0; i < num_fields; ++i) {
            if (rnulls[i]) {
                row_data[i] = "";
            } else {
                if (rlens[i] > buffers[i].size()) {
                    buffers[i].resize(rlens[i]);
                    rbinds[i].buffer = buffers[i].data();
                    rbinds[i].buffer_length = buffers[i].size();
                    D.mysql_stmt_fetch_column(stmt, &rbinds[i], (unsigned int)i, 0);
                }
                row_data[i] = std::string(buffers[i].data(), rlens[i]);
            }
        }
        rows.push_back(std::move(row_data));
    }

    if (fetch_rc != MYSQL_NO_DATA) {
        LOG_DB_QUERY_ERROR("MariaDB async: stmt fetch error: {}", D.mysql_stmt_error(stmt));
        D.mysql_free_result(meta);
        return nullptr;
    }
    D.mysql_free_result(meta);
    return std::make_shared<MariaDbResult>(
        std::move(colNames), std::move(rows), rows.size(), num_fields);
}

// 推进一步。调用方保证锁内/锁外语义：本函数自己锁 conn->m_mutex，返回时锁已释放。
MariaDBConnection::AsyncStmtOp::StepResult MariaDBConnection::AsyncStmtOp::step() {
    std::lock_guard<std::mutex> lk(conn->m_mutex);
    auto& D = MariaDbDriver::instance();
    if (!conn->m_mysql) return {Step::Fail, 0};

    // ---- stmt 获取：缓存命中直接复用（跳过 prepare 往返）；未命中新建后缓存 ----
    if (!stmt) {
        stmt = conn->lookup_stmt(sql);        // 持锁调用
        if (stmt) {
            stmt_is_cached = true;
            prepare_done = true;              // 复用：省 prepare，直接 bind + execute
        } else {
            stmt = D.mysql_stmt_init(conn->m_mysql);
            if (!stmt) {
                LOG_DB_QUERY_ERROR("MariaDB async: stmt init error: {}",
                                   D.mysql_error(conn->m_mysql));
                return {Step::Fail, 0};
            }
        }
    }

    // ---- prepare（非阻塞，仅缓存未命中时） ----
    int ret = 0;
    if (!prepare_done) {
        int rc;
        if (!prepare_started) {
            rc = D.mysql_stmt_prepare_start(&ret, stmt, sql.data(), (unsigned long)sql.size());
            prepare_started = true;
        } else {
            rc = D.mysql_stmt_prepare_cont(&ret, stmt, last_wait);
        }
        if (rc) { last_wait = rc; return {Step::Wait, rc}; }
        if (ret != 0) {
            LOG_DB_QUERY_ERROR("MariaDB async: prepare error: {} (sql={})",
                               D.mysql_stmt_error(stmt), sql);
            if (stmt && !stmt_is_cached) { D.mysql_stmt_close(stmt); stmt = nullptr; }
            return {Step::Fail, 0};
        }
        // 新 prepare 完成 → 入缓存（缓存所有权，析构不再 close）
        conn->store_stmt(sql, stmt);
        stmt_is_cached = true;
        prepare_done = true;
    }

    // ---- bind 参数（纯内存，prepare 完成后一次） ----
    if (!bound) {
        size_t n = params.size();
        if (n > 0) {
            binds.resize(n);
            bind_lengths.resize(n);
            bind_nulls.assign(n, 0);
            param_copies = params;
            for (size_t i = 0; i < n; ++i) {
                binds[i].buffer_type = MYSQL_TYPE_STRING;
                binds[i].buffer = (void*)param_copies[i].c_str();
                binds[i].buffer_length = param_copies[i].length();
                binds[i].length = &bind_lengths[i];
                bind_lengths[i] = param_copies[i].length();
                binds[i].is_null = &bind_nulls[i];
            }
            if (D.mysql_stmt_bind_param(stmt, binds.data()) != 0) {
                LOG_DB_QUERY_ERROR("MariaDB async: bind param error: {}",
                                   D.mysql_stmt_error(stmt));
                return {Step::Fail, 0};
            }
        }
        bound = true;
    }

    // ---- execute（非阻塞） ----
    if (!execute_done) {
        int rc;
        if (!execute_started) {
            rc = D.mysql_stmt_execute_start(&ret, stmt);
            execute_started = true;
        } else {
            rc = D.mysql_stmt_execute_cont(&ret, stmt, last_wait);
        }
        if (rc) { last_wait = rc; return {Step::Wait, rc}; }
        if (ret != 0) {
            LOG_DB_QUERY_ERROR("MariaDB async: execute error: {}", D.mysql_stmt_error(stmt));
            invalidate_cached();
            return {Step::Fail, 0};
        }
        execute_done = true;
    }

    // ---- 无结果集（INSERT/UPDATE/DELETE 等）→ 完成 ----
    unsigned int fcount = D.mysql_stmt_field_count(stmt);
    if (fcount == 0) {
        exec_ok = true;
        return {Step::Done, 0};
    }

    // ---- store_result（把全部行拉进客户端内存，非阻塞） ----
    if (!store_done) {
        int rc;
        if (!store_started) {
            rc = D.mysql_stmt_store_result_start(&ret, stmt);
            store_started = true;
        } else {
            rc = D.mysql_stmt_store_result_cont(&ret, stmt, last_wait);
        }
        if (rc) { last_wait = rc; return {Step::Wait, rc}; }
        if (ret != 0) {
            LOG_DB_QUERY_ERROR("MariaDB async: store result error: {}",
                               D.mysql_stmt_error(stmt));
            invalidate_cached();
            return {Step::Fail, 0};
        }
        store_done = true;
    }

    // execute() 误用到带结果集的 SQL：拉完即弃，保持连接干净（协议是请求-响应，
    // 不消费结果集会让下一条命令 "commands out of sync"）。
    if (!want_result) {
        D.mysql_stmt_free_result(stmt);
        exec_ok = true;
        return {Step::Done, 0};
    }

    // ---- 读行（store 完成后全在内存） ----
    result = read_stmt_rows(stmt, D);
    if (!result) {
        invalidate_cached();
        return {Step::Fail, 0};
    }
    // 缓存 stmt 复用：结果已全量消费，free_result 为下次 execute 腾干净状态
    // （省掉 mysql_stmt_reset 那趟往返）。
    if (stmt_is_cached) D.mysql_stmt_free_result(stmt);
    return {Step::Done, 0};
}

// io 线程路径：用 io_context.async_wait 等 socket 就绪，控制权交还事件循环。
// 一个 fd 上可能同时等多个事件（READ|WRITE|EXCEPT），各自注册 async_wait；
// 首个触发的续作（fired 自增），其余忽略。posix::stream_descriptor::assign 会
// 接管 fd 所有权（析构时 close），续作回调里必须 release() 归还 mariadb。
void MariaDBConnection::AsyncStmtOp::schedule_async_wait(int mask) {
    auto& D = MariaDbDriver::instance();
    boost::asio::io_context* ctx = current_io_context();
    int fd = 0;
    unsigned int timeout_ms = 0;
    {
        std::lock_guard<std::mutex> lk(conn->m_mutex);
        if (!conn->m_mysql) { complete_fail(); return; }
        fd = static_cast<int>(D.mysql_get_socket(conn->m_mysql));
        if (D.mysql_get_timeout_value_ms)
            timeout_ms = D.mysql_get_timeout_value_ms(conn->m_mysql);
    }

    int io_events = mask & (MYSQL_WAIT_READ | MYSQL_WAIT_WRITE | MYSQL_WAIT_EXCEPT);
    if (io_events == 0) {
        // 纯超时（MYSQL_WAIT_TIMEOUT）或无事件：用 steady_timer 等 timeout 后续
        auto self = shared_from_this();
        auto fired = std::make_shared<std::atomic<int>>(0);
        auto timer = std::make_shared<boost::asio::steady_timer>(*ctx);
        timer->expires_after(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 1000));
        timer->async_wait([self, timer, fired](const boost::system::error_code& tec) {
            if (fired->exchange(1) == 0) {
                self->last_wait = MYSQL_WAIT_TIMEOUT;
                self->run();
            }
        });
        return;
    }

    auto self = shared_from_this();
    auto fired = std::make_shared<std::atomic<int>>(0);
    auto desc = std::make_shared<boost::asio::posix::stream_descriptor>(ctx->get_executor());
    boost::system::error_code ec;
    // 带 ec 的 assign 重载在本构建（无 BOOST_ASIO_NO_DEPRECATED）返回 error_code，
    // 值即 ec——收下它（同名互赋）满足 clang-tidy unused-return-value。
    ec = desc->assign(fd, ec);
    if (ec) {
        desc->release();   // assign 失败也要归还 fd，避免析构 close 掉 mariadb 的 socket
        LOG_DB_QUERY_ERROR("MariaDB async: descriptor assign error: {}", ec.message());
        complete_fail();
        return;
    }

    // 每个请求的事件注册一个 async_wait；首个触发者续作并 release，其余忽略。
    auto make_handler = [self, desc, fired](int event) {
        return [self, desc, fired, event](const boost::system::error_code& wec) {
            if (wec) {
                if (fired->exchange(1) == 0) {
                    desc->release();
                    self->complete_fail();
                }
                return;
            }
            if (fired->fetch_add(1) == 0) {
                desc->release();          // 归还 fd 所有权给 mariadb
                self->last_wait = event;
                self->run();
            }
        };
    };

    if (io_events & MYSQL_WAIT_READ)
        desc->async_wait(boost::asio::posix::stream_descriptor::wait_read,
                         make_handler(MYSQL_WAIT_READ));
    if (io_events & MYSQL_WAIT_WRITE)
        desc->async_wait(boost::asio::posix::stream_descriptor::wait_write,
                         make_handler(MYSQL_WAIT_WRITE));
    if (io_events & MYSQL_WAIT_EXCEPT)
        desc->async_wait(boost::asio::posix::stream_descriptor::wait_error,
                         make_handler(MYSQL_WAIT_EXCEPT));
}

// worker 线程路径：阻塞 poll 直到 socket 就绪/超时；返回 false 表示连接失效，直接收尾。
bool MariaDBConnection::AsyncStmtOp::blocking_wait(int mask) {
    auto& D = MariaDbDriver::instance();
    int fd = 0;
    unsigned int timeout_ms = 0;
    {
        std::lock_guard<std::mutex> lk(conn->m_mutex);
        if (!conn->m_mysql) return false;
        fd = static_cast<int>(D.mysql_get_socket(conn->m_mysql));
        if (D.mysql_get_timeout_value_ms)
            timeout_ms = D.mysql_get_timeout_value_ms(conn->m_mysql);
    }

    int io_events = mask & (MYSQL_WAIT_READ | MYSQL_WAIT_WRITE | MYSQL_WAIT_EXCEPT);
    if (io_events != 0) {
        struct pollfd pfd{fd, 0, 0};
        if (io_events & MYSQL_WAIT_READ) pfd.events |= POLLIN;
        if (io_events & MYSQL_WAIT_WRITE) pfd.events |= POLLOUT;
        if (io_events & MYSQL_WAIT_EXCEPT) pfd.events |= POLLPRI;
        int rc = ::poll(&pfd, 1, timeout_ms > 0 ? static_cast<int>(timeout_ms) : -1);
        if (rc < 0) {
            if (errno == EINTR) { last_wait = mask; return true; }
            return false;
        }
        if (rc == 0) { last_wait = MYSQL_WAIT_TIMEOUT; return true; }
        int fired = 0;
        if (pfd.revents & POLLIN) fired |= MYSQL_WAIT_READ;
        if (pfd.revents & POLLOUT) fired |= MYSQL_WAIT_WRITE;
        if (pfd.revents & POLLPRI) fired |= MYSQL_WAIT_EXCEPT;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        last_wait = fired ? fired : mask;
        return true;
    }

    // 纯超时等待
    if (timeout_ms > 0) {
        struct timespec ts{static_cast<time_t>(timeout_ms / 1000),
                           static_cast<long>((timeout_ms % 1000) * 1000000)};
        nanosleep(&ts, nullptr);
    }
    last_wait = MYSQL_WAIT_TIMEOUT;
    return true;
}

void MariaDBConnection::AsyncStmtOp::run() {
    for (;;) {
        StepResult r = step();
        if (r.s == Step::Done || r.s == Step::Fail) {
            // step() 已释放锁；回调可能重入连接。result/ok 以参数传入，不读 op 成员——
            // done 不能捕获 op（否则 op↔done 循环泄漏连接）。
            if (done) done(result, r.s == Step::Done && exec_ok);
            return;
        }
        if (current_io_context()) {
            schedule_async_wait(r.mask);   // io 线程：交还事件循环
            return;
        }
        if (!blocking_wait(r.mask)) {      // worker：阻塞 poll 后继续迭代
            exec_ok = false;
            result.reset();
            if (done) done(nullptr, false);
            return;
        }
    }
}

// ====================================================================
// MariaDBConnection —— 非阻塞 async override（阶段 2）
// ====================================================================
// 统一入口：构造 op（捕获 conn + sql + params + 完成回调），设 m_asyncInFlight
// 守卫，然后 op->run() 驱动。finish 回调先清 in-flight 再跑用户回调——用户回调会
// 继续 FSM 链，可能立刻在同一条连接上发起下一条查询（链式 CPS）。
void MariaDBConnection::start_async_op(const std::shared_ptr<AsyncStmtOp>& op) {
    bool expected = false;
    if (!m_asyncInFlight.compare_exchange_strong(expected, true)) {
        LOG_DB_QUERY_ERROR(
            "MariaDB async: connection {} 上已有 in-flight op（FSM 链未串行同一连接）",
            (void*)this);
        op->complete_fail();
        return;
    }
    op->run();
}

void MariaDBConnection::async_query(const std::string& sql, QueryCallback cb) {
    async_query(sql, std::vector<std::string>{}, std::move(cb));
}

void MariaDBConnection::async_query(const std::string& sql,
                                    const std::vector<std::string>& params,
                                    QueryCallback cb) {
    if (!MariaDbDriver::instance().has_nonblocking()) {
        // 缺非阻塞符号（老 libmariadb）：回退同步执行（旧行为）
        if (cb) cb(query(sql, params));
        return;
    }
    auto op = std::make_shared<AsyncStmtOp>();
    op->conn = this;
    op->owner = shared_from_this();
    op->want_result = true;
    op->sql = sql;
    op->params = params;
    // done 只捕获 self（连接）和用户 cb，绝不捕获 op：op 持有 done，捕获 op 即循环。
    op->done = [self = shared_from_this(), cb = std::move(cb)](
        std::shared_ptr<IDBResult> result, bool /*ok*/) {
        self->m_asyncInFlight.store(false, std::memory_order_release);
        if (cb) cb(result);
    };
    start_async_op(op);
}

void MariaDBConnection::async_execute(const std::string& sql, ExecuteCallback cb) {
    async_execute(sql, std::vector<std::string>{}, std::move(cb));
}

void MariaDBConnection::async_execute(const std::string& sql,
                                      const std::vector<std::string>& params,
                                      ExecuteCallback cb) {
    if (!MariaDbDriver::instance().has_nonblocking()) {
        if (cb) cb(execute(sql, params));
        return;
    }
    auto op = std::make_shared<AsyncStmtOp>();
    op->conn = this;
    op->owner = shared_from_this();
    op->want_result = false;
    op->sql = sql;
    op->params = params;
    op->done = [self = shared_from_this(), cb = std::move(cb)](
        std::shared_ptr<IDBResult> /*result*/, bool ok) {
        self->m_asyncInFlight.store(false, std::memory_order_release);
        if (cb) cb(ok);
    };
    start_async_op(op);
}

void MariaDBConnection::async_begin_transaction(ExecuteCallback cb) {
    async_execute("START TRANSACTION", std::move(cb));
}

void MariaDBConnection::async_commit(ExecuteCallback cb) {
    async_execute("COMMIT", std::move(cb));
}

void MariaDBConnection::async_rollback(ExecuteCallback cb) {
    async_execute("ROLLBACK", std::move(cb));
}

// ====================================================================
// MariaDBService
// ====================================================================

std::shared_ptr<IDBConnection> MariaDBService::create_connection(
    const std::string& host, const std::string& user, const std::string& password,
    const std::string& database, unsigned int port) {
    auto conn = std::make_shared<MariaDBConnection>();
    conn->set_connection_params(host, user, password, database, port);
    return conn;
}

} // namespace mail_system
