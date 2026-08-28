#ifndef PR_FRAMEWORK_DB_DB_POOL_H
#define PR_FRAMEWORK_DB_DB_POOL_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace pr {

// ---- 异步回调类型 ----
using QueryCallback   = std::function<void(std::shared_ptr<class IDBResult>)>;
using ExecuteCallback = std::function<void(bool success)>;

// ---- IDBResult: 查询结果接口 ----
class IDBResult {
public:
    virtual ~IDBResult() = default;
    virtual size_t get_row_count() const = 0;
    virtual size_t get_column_count() const = 0;
    virtual std::vector<std::string> get_column_names() const = 0;
    virtual std::map<std::string, std::string> get_row(size_t row_index) const = 0;
    virtual std::vector<std::map<std::string, std::string>> get_all_rows() const = 0;
    virtual std::string get_value(size_t row_index, const std::string& column_name) const = 0;
};

// ---- IDBConnection: 数据库连接接口 ----
class IDBConnection {
public:
    enum class Dialect { MySQL, Null };

    virtual ~IDBConnection() = default;
    virtual Dialect dialect() const = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual std::shared_ptr<IDBResult> query(const std::string& sql) = 0;
    virtual std::shared_ptr<IDBResult> query(const std::string& sql,
                                               const std::vector<std::string>& params) = 0;
    virtual bool execute(const std::string& sql) = 0;
    virtual bool execute(const std::string& sql,
                         const std::vector<std::string>& params) = 0;
    virtual bool begin_transaction() = 0;
    virtual bool commit() = 0;
    virtual bool rollback() = 0;
    virtual std::string get_last_error() const = 0;
    virtual std::string escape_string(const std::string& str) const = 0;

    // 连接保活/有效性校验（池 checkout 时调用，替代裸 SELECT 1）。
    // 默认 SELECT 1（与旧行为一致）；MariaDBConnection 覆写为 mysql_ping
    // （COM_PING，不跑查询、不产生结果集、不碰 prepared stmt 状态——是缓存 stmt
    // 前提下的必要选择）。
    virtual bool ping() {
        return query("SELECT 1") != nullptr;
    }

    // Async wrappers with default sync impl
    virtual void async_query(const std::string& sql, QueryCallback cb) {
        if (cb) cb(query(sql));
    }
    virtual void async_query(const std::string& sql,
                             const std::vector<std::string>& params, QueryCallback cb) {
        if (cb) cb(query(sql, params));
    }
    virtual void async_execute(const std::string& sql, ExecuteCallback cb) {
        if (cb) cb(execute(sql));
    }
    virtual void async_execute(const std::string& sql,
                               const std::vector<std::string>& params, ExecuteCallback cb) {
        if (cb) cb(execute(sql, params));
    }
    virtual void async_begin_transaction(ExecuteCallback cb) {
        if (cb) cb(begin_transaction());
    }
    virtual void async_commit(ExecuteCallback cb) {
        if (cb) cb(commit());
    }
    virtual void async_rollback(ExecuteCallback cb) {
        if (cb) cb(rollback());
    }
};

class ScopedConnection;  // fwd

// ---- DBPool: 数据库连接池抽象接口 ----
class DBPool {
public:
    virtual ~DBPool() = default;

    ScopedConnection acquire_connection();

    virtual size_t get_pool_size() const = 0;
    virtual size_t get_available_connections() const = 0;
    virtual size_t get_max_pool_size() const = 0;
    virtual size_t get_active_connections() const = 0;
    virtual void close() = 0;

    // Subclass/composite pool access. Prefer acquire_connection() for RAII.
    virtual std::shared_ptr<IDBConnection> get_connection() = 0;
    virtual void release_connection(std::shared_ptr<IDBConnection> connection) = 0;

protected:
    DBPool() = default;
    virtual void initialize_pool() = 0;
    virtual std::shared_ptr<IDBConnection> create_connection() = 0;

    friend class ScopedConnection;
};

// ---- ScopedConnection: RAII 数据库连接 ----
class ScopedConnection {
public:
    ~ScopedConnection() {
        if (pool_ && connection_) pool_->release_connection(connection_);
    }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;
    ScopedConnection(ScopedConnection&&) = default;
    ScopedConnection& operator=(ScopedConnection&&) = default;

    bool is_valid() const { return connection_ && connection_->is_connected(); }
    IDBConnection* operator->() const { return connection_.get(); }

    // 无池/取池失败的空连接：is_valid() 为 false，调用方按连接失败处理
    static ScopedConnection invalid() { return ScopedConnection(nullptr); }

private:
    friend class DBPool;
    explicit ScopedConnection(DBPool* pool) : pool_(pool) {
        if (pool_) connection_ = pool_->get_connection();
    }
    DBPool* pool_ = nullptr;
    std::shared_ptr<IDBConnection> connection_;
};

inline ScopedConnection DBPool::acquire_connection() {
    return ScopedConnection(this);
}

// ---- DBService: 数据库服务抽象工厂 ----
class DBService {
public:
    virtual ~DBService() = default;
    virtual std::shared_ptr<IDBConnection> create_connection(
        const std::string& host, const std::string& user,
        const std::string& password, const std::string& database,
        unsigned int port) = 0;
    virtual std::string get_service_name() const = 0;
protected:
    DBService() = default;
};

} // namespace pr

#endif // PR_FRAMEWORK_DB_DB_POOL_H
