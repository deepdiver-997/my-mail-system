#ifndef MAIL_SYSTEM_I_SHARD_ROUTER_H
#define MAIL_SYSTEM_I_SHARD_ROUTER_H

#include "mail_system/back/db/db_pool.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mail_system {

namespace storage {
class IStorageProvider;
}

namespace router {

// ====================================================================
// 分片路由抽象接口
//
// 将用户邮箱映射到 shard 索引，并提供对应 shard 的 DBPool 和
// IStorageProvider 访问。实现必须是线程安全的。
// ====================================================================
class IShardRouter {
public:
    virtual ~IShardRouter() = default;

    // 将 recipient email 路由到 shard 索引。
    // 返回 -1 表示未找到（仅 table/static 模式可能出现）。
    virtual int route(const std::string& email) = 0;

    // 总分片数
    virtual size_t shard_count() const = 0;

    // 可读名称，用于日志和 metrics（"hash" / "table" / "static"）
    virtual const char* name() const = 0;

    // 获取指定 shard 的数据库连接池
    virtual std::shared_ptr<DBPool> get_db_pool(size_t shard) = 0;

    // 获取指定 shard 的存储提供者
    virtual std::shared_ptr<storage::IStorageProvider> get_storage(size_t shard) = 0;

    // 按优先级排序的 shard 索引列表（调用方依次轮询，本地优先）
    // 默认 0..N-1，后续可按延迟排序
    virtual std::vector<size_t> shard_priority_order() const {
        std::vector<size_t> order(shard_count());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        return order;
    }
};

} // namespace router
} // namespace mail_system

#endif
