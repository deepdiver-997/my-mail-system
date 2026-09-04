#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <stdexcept>

namespace mail_system {

// 静态成员初始化
std::unique_ptr<MySQLService> MySQLService::s_instance = nullptr;
std::mutex MySQLService::s_mutex;

// MySQLResult实现

MySQLResult::MySQLResult(MYSQL_RES* result)
    : m_result(result), m_rowCount(0), m_columnCount(0) {
    if (m_result) {
        load_result_data();
        mysql_free_result(m_result);
        m_result = nullptr;
    }
}

MySQLResult::MySQLResult(std::vector<std::string> colNames,
                         std::vector<std::vector<std::string>> rows,
                         size_t rowCount, size_t colCount)
    : m_result(nullptr)
    , m_columnNames(std::move(colNames))
    , m_rows(std::move(rows))
    , m_rowCount(rowCount)
    , m_columnCount(colCount) {}

MySQLResult::~MySQLResult() {
    // m_result is already freed in constructor (if was non-null)
    // keeping this check as safety net, but should be nullptr by now
    if (m_result) {
        mysql_free_result(m_result);
        m_result = nullptr;
    }
}

void MySQLResult::load_result_data() {
    m_rowCount = mysql_num_rows(m_result);
    m_columnCount = mysql_num_fields(m_result);

    // 获取列名
    MYSQL_FIELD* fields = mysql_fetch_fields(m_result);
    for (size_t i = 0; i < m_columnCount; ++i) {
        m_columnNames.push_back(fields[i].name);
    }

    // 获取所有行数据
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(m_result))) {
        std::vector<std::string> rowData;
        unsigned long* lengths = mysql_fetch_lengths(m_result);
        for (size_t i = 0; i < m_columnCount; ++i) {
            if (row[i]) {
                rowData.push_back(std::string(row[i], lengths[i]));
            } else {
                rowData.push_back("");
            }
        }
        m_rows.push_back(rowData);
    }
}

size_t MySQLResult::get_row_count() const {
    return m_rowCount;
}

size_t MySQLResult::get_column_count() const {
    return m_columnCount;
}

std::vector<std::string> MySQLResult::get_column_names() const {
    return m_columnNames;
}

std::map<std::string, std::string> MySQLResult::get_row(size_t row_index) const {
    std::map<std::string, std::string> rowMap;
    if (row_index < m_rowCount) {
        for (size_t i = 0; i < m_columnCount; ++i) {
            rowMap[m_columnNames[i]] = m_rows[row_index][i];
        }
    }
    return rowMap;
}

std::vector<std::map<std::string, std::string>> MySQLResult::get_all_rows() const {
    std::vector<std::map<std::string, std::string>> allRows;
    for (size_t i = 0; i < m_rowCount; ++i) {
        allRows.push_back(get_row(i));
    }
    return allRows;
}

std::string MySQLResult::get_value(size_t row_index, const std::string& column_name) const {
    if (row_index >= m_rowCount) {
        return "";
    }

    for (size_t i = 0; i < m_columnCount; ++i) {
        if (m_columnNames[i] == column_name) {
            return m_rows[row_index][i];
        }
    }
    return "";
}

// MySQLConnection实现

MySQLConnection::MySQLConnection()
    : m_mysql(nullptr), m_port(3306), m_connected(false) {
    LOG_DATABASE_DEBUG("MySQLConnection constructor called");
    init_mysql();
}

MySQLConnection::~MySQLConnection() {
    disconnect();
}

