#ifndef MAIL_SYSTEM_MARIADB_SERVICE_H
#define MAIL_SYSTEM_MARIADB_SERVICE_H

#include "mail_system/back/db/db_service.h"
#include <mariadb/mysql.h>   // libmariadb（MariaDB Connector/C）类型与常量
#include <mutex>
#include <string>

namespace mail_system {

// ====================================================================
// MariaDbDriver —— 运行时 dlopen 加载 libmariadb + dlsym 绑定层
// ====================================================================
// 为什么 dlopen 而不是直接链接：libmysqlclient 与 libmariadb 都导出 mysql_*
// 符号，同一二进制同时链接会符号互渗（都解析到先链接的库）。dlopen 用
// RTLD_LOCAL 保持 libmariadb 内部符号局部，两个引擎真共存；MariaDBConnection
// 全部经这里 dlsym 出来的函数指针调用，保证同一连接始终用 libmariadb 的实现。
// 函数指针类型与 <mariadb/mysql.h> 声明逐字一致（ABI 必须精确）。
class MariaDbDriver {
public:
    static MariaDbDriver& instance();

    bool ensure_loaded();                       // 首次调用时 dlopen + dlsym
    bool loaded() const { return loaded_; }
    const std::string& error() const { return error_; }

    // ---- 连接生命周期 ----
    MYSQL* (*mysql_init)(MYSQL*) = nullptr;
    int (*mysql_options)(MYSQL*, enum mysql_option, const void*) = nullptr;
    MYSQL* (*mysql_real_connect)(MYSQL*, const char*, const char*, const char*,
                                 const char*, unsigned int, const char*, unsigned long) = nullptr;
    int (*mysql_set_character_set)(MYSQL*, const char*) = nullptr;
    void (*mysql_close)(MYSQL*) = nullptr;
    int (*mysql_ping)(MYSQL*) = nullptr;

    // ---- 文本协议查询 ----
    int (*mysql_query)(MYSQL*, const char*) = nullptr;
    int (*mysql_select_db)(MYSQL*, const char*) = nullptr;
    MYSQL_RES* (*mysql_store_result)(MYSQL*) = nullptr;
    unsigned int (*mysql_field_count)(MYSQL*) = nullptr;

    // ---- 结果读取 ----
    unsigned int (*mysql_num_fields)(MYSQL_RES*) = nullptr;
    MYSQL_FIELD* (*mysql_fetch_fields)(MYSQL_RES*) = nullptr;
    MYSQL_ROW (*mysql_fetch_row)(MYSQL_RES*) = nullptr;
    unsigned long* (*mysql_fetch_lengths)(MYSQL_RES*) = nullptr;
    unsigned long long (*mysql_num_rows)(MYSQL_RES*) = nullptr;
    void (*mysql_free_result)(MYSQL_RES*) = nullptr;

    // ---- 错误 ----
    unsigned int (*mysql_errno)(MYSQL*) = nullptr;
    const char* (*mysql_error)(MYSQL*) = nullptr;

    // ---- prepared statement ----
    MYSQL_STMT* (*mysql_stmt_init)(MYSQL*) = nullptr;
    int (*mysql_stmt_prepare)(MYSQL_STMT*, const char*, unsigned long) = nullptr;
    my_bool (*mysql_stmt_bind_param)(MYSQL_STMT*, MYSQL_BIND*) = nullptr;
    int (*mysql_stmt_execute)(MYSQL_STMT*) = nullptr;
    MYSQL_RES* (*mysql_stmt_result_metadata)(MYSQL_STMT*) = nullptr;
    unsigned int (*mysql_stmt_field_count)(MYSQL_STMT*) = nullptr;
    int (*mysql_stmt_store_result)(MYSQL_STMT*) = nullptr;
    my_bool (*mysql_stmt_bind_result)(MYSQL_STMT*, MYSQL_BIND*) = nullptr;
    int (*mysql_stmt_fetch)(MYSQL_STMT*) = nullptr;
    int (*mysql_stmt_fetch_column)(MYSQL_STMT*, MYSQL_BIND*, unsigned int, unsigned long) = nullptr;
    my_bool (*mysql_stmt_close)(MYSQL_STMT*) = nullptr;
    const char* (*mysql_stmt_error)(MYSQL_STMT*) = nullptr;
    unsigned long long (*mysql_stmt_affected_rows)(MYSQL_STMT*) = nullptr;

