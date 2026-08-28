#ifndef MAIL_SYSTEM_MARIADB_SERVICE_H
#define MAIL_SYSTEM_MARIADB_SERVICE_H

#include "mail_system/back/db/db_service.h"
#include <mariadb/mysql.h>   // libmariadb（MariaDB Connector/C）类型与常量
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

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
    my_bool (*mysql_stmt_free_result)(MYSQL_STMT*) = nullptr;
    const char* (*mysql_stmt_error)(MYSQL_STMT*) = nullptr;
    unsigned long long (*mysql_stmt_affected_rows)(MYSQL_STMT*) = nullptr;

    // ---- 工具 ----
    unsigned long (*mysql_real_escape_string)(MYSQL*, char*, const char*, unsigned long) = nullptr;

    // ---- 非阻塞 API（阶段 2：非阻塞状态机 + io_context async_wait 集成） ----
    // libmariadb 对每个可能阻塞的调用提供 *_start / *_cont 对：
    //   *_start 首次发起；返回 0 = 已完成（*ret 存阻塞版返回值），
    //   非 0 = MYSQL_WAIT_READ/WRITE/EXCEPT/TIMEOUT 位掩码（等待 socket）。
    //   *_cont(stmt, 上次返回的 wait 掩码) 在 socket 就绪后续；返回语义同 *_start。
    // 全部可选符号：缺则 has_nonblocking()=false，async_* 回退同步执行。
    int (*mysql_stmt_prepare_start)(int*, MYSQL_STMT*, const char*, unsigned long) = nullptr;
    int (*mysql_stmt_prepare_cont)(int*, MYSQL_STMT*, int) = nullptr;
    int (*mysql_stmt_execute_start)(int*, MYSQL_STMT*) = nullptr;
    int (*mysql_stmt_execute_cont)(int*, MYSQL_STMT*, int) = nullptr;
    int (*mysql_stmt_store_result_start)(int*, MYSQL_STMT*) = nullptr;
    int (*mysql_stmt_store_result_cont)(int*, MYSQL_STMT*, int) = nullptr;
    my_socket (*mysql_get_socket)(MYSQL*) = nullptr;
    unsigned int (*mysql_get_timeout_value_ms)(const MYSQL*) = nullptr;

    // 非阻塞路径是否可用（全部必需符号已绑定）
    bool has_nonblocking() const {
        return mysql_stmt_prepare_start && mysql_stmt_prepare_cont &&
               mysql_stmt_execute_start && mysql_stmt_execute_cont &&
               mysql_stmt_store_result_start && mysql_stmt_store_result_cont &&
               mysql_get_socket;
    }

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
// MariaDBConnection —— IDBConnection 实现（同步 + 非阻塞 async 双路径）
// ====================================================================
// enable_shared_from_this：非阻塞 async 在 io_context.async_wait 挂起期间持有
// shared_ptr 保活连接（池里释放/销毁也不会悬垂，续作回调安全）。连接始终由池的
// shared_ptr 拥有（MariaDBService::create_connection 用 make_shared）。
class MariaDBConnection : public IDBConnection,
                          public std::enable_shared_from_this<MariaDBConnection> {
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

    // ---- 非阻塞 async（阶段 2 override） ----
    // libmariadb 非阻塞路径可用时走真异步状态机（io 线程 async_wait 不阻塞，
    // worker 线程阻塞 poll）；不可用（缺符号）回退同步执行后内联回调（旧行为）。
    void async_query(const std::string& sql, QueryCallback cb) override;
    void async_query(const std::string& sql, const std::vector<std::string>& params,
                     QueryCallback cb) override;
    void async_execute(const std::string& sql, ExecuteCallback cb) override;
    void async_execute(const std::string& sql, const std::vector<std::string>& params,
                       ExecuteCallback cb) override;
    void async_begin_transaction(ExecuteCallback cb) override;
    void async_commit(ExecuteCallback cb) override;
    void async_rollback(ExecuteCallback cb) override;

    // 保活：mysql_ping（COM_PING），不跑查询、不碰 prepared stmt 状态。
    // 池 checkout 校验走它（替代 SELECT 1）——缓存 stmt 后 SELECT 1 不再干净。
    bool ping() override;

private:
    MYSQL* m_mysql;
    std::string m_host;
    std::string m_user;
    std::string m_password;
    std::string m_database;
    unsigned int m_port;
    bool m_connected;
    mutable std::mutex m_mutex;

    // 非阻塞 in-flight 守卫：连接被 FSM 链独占（ScopedConnection 持链），单飞行是
    // 结构性保证；此标志只做防御性检测（误用即记错）。
    std::atomic<bool> m_asyncInFlight{false};

    // ---- prepared stmt 缓存（Phase 3：省 prepare 往返，一趟变一趟） ----
    // 每连接 SQL→MYSQL_STMT*。首次 prepare 一次，后续 bind + execute 复用。
    // 复用靠每次用后 mysql_stmt_free_result（结果已全量消费，无需 mysql_stmt_reset
    // 那趟往返）。stmts 绑定连接，断连/失效时整体清空重建。
    // 访问约定：以下三个 helper 只在持 m_mutex 时调用（async step / disconnect 都持锁）。
    std::unordered_map<std::string, MYSQL_STMT*> m_stmtCache;
    static constexpr size_t kMaxCachedStmts = 128;   // 批量 IN 变长 SQL 会撑大缓存，超限整体清空

    MYSQL_STMT* lookup_stmt(const std::string& sql);     // 缓存命中（持锁调用）
    void store_stmt(const std::string& sql, MYSQL_STMT* stmt);  // 入缓存（持锁调用，超限清空）
    void clear_stmt_cache();                             // 关掉全部缓存 stmt（持锁调用）

    bool ensure_driver();   // 加载 libmariadb，失败记日志
    void init_mysql();

    // 非阻塞状态机：op 结构体 + 驱动逻辑定义在 .cpp（不暴露到头文件）。
    struct AsyncStmtOp;
    void start_async_op(const std::shared_ptr<AsyncStmtOp>& op);
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