void MySQLConnection::init_mysql() {
    LOG_DATABASE_DEBUG("MySQLConnection::init_mysql() called");
    m_mysql = mysql_init(nullptr);
    if (!m_mysql) {
        throw std::runtime_error("Failed to initialize MySQL connection");
    }

    // 设置自动重连
    // bool reconnect = true;
    // mysql_options(m_mysql, MYSQL_OPT_RECONNECT, &reconnect);

    // 设置连接超时
    int timeout = 5;
    mysql_options(m_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
}

void MySQLConnection::set_connection_params(
    const std::string& host,
    const std::string& user,
    const std::string& password,
    const std::string& database,
    unsigned int port
) {
    m_host = host;
    m_user = user;
    m_password = password;
    m_database = database;
    m_port = port;
}

bool MySQLConnection::connect() {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_connected) {
        LOG_DATABASE_DEBUG("Already connected to {}:{}", m_host, m_port);
        return true;
    }

    if (!m_mysql) {
        LOG_DATABASE_DEBUG("MySQL not initialized, calling init_mysql()");
        init_mysql();
    }

    LOG_DATABASE_DEBUG("Attempting to connect to MySQL server: Host={}, Port={}, User={}, Database={}",
                      m_host, m_port, m_user, m_database);

    // 首先尝试连接到指定数据库
    if (mysql_real_connect(
            m_mysql,
            m_host.c_str(),
            m_user.c_str(),
            m_password.c_str(),
            m_database.c_str(),
            m_port,
            nullptr,
            0
        ) != nullptr) {
        m_connected = true;
        mysql_set_character_set(m_mysql, "utf8mb4");
        LOG_DATABASE_DEBUG("Successfully connected to MySQL server and database");
        return true;
    }

    // 如果连接失败，记录错误
    unsigned int mysql_err = mysql_errno(m_mysql);
    const char* mysql_error_str = mysql_error(m_mysql);
    LOG_DATABASE_ERROR("MySQL connection error (with database): {} (errno: {})", mysql_error_str, mysql_err);

    // 如果错误是"Unknown database" (errno 1049)，尝试不指定数据库连接
    if (mysql_err == 1049 || mysql_err == 2002) {
        LOG_DATABASE_WARN("Database does not exist or connection failed, trying without specifying database");

        // 重新初始化 MySQL 连接
        if (m_mysql) {
            mysql_close(m_mysql);
            m_mysql = nullptr;
        }
        init_mysql();

        // 不指定数据库连接
        if (mysql_real_connect(
                m_mysql,
                m_host.c_str(),
                m_user.c_str(),
                m_password.c_str(),
                nullptr,  // 不指定数据库
                m_port,
                nullptr,
                0
            ) != nullptr) {
            m_connected = true;
            mysql_set_character_set(m_mysql, "utf8mb4");
            LOG_DATABASE_DEBUG("Successfully connected to MySQL server (without database)");
            return true;
        }
    }

    LOG_DATABASE_ERROR("MySQL connection error (final attempt): {} (errno: {})",
                      mysql_error(m_mysql), mysql_errno(m_mysql));
    return false;
}

void MySQLConnection::disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_mysql) {
        mysql_close(m_mysql);
        m_mysql = nullptr;
    }
    m_connected = false;
}

bool MySQLConnection::is_connected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected && m_mysql;
}

std::shared_ptr<IDBResult> MySQLConnection::query(const std::string& sql) {
    LOG_DB_QUERY_DEBUG("MySQLConnection::query() called");
    LOG_DB_QUERY_DEBUG("SQL: {}", sql);

    std::unique_lock<std::mutex> lock(m_mutex);

    // 直接检查成员变量，避免在持有锁的情况下调用 is_connected() 导致死锁
    if (!m_connected || !m_mysql) {
        LOG_DB_QUERY_DEBUG("Not connected, calling connect()...");

        // 释放锁以避免在 connect() 中死锁
        lock.unlock();

        if (!connect()) {
            LOG_DB_QUERY_ERROR("Failed to connect");
            return nullptr;
        }

        // 重新获取锁
        lock.lock();
    } else {
        LOG_DB_QUERY_DEBUG("Already connected");
    }

    LOG_DB_QUERY_DEBUG("Executing mysql_query()...");
    if (mysql_query(m_mysql, sql.c_str()) != 0) {
        const auto err = mysql_errno(m_mysql);
        if (err == 1046 && !m_database.empty()) {
            LOG_DB_QUERY_WARN("No database selected, trying USE {} and retry query", m_database);
            if (mysql_select_db(m_mysql, m_database.c_str()) == 0 &&
                mysql_query(m_mysql, sql.c_str()) == 0) {
                LOG_DB_QUERY_DEBUG("Retry query succeeded after selecting database {}", m_database);
            } else {
                LOG_DB_QUERY_ERROR("MySQL query error: {} (errno: {})",
                                   mysql_error(m_mysql), mysql_errno(m_mysql));
                return nullptr;
            }
        } else {
        LOG_DB_QUERY_ERROR("MySQL query error: {} (errno: {})",
                         mysql_error(m_mysql), mysql_errno(m_mysql));
        return nullptr;
        }
    }

    LOG_DB_QUERY_DEBUG("Query executed successfully, storing result...");
    MYSQL_RES* result = mysql_store_result(m_mysql);
    if (!result) {
        if (mysql_field_count(m_mysql) == 0) {
            // 没有结果集的查询（如INSERT, UPDATE, DELETE）
            LOG_DB_QUERY_DEBUG("No result set (INSERT, UPDATE, DELETE, etc.)");
            return nullptr;
        } else {
            // 查询出错
            LOG_DB_QUERY_ERROR("MySQL store result error: {}", mysql_error(m_mysql));
            return nullptr;
        }
    }

    LOG_DB_QUERY_DEBUG("Returning result");
    return std::make_shared<MySQLResult>(result);
}