    // ---- 工具 ----
    unsigned long (*mysql_real_escape_string)(MYSQL*, char*, const char*, unsigned long) = nullptr;

    // ---- 阶段 2 预留：非阻塞 API（可选符号，缺则不启用 async） ----
    int (*mysql_stmt_execute_start)(int*, MYSQL_STMT*) = nullptr;
    int (*mysql_stmt_execute_cont)(int*, MYSQL_STMT*, int) = nullptr;
    my_socket (*mysql_get_socket)(MYSQL*) = nullptr;

private:
    MariaDbDriver() = default;
    void* handle_ = nullptr;
    bool loaded_ = false;
    std::string error_;

    template <typename T>
    bool sym(T* out, const char* name);          // 必需符号，失败即加载失败
    template <typename T>
    bool sym_optional(T* out, const char* name); // 可选符号，失败仅告警
};

// ====================================================================
// MariaDbResult —— 查询结果（镜像 MySQLResult，基于 libmariadb 的 MYSQL_RES）
// ====================================================================
class MariaDbResult : public IDBResult {
public:
    explicit MariaDbResult(MYSQL_RES* result);
    MariaDbResult(std::vector<std::string> colNames,
                  std::vector<std::vector<std::string>> rows,
                  size_t rowCount, size_t colCount);
    ~MariaDbResult() override;

    size_t get_row_count() const override;
    size_t get_column_count() const override;
    std::vector<std::string> get_column_names() const override;
    std::map<std::string, std::string> get_row(size_t row_index) const override;
    std::vector<std::map<std::string, std::string>> get_all_rows() const override;
    std::string get_value(size_t row_index, const std::string& column_name) const override;

private:
    MYSQL_RES* m_result;
    std::vector<std::string> m_columnNames;
    std::vector<std::vector<std::string>> m_rows;
    size_t m_rowCount;
    size_t m_columnCount;

    void load_result_data();
};

// ====================================================================
// MariaDBConnection —— IDBConnection 实现（同步路径，镜像 MySQLConnection）
// ====================================================================
class MariaDBConnection : public IDBConnection {
public:
    enum class ParamType { String, Int };
    MariaDBConnection();
    ~MariaDBConnection() override;

    void set_connection_params(const std::string& host, const std::string& user,
                               const std::string& password, const std::string& database,
                               unsigned int port);

    bool connect() override;
    void disconnect() override;
    Dialect dialect() const override { return Dialect::MySQL; }
    bool is_connected() const override;
    std::shared_ptr<IDBResult> query(const std::string& sql) override;
    std::shared_ptr<IDBResult> query(const std::string& sql,
                                     const std::vector<std::string>& params) override;
    bool execute(const std::string& sql) override;
    bool execute(const std::string& sql, const std::vector<std::string>& params) override;
    bool execute(const std::string& sql, const std::vector<std::string>& params,
                 const std::vector<ParamType>& types);
    bool begin_transaction() override;
    bool commit() override;
    bool rollback() override;
    std::string get_last_error() const override;
    std::string escape_string(const std::string& str) const override;

private:
    MYSQL* m_mysql;
    std::string m_host;
    std::string m_user;
    std::string m_password;
    std::string m_database;
    unsigned int m_port;
    bool m_connected;
    mutable std::mutex m_mutex;

    bool ensure_driver();   // 加载 libmariadb，失败记日志
    void init_mysql();
};

// ====================================================================
// MariaDBService —— DBService 工厂
// ====================================================================
class MariaDBService : public DBService {
public:
    MariaDBService() = default;
    ~MariaDBService() override = default;

    std::shared_ptr<IDBConnection> create_connection(
        const std::string& host, const std::string& user, const std::string& password,
        const std::string& database, unsigned int port) override;

    std::string get_service_name() const override { return "mariadb"; }
};

} // namespace mail_system

#endif // MAIL_SYSTEM_MARIADB_SERVICE_H
