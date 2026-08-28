#include "mail_system/back/db/mariadb_service.h"
#include "mail_system/back/common/logger.h"
#include <dlfcn.h>
#include <cstring>
#include <stdexcept>

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
    ok &= sym(&mysql_stmt_error,              "mysql_stmt_error");
    ok &= sym(&mysql_stmt_affected_rows,      "mysql_stmt_affected_rows");
    ok &= sym(&mysql_real_escape_string,      "mysql_real_escape_string");
    if (!ok) {
        LOG_DATABASE_ERROR("MariaDbDriver: dlsym 缺必需符号: {}", error_);
        return false;
    }
    // 阶段 2 预留：非阻塞 API（可选，缺则不启用 async）
    sym_optional(&mysql_stmt_execute_start,   "mysql_stmt_execute_start");
    sym_optional(&mysql_stmt_execute_cont,    "mysql_stmt_execute_cont");
    sym_optional(&mysql_get_socket,           "mysql_get_socket");

    loaded_ = true;
    LOG_DATABASE_INFO("MariaDbDriver: libmariadb loaded ({} symbol(s) bound)", 32);
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