std::shared_ptr<IDBResult> MySQLConnection::query(const std::string& sql, const std::vector<std::string>& params) {
    std::unique_lock<std::mutex> lock(m_mutex);

    // 直接检查成员变量，避免在持有锁的情况下调用 is_connected() 导致死锁
    if (!m_connected || !m_mysql) {
        // 释放锁以避免在 connect() 中死锁
        lock.unlock();

        if (!connect()) {
            return nullptr;
        }

        // 重新获取锁
        lock.lock();
    }

    // 准备语句
    MYSQL_STMT* stmt = mysql_stmt_init(m_mysql);
    if (!stmt) {
        LOG_DB_QUERY_ERROR("MySQL stmt init error: {}", mysql_error(m_mysql));
        return nullptr;
    }

    // 清理资源的RAII包装
    struct StmtGuard {
        MYSQL_STMT* s;
        ~StmtGuard() { if (s) mysql_stmt_close(s); }
    } guard{stmt};

    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()) != 0) {
        LOG_DB_QUERY_ERROR("MySQL stmt prepare error: {}", mysql_stmt_error(stmt));
        return nullptr;
    }

    // 绑定参数 — 变量必须存活到 mysql_stmt_execute 之后
    size_t param_count = params.size();
    std::vector<MYSQL_BIND> binds;
    std::vector<unsigned long> lengths;
    std::vector<char> nulls;
    std::vector<std::string> param_copies;

    if (param_count > 0) {
        binds.resize(param_count);
        lengths.resize(param_count);
        nulls.resize(param_count, 0);
        param_copies = params;  // 复制一份，确保执行期间缓冲区有效

        for (size_t i = 0; i < param_count; ++i) {
            binds[i].buffer_type = MYSQL_TYPE_STRING;
            binds[i].buffer = (void*)param_copies[i].c_str();
            binds[i].buffer_length = param_copies[i].length();
            binds[i].length = &lengths[i];
            lengths[i] = param_copies[i].length();
            binds[i].is_null = (bool*)&nulls[i];
        }

        if (mysql_stmt_bind_param(stmt, binds.data()) != 0) {
            LOG_DB_QUERY_ERROR("MySQL stmt bind param error: {}", mysql_stmt_error(stmt));
            return nullptr;
        }
    }

    // 执行语句
    if (mysql_stmt_execute(stmt) != 0) {
        LOG_DB_QUERY_ERROR("MySQL stmt execute error: {}", mysql_stmt_error(stmt));
        return nullptr;
    }

    // 获取结果元数据（列名/类型）并拉取数据行
    {
        MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
        if (!meta) {
            if (mysql_stmt_field_count(stmt) == 0) {
                return nullptr;  // 没有结果集
            }
            LOG_DB_QUERY_ERROR("MySQL stmt result metadata error: {}", mysql_stmt_error(stmt));
            return nullptr;
        }

        // 收集列名
        size_t num_fields = mysql_num_fields(meta);
        std::vector<std::string> colNames;
        MYSQL_FIELD* fields = mysql_fetch_fields(meta);
        for (size_t i = 0; i < num_fields; ++i) {
            colNames.push_back(fields[i].name);
        }

        // 存储结果并获取行数
        if (mysql_stmt_store_result(stmt) != 0) {
            LOG_DB_QUERY_ERROR("MySQL stmt store result error: {}", mysql_stmt_error(stmt));
            mysql_free_result(meta);
            return nullptr;
        }

        // 逐行拉取数据
        std::vector<std::vector<std::string>> rows;

        // 为每列绑定缓冲区
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
            result_binds[i].is_null = (bool*)&result_nulls[i];
        }

        if (mysql_stmt_bind_result(stmt, result_binds.data()) != 0) {
            LOG_DB_QUERY_ERROR("MySQL stmt bind result error: {}", mysql_stmt_error(stmt));
            mysql_free_result(meta);
            return nullptr;
        }

        int fetch_rc;
        // MYSQL_DATA_TRUNCATED(101) = 行可用、部分列超出绑定缓冲 → 循环体内按
        // result_lengths 扩缓冲重取该列。若不接受该返回值，任何一列超缓冲
        // （如 >256 字节的 subject）都会中止整个结果集 → 查询整体失败。
        // 事故：2026-09-05 test3 收件箱 285 字节主题行导致 IMAP UID SEARCH
        // 全体失败（NO Server error），客户端表现即"账号连不上"。
        while ((fetch_rc = mysql_stmt_fetch(stmt)) == 0 || fetch_rc == MYSQL_DATA_TRUNCATED) {
            std::vector<std::string> row_data(num_fields);
            for (size_t i = 0; i < num_fields; ++i) {
                if (result_nulls[i]) {
                    row_data[i] = "";
                } else {
                    // 如果数据被截断，扩展缓冲区重试
                    if (result_lengths[i] > buffers[i].size()) {
                        buffers[i].resize(result_lengths[i]);
                        result_binds[i].buffer = buffers[i].data();
                        result_binds[i].buffer_length = buffers[i].size();
                        mysql_stmt_fetch_column(stmt, &result_binds[i], i, 0);
                    }
                    row_data[i] = std::string(buffers[i].data(), result_lengths[i]);
                }
            }
            rows.push_back(std::move(row_data));
        }

        if (fetch_rc != MYSQL_NO_DATA) {
            // mysql_stmt_error 对部分状态（如 DATA_TRUNCATED）返回空串，
            // 必须带上 rc 数值才有诊断价值（101=DATA_TRUNCATED）
            LOG_DB_QUERY_ERROR("MySQL stmt fetch error: rc={} msg='{}'",
                               fetch_rc, mysql_stmt_error(stmt));
            mysql_free_result(meta);
            return nullptr;
        }

        mysql_free_result(meta);
        return std::make_shared<MySQLResult>(
            std::move(colNames), std::move(rows), rows.size(), num_fields);
    }
}

