#ifndef MOCK_DB_POOL_H
#define MOCK_DB_POOL_H

#include "mail_system/back/db/db_pool.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace mail_system {
namespace test {

// ================================================================
// MockDbResult — 程序化查询结果
// ================================================================
class MockDbResult : public pr::IDBResult {
public:
    explicit MockDbResult(std::vector<std::map<std::string, std::string>> rows = {})
        : rows_(std::move(rows)) {}

    size_t get_row_count() const override { return rows_.size(); }
    size_t get_column_count() const override {
        size_t n = 0;
        for (const auto& r : rows_) n = std::max(n, r.size());
        return n;
    }
    std::vector<std::string> get_column_names() const override {
        std::vector<std::string> cols;
        if (!rows_.empty())
            for (const auto& [k, v] : rows_[0]) { (void)v; cols.push_back(k); }
        return cols;
    }
    std::map<std::string, std::string> get_row(size_t row_index) const override {
        return row_index < rows_.size() ? rows_[row_index] : std::map<std::string, std::string>{};
    }
    std::vector<std::map<std::string, std::string>> get_all_rows() const override { return rows_; }
    std::string get_value(size_t row_index, const std::string& column_name) const override {
        if (row_index >= rows_.size()) return {};
        auto it = rows_[row_index].find(column_name);
        return it != rows_[row_index].end() ? it->second : std::string{};
    }

private:
    std::vector<std::map<std::string, std::string>> rows_;
};

// ================================================================
// MockDbConnection — 支持延迟回调的数据库连接
//
//   默认 Deferred 模式：async_query/async_execute 保存回调，由测试线程
//   fire_query()/fire_execute() 手动触发，模拟 DB worker 线程延迟。
//   可切换同步模式（set_deferred(false)）恢复原行为。
// ================================================================
class MockDbConnection : public pr::IDBConnection {
public:
    MockDbConnection() { connected_ = true; }

    std::queue<std::shared_ptr<pr::IDBResult>> sync_results_;
    mutable std::mutex q_mu_;

    void set_deferred(bool v) {
        std::lock_guard<std::mutex> lk(mu_);
        deferred_ = v;
    }

    // 清空 pending 回调与最近参数（测试隔离：排除持久化 worker 等其它查询来源）
    void clear_pending() {
        std::lock_guard<std::mutex> lk(mu_);
        pending_query_ = nullptr;
        pending_execute_ = nullptr;
        last_sql_.clear();
        last_params_.clear();
    }

