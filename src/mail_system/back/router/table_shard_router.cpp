#include "mail_system/back/router/table_shard_router.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/common/logger.h"
#include <algorithm>
#include <cctype>

namespace mail_system {
namespace router {

TableShardRouter::TableShardRouter(
    std::shared_ptr<DBPool> route_db_pool,
    std::string table_name,
    std::string email_column,
    std::string shard_column,
    size_t shard_count,
    size_t cache_capacity,
    std::vector<std::shared_ptr<DBPool>> db_pools,
    std::vector<std::shared_ptr<storage::IStorageProvider>> storages)
    : m_route_db_pool(std::move(route_db_pool))
    , m_table_name(std::move(table_name))
    , m_email_column(std::move(email_column))
    , m_shard_column(std::move(shard_column))
    , m_shard_count(shard_count > 0 ? shard_count : 1)
    , m_cache(cache_capacity > 0 ? cache_capacity : 100000,
              std::chrono::seconds(0))  // TTL=0: 映射不可变，永不过期
    , m_db_pools(std::move(db_pools))
    , m_storages(std::move(storages))
{
    LOG_SERVER_INFO("TableShardRouter created: table={} cache_capacity={} shards={}",
                    m_table_name, cache_capacity, m_shard_count);
}

int TableShardRouter::route(const std::string& email) {
    if (email.empty()) return -1;

    std::string key = to_lower(email);

    // 先查缓存
    bool stale;
    int shard;
    if (m_cache.get(key, shard, stale)) {
        return shard;  // TTL=0 意味着永远不会 stale
    }

    // 缓存未命中，查数据库
    if (!m_route_db_pool) {
        LOG_SERVER_ERROR("TableShardRouter: route_db_pool is null");
        return -1;
    }

    auto conn = m_route_db_pool->acquire_connection();
    if (!conn->is_valid()) {
        LOG_SERVER_ERROR("TableShardRouter: failed to acquire connection for route lookup");
        return -1;
    }

    std::string sql = db::sql::build_shard_lookup(m_table_name, m_email_column, m_shard_column,
                                                    (*conn)->escape_string(key));

    auto result = (*conn)->query(sql);
    if (result && result->get_row_count() > 0) {
        try {
            shard = std::stoi(result->get_value(0, m_shard_column));
            if (shard >= 0 && static_cast<size_t>(shard) < m_shard_count) {
                m_cache.put(key, shard);
                return shard;
            }
        } catch (const std::exception& e) {
            LOG_SERVER_ERROR("TableShardRouter: failed to parse shard_id for {}: {}", key, e.what());
        }
    }

    return -1;
}

std::shared_ptr<DBPool> TableShardRouter::get_db_pool(size_t shard) {
    if (shard >= m_db_pools.size()) return nullptr;
    return m_db_pools[shard];
}

std::shared_ptr<storage::IStorageProvider> TableShardRouter::get_storage(size_t shard) {
    if (shard >= m_storages.size()) return nullptr;
    return m_storages[shard];
}

std::string TableShardRouter::to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

} // namespace router
} // namespace mail_system