bool MySQLConnection::execute(const std::string& sql) {
    LOG_DB_QUERY_DEBUG("MySQLConnection::execute() called");
    LOG_DB_QUERY_DEBUG("SQL length: {}", sql.length());
    LOG_DB_QUERY_DEBUG("Acquiring mutex lock...");

    std::unique_lock<std::mutex> lock(m_mutex);

    LOG_DB_QUERY_DEBUG("Mutex lock acquired");

    // 直接检查成员变量，避免在持有锁的情况下调用 is_connected() 导致死锁
    if (!m_connected || !m_mysql) {
        LOG_DB_QUERY_DEBUG("Not connected, calling connect()...");

        // 释放锁以避免在 connect() 中死锁
        lock.unlock();

        if (!connect()) {
            LOG_DB_QUERY_ERROR("Failed to connect");
            return false;
        }

        // 重新获取锁
        lock.lock();
    } else {
        LOG_DB_QUERY_DEBUG("Already connected");
    }

    LOG_DB_QUERY_DEBUG("Executing mysql_query()...");
    if (mysql_query(m_mysql, sql.c_str()) != 0) {
        const auto err = mysql_errno(m_mysql);
        if (err == 1046 && !m_database.empty()) {
            LOG_DB_QUERY_WARN("No database selected, trying USE {} and retry execute", m_database);
            if (mysql_select_db(m_mysql, m_database.c_str()) == 0 &&
                mysql_query(m_mysql, sql.c_str()) == 0) {
                LOG_DB_QUERY_DEBUG("Retry execute succeeded after selecting database {}", m_database);
                return true;
            }
        }
        LOG_DB_QUERY_ERROR("MySQL execute error: {} (errno: {})",
                         mysql_error(m_mysql), mysql_errno(m_mysql));
        return false;
    }

    LOG_DB_QUERY_DEBUG("Query executed successfully");
    return true;
}

bool MySQLConnection::execute(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<ParamType> types(params.size(), ParamType::String);
    return execute(sql, params, types);
}