    // ---- 查询 pending / 手动触发 ----
    bool has_pending_query() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_query_ != nullptr;
    }
    bool has_pending_execute() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_execute_ != nullptr;
    }
    std::vector<std::string> last_query_params() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_params_;
    }
    std::string last_query_sql() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_sql_;
    }

    // 注意：query() 会在 async_query_impl 持有 mu_ 的情况下被调用，
    // 因此结果队列必须用独立的锁，否则非递归互斥量重锁直接死锁。
    std::shared_ptr<pr::IDBResult> pop_sync_result() {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (sync_results_.empty()) return std::make_shared<MockDbResult>();
        auto r = std::move(sync_results_.front());
        sync_results_.pop();
        return r;
    }

    void fire_query(std::shared_ptr<pr::IDBResult> result) {
        pr::QueryCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = std::move(pending_query_);
            pending_query_ = nullptr;
        }
        if (cb) cb(std::move(result));
    }
    void fire_execute(bool success) {
        pr::ExecuteCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = std::move(pending_execute_);
            pending_execute_ = nullptr;
        }
        if (cb) cb(success);
    }

    // ---- IDBConnection impl ----
    pr::IDBConnection::Dialect dialect() const override {
        return pr::IDBConnection::Dialect::Null;
    }
    bool connect() override { connected_ = true; return true; }
    void disconnect() override { connected_ = false; }
    bool is_connected() const override { return connected_; }

    std::shared_ptr<pr::IDBResult> query(const std::string& sql) override {
        (void)sql;
        return pop_sync_result();
    }
    std::shared_ptr<pr::IDBResult> query(const std::string& sql,
                                         const std::vector<std::string>& params) override {
        (void)sql; (void)params;
        return pop_sync_result();
    }

    // 同步模式下的可编程结果队列：query() 逐个弹出（空则空结果）。
    // 供同步 DB 路径（如 IMAP get_mailbox_mails 的 sq 桥）注入行数据。
    void push_sync_result(std::shared_ptr<pr::IDBResult> r) {
        std::lock_guard<std::mutex> lk(q_mu_);
        sync_results_.push(std::move(r));
    }
    bool execute(const std::string& sql) override { (void)sql; return true; }
    bool execute(const std::string& sql,
                 const std::vector<std::string>& params) override {
        (void)sql; (void)params; return true;
    }
    bool begin_transaction() override { return true; }
    bool commit() override { return true; }
    bool rollback() override { return true; }
    std::string get_last_error() const override { return {}; }
    std::string escape_string(const std::string& str) const override { return str; }

    void async_query(const std::string& sql, pr::QueryCallback cb) override {
        async_query_impl(sql, {}, std::move(cb));
    }
    void async_query(const std::string& sql,
                     const std::vector<std::string>& params, pr::QueryCallback cb) override {
        async_query_impl(sql, params, std::move(cb));
    }
    void async_execute(const std::string& sql, pr::ExecuteCallback cb) override {
        async_execute_impl(sql, {}, std::move(cb));
    }
    void async_execute(const std::string& sql,
                       const std::vector<std::string>& params, pr::ExecuteCallback cb) override {
        async_execute_impl(sql, params, std::move(cb));
    }
    void async_begin_transaction(pr::ExecuteCallback cb) override { if (cb) cb(true); }
    void async_commit(pr::ExecuteCallback cb) override { if (cb) cb(true); }
    void async_rollback(pr::ExecuteCallback cb) override { if (cb) cb(true); }

private:
    void async_query_impl(const std::string& sql, const std::vector<std::string>& params,
                          pr::QueryCallback cb) {
        bool sync_call = false;
        std::shared_ptr<pr::IDBResult> result;
        {
            std::lock_guard<std::mutex> lk(mu_);
            last_sql_ = sql;
            last_params_ = params;
            if (deferred_) {
                pending_query_ = std::move(cb);
                return;
            }
            sync_call = true;
            result = query(sql, params);
        }
        // 同步模式：释放锁后再回调，避免回调链重入 async_query 造成非递归锁死锁
        if (sync_call) cb(std::move(result));
    }
    void async_execute_impl(const std::string& sql, const std::vector<std::string>& params,
                            pr::ExecuteCallback cb) {
        bool sync_call = false;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            last_sql_ = sql;
            last_params_ = params;
            if (deferred_) {
                pending_execute_ = std::move(cb);
                return;
            }
            sync_call = true;
            ok = execute(sql, params);
        }
        if (sync_call) cb(ok);
    }

    mutable std::mutex mu_;
    bool connected_ = true;
    bool deferred_ = true;
    std::string last_sql_;
    std::vector<std::string> last_params_;
    pr::QueryCallback pending_query_;
    pr::ExecuteCallback pending_execute_;
};

// ================================================================
// MockDbPool — 返回 MockDbConnection 的连接池
// ================================================================
class MockDbPool : public pr::DBPool {
public:
    std::shared_ptr<MockDbConnection> mock_conn() const { return conn_; }

    size_t get_pool_size() const override { return 1; }
    size_t get_available_connections() const override { return 1; }
    size_t get_max_pool_size() const override { return 1; }
    size_t get_active_connections() const override { return 0; }
    void close() override {}
    std::shared_ptr<pr::IDBConnection> get_connection() override { return conn_; }
    void release_connection(std::shared_ptr<pr::IDBConnection>) override {}

protected:
    void initialize_pool() override {}
    std::shared_ptr<pr::IDBConnection> create_connection() override { return conn_; }

private:
    std::shared_ptr<MockDbConnection> conn_ = std::make_shared<MockDbConnection>();
};

} // namespace test
} // namespace mail_system
#endif
