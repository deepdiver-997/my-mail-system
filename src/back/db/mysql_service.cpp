#include "mail_system/back/db/mysql_service.h"
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
    }
}

MySQLResult::~MySQLResult() {
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
    init_mysql();
}

MySQLConnection::~MySQLConnection() {
    disconnect();
}

void MySQLConnection::init_mysql() {
    m_mysql = mysql_init(nullptr);
    if (!m_mysql) {
        throw std::runtime_error("Failed to initialize MySQL connection");
    }

    // 设置自动重连
    bool reconnect = true;
    mysql_options(m_mysql, MYSQL_OPT_RECONNECT, &reconnect);

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
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_connected) {
        return true;
    }

    if (!m_mysql) {
        init_mysql();
    }

    if (mysql_real_connect(
            m_mysql,
            m_host.c_str(),
            m_user.c_str(),
            m_password.c_str(),
            m_database.c_str(),
            m_port,
            nullptr,
            0
        ) == nullptr) {
        std::cerr << "MySQL connection error: " << mysql_error(m_mysql) << std::endl;
        return false;
    }

    m_connected = true;
    return true;
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
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!is_connected()) {
        if (!connect()) {
            return nullptr;
        }
    }

    if (mysql_query(m_mysql, sql.c_str()) != 0) {
        std::cerr << "MySQL query error: " << mysql_error(m_mysql) << std::endl;
        return nullptr;
    }

    MYSQL_RES* result = mysql_store_result(m_mysql);
    if (!result) {
        if (mysql_field_count(m_mysql) == 0) {
            // 没有结果集的查询（如INSERT, UPDATE, DELETE）
            return nullptr;
        } else {
            // 查询出错
            std::cerr << "MySQL store result error: " << mysql_error(m_mysql) << std::endl;
            return nullptr;
        }
    }

    return std::make_shared<MySQLResult>(result);
}

bool MySQLConnection::execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!is_connected()) {
        if (!connect()) {
            return false;
        }
    }

    if (mysql_query(m_mysql, sql.c_str()) != 0) {
        std::cerr << "MySQL execute error: " << mysql_error(m_mysql) << std::endl;
        return false;
    }

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