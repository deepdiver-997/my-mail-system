#ifndef MAIL_SYSTEM_TABLE_SHARD_ROUTER_H
#define MAIL_SYSTEM_TABLE_SHARD_ROUTER_H

#include "mail_system/back/router/i_shard_router.h"
#include "mail_system/back/common/lru_cache.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace mail_system {

namespace router {

// ====================================================================
// 数据库表分片路由器
//
// 用 DBPool 查询 user_shards 表获取 email -> shard_id 的映射。
// 内部使用 LruCache<string,int> 缓存，TTL=0（映射不可变，永不过期）。
//
// "路由 DB" 用于查映射表，各 shard 的 DB 用于业务查询。
// 路由 DB 可以和 shard0 的 DB 是同一个实例。
// ====================================================================
class TableShardRouter : public IShardRouter {
public:
    TableShardRouter(std::shared_ptr<DBPool> route_db_pool,
                     std::string table_name,
                     std::string email_column,
                     std::string shard_column,
                     size_t shard_count,
                     size_t cache_capacity,
                     std::vector<std::shared_ptr<DBPool>> db_pools,
                     std::vector<std::shared_ptr<storage::IStorageProvider>> storages);

    int route(const std::string& email) override;
    size_t shard_count() const override { return m_shard_count; }
    const char* name() const override { return "table"; }

    std::shared_ptr<DBPool> get_db_pool(size_t shard) override;
    std::shared_ptr<storage::IStorageProvider> get_storage(size_t shard) override;

private:
    static std::string to_lower(const std::string& s);

    std::shared_ptr<DBPool> m_route_db_pool;
    std::string m_table_name;
    std::string m_email_column;
    std::string m_shard_column;
    size_t m_shard_count;

    // email -> shard_id 缓存（映射不可变，TTL=0 永不过期）
    mutable LruCache<std::string, int> m_cache;

    std::vector<std::shared_ptr<DBPool>> m_db_pools;
    std::vector<std::shared_ptr<storage::IStorageProvider>> m_storages;
};

} // namespace router
} // namespace mail_system

#endif
