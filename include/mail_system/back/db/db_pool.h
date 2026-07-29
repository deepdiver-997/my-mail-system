#ifndef MAIL_SYSTEM_DB_POOL_H
#define MAIL_SYSTEM_DB_POOL_H

#include "mail_system/back/db/db_service.h"
#include "mail_system/back/common/logger.h"
#include <nlohmann/json.hpp> // JSON库
#include <fstream>
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <filesystem>

namespace mail_system {

class ScopedConnection;  // 前向声明

// 数据库连接池抽象类
// 外部调用者只能通过 acquire_connection() 获取 RAII 连接，不可能泄漏
class DBPool {
public:
    virtual ~DBPool() = default;

    // 唯一公开的获取连接方式 —— 返回 RAII 包装，析构自动归还
    ScopedConnection acquire_connection();

    // 连接池运行状况（只读）
    virtual size_t get_pool_size() const = 0;
    virtual size_t get_available_connections() const = 0;
    virtual size_t get_max_pool_size() const = 0;
    virtual size_t get_active_connections() const = 0;
    virtual void close() = 0;

protected:
    DBPool() = default;

    // ---- 以下仅子类 + ScopedConnection(friend) 可访问 ----
    virtual std::shared_ptr<IDBConnection> get_connection() = 0;
    virtual void release_connection(std::shared_ptr<IDBConnection> connection) = 0;
    virtual void initialize_pool() = 0;
    virtual std::shared_ptr<IDBConnection> create_connection() = 0;

    friend class ScopedConnection;
    friend class DistributedMySQLPool;  // 分布式池需跨节点访问子池
};

// RAII 数据库连接 —— 只能由 DBPool::acquire_connection() 创建
// 析构时自动归还连接，彻底杜绝连接泄漏
class ScopedConnection {
public:
    ~ScopedConnection() {
        if (pool_ && connection_) {
            pool_->release_connection(connection_);
        }
    }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;
    ScopedConnection(ScopedConnection&&) = default;
    ScopedConnection& operator=(ScopedConnection&&) = default;

    bool is_valid() const { return connection_ && connection_->is_connected(); }

    // 通用 IDBConnection 接口（query/execute 等）
    IDBConnection* operator->() const { return connection_.get(); }

private:
    friend class DBPool;

    // 仅 DBPool 可构造
    explicit ScopedConnection(DBPool* pool)
        : pool_(pool) {
        if (pool_) {
            connection_ = pool_->get_connection();
        }
    }

    DBPool* pool_ = nullptr;
    std::shared_ptr<IDBConnection> connection_;
};

// DBPool::acquire_connection 必须在 ScopedConnection 完整定义之后
inline ScopedConnection DBPool::acquire_connection() {
    return ScopedConnection(this);
}

// 数据库连接池配置
struct DBPoolConfig {
    struct NodeConfig {
        std::string name;
        std::string host;
        std::string user;
        std::string password;
        std::string database;
        unsigned int port;
        size_t weight;
        bool enabled;

        NodeConfig()
            : port(3306),
              weight(1),
              enabled(true) {}
    };

    std::string achieve;
    std::string host;
    std::string user;
    std::string password;
    std::string database;
    std::string initialize_script;
    unsigned int port;
    size_t initial_pool_size;
    size_t max_pool_size;
    unsigned int connection_timeout;
    unsigned int idle_timeout;
    unsigned int distributed_node_retry_interval;
    std::vector<NodeConfig> nodes;

    DBPoolConfig()
        : port(3306),
          initial_pool_size(5),
          max_pool_size(10),
          connection_timeout(5),
          idle_timeout(60),
          distributed_node_retry_interval(5) {}
    void show() const {
        LOG_DATABASE_INFO("DBPoolConfig: achieve={} host={} user={} db={} port={}"
                          " initial_pool={} max_pool={} conn_timeout={} idle_timeout={}"
                          " distributed_retry={} node_count={}",
                          achieve, host, user, database, port,
                          initial_pool_size, max_pool_size, connection_timeout,
                          idle_timeout, distributed_node_retry_interval, nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            LOG_DATABASE_INFO("  node[{}] name={} host={} port={} db={} weight={} enabled={}",
                              i, node.name, node.host, node.port,
                              node.database, node.weight, node.enabled);
        }
    }

    std::string resolve_path(const std::string& config_path, const std::string& relative_path) {
        if (relative_path.empty()) {
            return "";
        }
        std::filesystem::path config_dir = std::filesystem::path(config_path).parent_path();
        return (config_dir / relative_path).lexically_normal().string();
    }

    // 从JSON对象加载配置
    bool loadFromJson(const std::string& filename) {
        std::ifstream config_file(filename.c_str());
        if (!config_file.is_open()) {
            LOG_DATABASE_ERROR("Failed to open config file: {}", filename);
            return false;
        }
        nlohmann::json json;
        config_file >> json;
        config_file.close();
        if (json.is_discarded()) {
            LOG_DATABASE_ERROR("Failed to parse config file: {}", filename);
            return false;
        }
        achieve = json.value("achieve", achieve);
        host = json.value("host", host);
        user = json.value("user", user);
        password = json.value("password", password);
        database = json.value("database", database);
        initialize_script = resolve_path(filename, json.value("initialize_script", initialize_script));
        port = json.value("port", port);
        initial_pool_size = json.value("initial_pool_size", initial_pool_size);
        max_pool_size = json.value("max_pool_size", max_pool_size);
        connection_timeout = json.value("connection_timeout", connection_timeout);
        idle_timeout = json.value("idle_timeout", idle_timeout);
        distributed_node_retry_interval =
            json.value("distributed_node_retry_interval", distributed_node_retry_interval);

        if (json.contains("nodes") && json["nodes"].is_array()) {
            nodes.clear();
            for (const auto& item : json["nodes"]) {
                if (!item.is_object()) {
                    continue;
                }

                NodeConfig node;
                node.name = item.value("name", std::string());
                node.host = item.value("host", host);
                node.user = item.value("user", user);
                node.password = item.value("password", password);
                node.database = item.value("database", database);
                node.port = item.value("port", port);
                node.weight = item.value("weight", static_cast<size_t>(1));
                node.enabled = item.value("enabled", true);

                if (!node.host.empty() && node.enabled) {
                    nodes.push_back(node);
                }
            }
        }
        return true;
    }
};

// 数据库连接池工厂接口
class DBPoolFactory {
public:
    virtual ~DBPoolFactory() = default;

    // 创建连接池
    virtual std::shared_ptr<DBPool> create_pool(
        const DBPoolConfig& config,
        std::shared_ptr<DBService> db_service
    ) = 0;

protected:
    DBPoolFactory() = default;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_DB_POOL_H