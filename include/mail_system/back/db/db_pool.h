#ifndef MAIL_SYSTEM_DB_POOL_H
#define MAIL_SYSTEM_DB_POOL_H

#include "framework/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/common/logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <memory>
#include <vector>

namespace mail_system {

// 向后兼容
using pr::DBPool;
using pr::ScopedConnection;
using pr::IDBConnection;
using pr::IDBResult;
using pr::QueryCallback;
using pr::ExecuteCallback;

// ================================================================
// DBPoolConfig — 连接池配置 (应用层, 含 JSON 加载 + LOG_* 日志)
// ================================================================
struct DBPoolConfig {
    struct NodeConfig {
        std::string name, host, user, password, database;
        unsigned int port = 3306;
        size_t weight = 1;
        bool enabled = true;
    };

    std::string achieve, host, user, password, database, initialize_script;
    unsigned int port = 3306;
    size_t initial_pool_size = 5, max_pool_size = 10;
    unsigned int connection_timeout = 5, idle_timeout = 60;
    unsigned int distributed_node_retry_interval = 5;
    std::vector<NodeConfig> nodes;

    void show() const {
        LOG_DATABASE_INFO("DBPoolConfig: achieve={} host={} user={} db={} port={}"
                          " initial_pool={} max_pool={} conn_timeout={} idle_timeout={}"
                          " distributed_retry={} node_count={}",
                          achieve, host, user, database, port,
                          initial_pool_size, max_pool_size, connection_timeout,
                          idle_timeout, distributed_node_retry_interval, nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
            LOG_DATABASE_INFO("  node[{}] name={} host={} port={} db={} weight={} enabled={}",
                              i, node.name, node.host, node.port,
                              node.database, node.weight, node.enabled);
        }
    }

    bool loadFromJson(const std::string& filename) {
        std::ifstream f(filename);
        if (!f.is_open()) { LOG_DATABASE_ERROR("Failed to open config file: {}", filename); return false; }
        nlohmann::json j; f >> j;
        if (j.is_discarded()) { LOG_DATABASE_ERROR("Failed to parse config file: {}", filename); return false; }

        achieve          = j.value("achieve", achieve);
        host             = j.value("host", host);
        user             = j.value("user", user);
        password         = j.value("password", password);
        database         = j.value("database", database);
        port             = j.value("port", port);
        initial_pool_size= j.value("initial_pool_size", initial_pool_size);
        max_pool_size    = j.value("max_pool_size", max_pool_size);
        connection_timeout=j.value("connection_timeout", connection_timeout);
        idle_timeout     = j.value("idle_timeout", idle_timeout);
        distributed_node_retry_interval = j.value("distributed_node_retry_interval", distributed_node_retry_interval);

        auto resolve = [&](const std::string& rel) {
            if (rel.empty()) return std::string();
            return (std::filesystem::path(filename).parent_path() / rel).lexically_normal().string();
        };
        initialize_script = resolve(j.value("initialize_script", initialize_script));

        if (j.contains("nodes") && j["nodes"].is_array()) {
            nodes.clear();
            for (auto& item : j["nodes"]) {
                if (!item.is_object()) continue;
                NodeConfig n;
                n.name     = item.value("name", std::string());
                n.host     = item.value("host", host);
                n.user     = item.value("user", user);
                n.password = item.value("password", password);
                n.database = item.value("database", database);
                n.port     = item.value("port", port);
                n.weight   = item.value("weight", size_t(1));
                n.enabled  = item.value("enabled", true);
                if (!n.host.empty() && n.enabled) nodes.push_back(n);
            }
        }
        return true;
    }
};

// ================================================================
// DBPoolFactory — 连接池工厂 (应用层, 具体实现由 app 提供)
// ================================================================
class DBPoolFactory {
public:
    virtual ~DBPoolFactory() = default;
    virtual std::shared_ptr<DBPool> create_pool(
        const DBPoolConfig& config, std::shared_ptr<DBService> db_service) = 0;
protected:
    DBPoolFactory() = default;
};

} // namespace mail_system
#endif // MAIL_SYSTEM_DB_POOL_H