bool MySQLConnection::execute(const std::string& sql, const std::vector<std::string>& params, const std::vector<ParamType>& types) {
    std::unique_lock<std::mutex> lock(m_mutex);

    // 直接检查成员变量，避免在持有锁的情况下调用 is_connected() 导致死锁
    if (!m_connected || !m_mysql) {
        // 释放锁以避免在 connect() 中死锁
        lock.unlock();

        if (!connect()) {
            return false;
        }

        // 重新获取锁
        lock.lock();
    }

    // 准备语句
    MYSQL_STMT* stmt = mysql_stmt_init(m_mysql);
    if (!stmt) {
        LOG_DB_QUERY_ERROR("MySQL stmt init error: {}", mysql_error(m_mysql));
        return false;
    }

    // 清理资源的RAII包装
    struct StmtGuard {
        MYSQL_STMT* s;
        ~StmtGuard() { if (s) mysql_stmt_close(s); }
    } guard{stmt};

    LOG_DB_QUERY_DEBUG("Preparing statement: {}", sql);
    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()) != 0) {
        LOG_DB_QUERY_ERROR("MySQL stmt prepare error: {}", mysql_stmt_error(stmt));
        return false;
    }
    LOG_DB_QUERY_DEBUG("Statement prepared successfully");

    // 绑定参数 — 变量必须存活到 mysql_stmt_execute 之后
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
        param_copies = params;  // 复制一份，确保执行期间缓冲区有效

        LOG_DB_QUERY_DEBUG("Binding {} parameters:", param_count);
        for (size_t i = 0; i < param_count; ++i) {
            ParamType type = (i < types.size()) ? types[i] : ParamType::String;
            LOG_DB_QUERY_DEBUG("  Param[{}]: type={}, value=[{}], length={}",
                             i, type == ParamType::Int ? "Int" : "String",
                             params[i], params[i].length());

            if (type == ParamType::Int) {
                int_values[i] = std::stoi(params[i]);
                binds[i].buffer_type = MYSQL_TYPE_LONG;
                binds[i].buffer = &int_values[i];
                binds[i].buffer_length = sizeof(int_values[i]);
                binds[i].length = nullptr; // not needed for fixed-size
                LOG_DB_QUERY_DEBUG("    -> bound as int: {}", int_values[i]);
            } else {
                binds[i].buffer_type = MYSQL_TYPE_STRING;
                binds[i].buffer = (void*)param_copies[i].c_str();
                binds[i].buffer_length = param_copies[i].length();
                binds[i].length = &lengths[i];
                lengths[i] = param_copies[i].length();
                LOG_DB_QUERY_DEBUG("    -> bound as string: [{}]", param_copies[i]);
            }
            binds[i].is_null = (bool*)&nulls[i];
        }

        if (mysql_stmt_bind_param(stmt, binds.data()) != 0) {
            LOG_DB_QUERY_ERROR("MySQL stmt bind param error: {}", mysql_stmt_error(stmt));
            return false;
        }
    }

    // 执行语句
    if (mysql_stmt_execute(stmt) != 0) {
        LOG_DB_QUERY_ERROR("MySQL stmt execute error: {}", mysql_stmt_error(stmt));
        return false;
    }

    // 检查受影响的行数
    my_ulonglong affected_rows = mysql_stmt_affected_rows(stmt);
    LOG_DB_QUERY_DEBUG("MySQL stmt affected rows: {}", affected_rows);

    return true;
}

bool MySQLConnection::begin_transaction() {
    return execute("START TRANSACTION");
}

bool MySQLConnection::commit() {
    return execute("COMMIT");
}

bool MySQLConnection::rollback() {
    return execute("ROLLBACK");
}

std::string MySQLConnection::get_last_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_mysql) {
        return mysql_error(m_mysql);
    }
    return "MySQL connection not initialized";
}

std::string MySQLConnection::escape_string(const std::string& str) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_mysql) {
        return str;
    }

    char* escaped = new char[str.length() * 2 + 1];
    mysql_real_escape_string(m_mysql, escaped, str.c_str(), str.length());
    std::string result(escaped);
    delete[] escaped;
    return result;
}

// MySQLService实现

MySQLService::MySQLService() = default;

MySQLService::~MySQLService() = default;

std::shared_ptr<IDBConnection> MySQLService::create_connection(
    const std::string& host,
    const std::string& user,
    const std::string& password,
    const std::string& database,
    unsigned int port
) {
    LOG_DATABASE_DEBUG("MySQLService::create_connection() called.");
    auto connection = std::make_shared<MySQLConnection>();
    connection->set_connection_params(host, user, password, database, port);
    return connection;
}

std::string MySQLService::get_service_name() const {
    return "MySQL";
}

MySQLService& MySQLService::get_instance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_instance) {
        s_instance = std::unique_ptr<MySQLService>(new MySQLService());
    }
    return *s_instance;
}

} // namespace mail_system