# 分布式分片重构

## 动机

ProtoRelay 最初是单节点架构：一个 DB 连接池、一个文件存储目录、所有用户共享。要支持水平扩展，必须引入分片路由层，让系统能够根据用户邮箱将请求定向到正确的 shard。

## 架构讨论过程

### 第一轮：路由抽象

最初的提案是简单的 `IShardRouter` 接口，只负责 `email → shard_index` 映射。有三种实现：

| 实现 | 路由方式 | 场景 |
|------|---------|------|
| HashShardRouter | hash(email) % N | 逻辑分片 |
| TableShardRouter | 查 user_shards 表 + LruCache 缓存 | DB 驱动的物理分片 |
| StaticShardRouter | 配置文件 domain→shard 映射 | 按域名隔离 |

用户提出两个关键问题：
1. **配置多源**：Router 的数据源是否应该支持 MySQL/PostgreSQL/SQLite 多种？
   - 结论：不需要在 Router 层重复抽象——`TableShardRouter` 通过现有 `DBPool` 接口查询，DB 层的多源已经由 `DBPoolFactory` 处理
2. **只读缓存**：user_shards 映射一旦分配永不变更，用 `LruCache<string,int>` TTL=0

### 第二轮：Router 持有资源数组

用户发现初始设计的问题：每个组件仍然直接持有 `shared_ptr<DBPool>` 和 `shared_ptr<IStorageProvider>`，调用方改动太大。

关键讨论：
> "把抽象文件系统数组和 dbpool 数组都放入 router 里，然后 router 提供和现有相似的接口，这样调用方就不会有很大的改动"

改进后的接口：

```cpp
class IShardRouter {
    virtual int route(const std::string& email) = 0;
    virtual size_t shard_count() const = 0;
    virtual std::shared_ptr<DBPool> get_db_pool(size_t shard) = 0;
    virtual std::shared_ptr<IStorageProvider> get_storage(size_t shard) = 0;
};
```

Router 在构造时创建每个 shard 的 DB 池和存储，对外暴露 `get_db_pool(shard)` / `get_storage(shard)`。调用方只需：

```cpp
// 之前
auto conn = m_dbPool->acquire_connection();
// 之后
int shard = m_shardRouter->route(email);
auto conn = m_shardRouter->get_db_pool(shard)->acquire_connection();
```

### 第三轮：消除默认 shard 的安全隐患

`ServerBase` 中加了便捷方法 `get_db_pool(int shard = 0)`，但用户敏锐地指出了问题：

> "像这种代码是不是其实很危险啊，万一是分片模式，这样可能会拿到错误的存储对象"

SMTP session 的 20 处存储操作如果使用默认 `shard=0`，会在多 shard 部署时把 shard3 用户的邮件写到 shard0 的存储。解决方案：

1. 删除带默认值的便捷方法
2. SMTP session 改为 `m_shardRouter->get_storage(ctx.shard_index)` — 从 session context 取 shard
3. IMAP FSM tpp 改为 `this->acquire_connection(ctx->shard_index)`

### 第四轮：消除裸资源依赖——删掉旧的让编译器找

这是最关键的架构决策。用户提出：

> "fsm、outbound 和持久化队列都不要直接拿 DBPool 或者 IStorageProvider，邮件对象里有足够的信息去 router 算 shard，这种裸资源只有在被调用时才会出现，不要作为参数传递"

**重构方法：先删除旧成员，让编译器报错，根据错误逐个修复。**

#### 删除 ServerBase 的 m_dbPool 和 m_storageProvider

```cpp
// server_base.h — 删除
- std::shared_ptr<DBPool> m_dbPool;
- std::shared_ptr<storage::IStorageProvider> m_storageProvider;
```

编译器立即报出所有直接引用处：

```
server_base.h:157: error: use of undeclared identifier 'm_dbPool'
smtps_server.cpp:25: error: use of undeclared identifier 'm_dbPool'
smtps_session.tpp:150: error: no member named 'm_storageProvider'
...
```

然后逐个改为走 router：

| 文件 | 改动 |
|------|------|
| `server_base.h` metrics | `m_dbPool->get_pool_size()` → `m_shardRouter->get_db_pool(0)->get_pool_size()` |
| `smtps_server.cpp` PersistentQueue | `new PersistentQueue(m_dbPool, ..., m_storageProvider)` → `new PersistentQueue(m_shardRouter, ...)` |
| `smtps_server.cpp` OutboundClient | `new SmtpOutboundClient(m_dbPool, ...)` → `new SmtpOutboundClient(m_shardRouter, ...)` |
| `smtps_server.cpp` FSM | `new TraditionalSmtpsFsm(..., m_dbPool, m_shardRouter)` → `new TraditionalSmtpsFsm(..., m_shardRouter)` |
| `smtps_session.tpp` | `m_server->m_storageProvider` → `m_server->m_shardRouter->get_storage(ctx.shard_index)` |

#### 删除 FSM 的 m_dbPool 和 m_storageProvider

```cpp
// imaps_fsm.hpp — 删除
- std::shared_ptr<DBPool> m_dbPool;
- std::shared_ptr<storage::IStorageProvider> m_storageProvider;

// 构造函数简化
- ImapsFsm(..., db_pool, storage_provider, shard_router)
+ ImapsFsm(..., shard_router)
```

编译器又报出 tpp 中 4 处 `this->m_dbPool->acquire_connection()`，全部改为 `this->acquire_connection(ctx->shard_index)`。

#### 删除 PersistentQueue 的 db_pool_ 和 storage_provider_

```cpp
// persistent_queue.h — 删除
- std::shared_ptr<DBPool> db_pool_;
- std::shared_ptr<storage::IStorageProvider> storage_provider_;

// 新增分片辅助
+ int shard_from_mail(const mail* m) const;
```

从邮件对象的 `from`/`to` 字段计算 shard：

```cpp
int PersistentQueue::shard_from_mail(const mail* m) const {
    const std::string& key = !m->from.empty() ? m->from :
                             (!m->to.empty() ? m->to.front() : "");
    int shard = m_shardRouter->route(key);
    return shard >= 0 ? shard : 0;
}
```

持久化事务中使用：`m_shardRouter->get_db_pool(shard_from_mail(mail_data))->acquire_connection()`。

#### 删除 OutboundClient 的 db_pool_

同样改为只持有 `m_shardRouter`。

### 第五轮：OutboxRepository 去状态化

用户进一步提出：

> "这个从数据库扫货未投递的邮件元数据的 Repository 还是持有原始数据库连接池，你觉得有必要改造吗？要不让这个 Repo 不要持有连接池而是在调用时传入连接池"

以及多 shard 任务窃取策略：

> "调用者优先处理本地的未处理事件，然后本地处理完了就找到目前最空闲的连接池去窃取对应数据库的投递任务"

#### OutboxRepository 改为完全无状态

```cpp
// 之前
class OutboxRepository {
    std::shared_ptr<DBPool> db_pool_;
    explicit OutboxRepository(std::shared_ptr<DBPool>);
    std::vector<OutboxRecord> claim_batch(worker_id, limit, lease_seconds);
};

// 之后
class OutboxRepository {
    OutboxRepository() = default;  // 默认构造，零状态
    std::vector<OutboxRecord> claim_batch(DBPool& db, worker_id, limit, lease_seconds);
};
```

每个方法接收 `DBPool&`，调用时由调用方从 router 获取：

```cpp
auto db = m_shardRouter->get_db_pool(shard);
auto records = repository_.claim_batch(*db, worker_id_, limit, lease);
```

#### IShardRouter 优先级排序接口

```cpp
// 返回按优先级排序的 shard 索引列表
// 默认 0..N-1，后续可按 DB 往返延迟排序
virtual std::vector<size_t> shard_priority_order() const;
```

用户提出的排序思路：
> "如果要区分本地和外部的优先级，是不是得给 router 内部资源数组排个序？要不按照往返延迟排序，在启动前？"

当前默认实现返回 `0..N-1`（所有 shard 等权）。子类可覆盖为延迟排序——启动时对各 shard 的 DB 执行 `SELECT 1` 测 RTT，按延迟升序排列。

#### SmtpOutboundClient 多 shard 轮询

```cpp
void run_loop() {
    while (running_) {
        // 回收所有 shard 的过期租约
        for (auto shard : steal_shard_order_)
            repository_.requeue_expired_leases(*m_shardRouter->get_db_pool(shard));

        // 本地优先 → 按优先级偷空闲 shard 的任务
        std::vector<OutboxRecord> records;
        for (auto shard : steal_shard_order_) {
            records = repository_.claim_batch(*m_shardRouter->get_db_pool(shard), ...);
            if (!records.empty()) break;
        }
    }
}
```

## 最终架构

### 依赖关系图

```
                    IShardRouter
                   /    |     \
          get_db_pool  route  get_storage  shard_priority_order
             /           |          \              |
        DBPool[]    shard_index  Storage[]    [home, steal_1, ...]
```

### 各组件的资源获取方式

| 组件 | 持有 | shard 来源 |
|------|------|-----------|
| **ServerBase** | `m_shardRouter` | 无直接使用，仅转发 |
| **ImapsFsm** | `m_shardRouter` | `ctx->shard_index`（AUTH 时由 router 计算） |
| **SmtpsFsm** | `m_shardRouter` | `ctx->shard_index`（AUTH 时由 router 计算） |
| **SmtpsSession** | 通过 `m_server->m_shardRouter` | `context_.shard_index` |
| **PersistentQueue** | `m_shardRouter` | `shard_from_mail(mail)` — 从 `mail->from`/`mail->to` 计算 |
| **SmtpOutboundClient** | `m_shardRouter` | `steal_shard_order_` — 轮询所有 shard |
| **OutboxRepository** | **无状态** | 调用时传入 `DBPool&` |

### 编译期驱动的重构方法

核心技巧：**先删除旧的成员/参数，让编译器找出所有未重构的调用处，然后逐个修复。**

```
1. 删除 ServerBase::m_dbPool → 编译报 20+ 处错误 → 逐个改为走 router
2. 删除 FSM::m_dbPool → 编译报 tpp 中 4 处 + hpp 中 20 处 → 逐个改为 acquire_connection(shard)
3. 删除 PersistentQueue::db_pool_ → 编译报 8 处 → 逐个改为 shard_from_mail
4. 删除 OutboundClient::db_pool_ → 编译报 2 处 → 改为 m_shardRouter
```

### 关键安全改进

- **SMTP session 存储操作**：20 处 `m_server->m_storageProvider` 全部改为 `m_server->m_shardRouter->get_storage(ctx.shard_index)`，杜绝写错 shard
- **无默认 shard**：删除所有 `get_db_pool()` / `get_storage()` 默认值，强制调用方显式传递 shard
- **PersistentQueue shard 感知**：邮件持久化不再固定用 shard 0，而是根据邮件内容计算目标 shard

## 相关文件

### 新建文件
| 文件 | 说明 |
|------|------|
| `include/mail_system/back/router/i_shard_router.h` | 纯虚接口 |
| `include/mail_system/back/router/hash_shard_router.h` | 哈希路由（header-only） |
| `include/mail_system/back/router/table_shard_router.h` | DB 查表路由 |
| `include/mail_system/back/router/static_shard_router.h` | 静态域名路由（header-only） |
| `src/mail_system/back/router/table_shard_router.cpp` | DB 路由实现 |
| `config/router_config.json` | 路由配置示例 |
| `config/sql/create_tables.sql` | 新增 user_shards 表 |

### 修改文件
| 文件 | 改动 |
|------|------|
| `include/mail_system/back/mailServer/server_config.h` | 新增 ShardRouterConfig |
| `include/mail_system/back/mailServer/server_base.h` | 删除 m_dbPool/m_storageProvider，新增 m_shardRouter |
| `src/mail_system/back/mailServer/server_base.cpp` | 重构构造顺序，router 包装所有资源 |
| `include/mail_system/back/mailServer/fsm/imaps/imaps_fsm.hpp` | 删除 m_dbPool/m_storageProvider，新增 acquire_connection/get_storage |
| `include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h` | 简化构造函数 |
| `include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp` | 使用 ctx->shard_index |
| `include/mail_system/back/mailServer/fsm/smtps/smtps_fsm.hpp` | 删除 m_dbPool，新增 acquire_connection |
| `include/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h` | 简化构造函数 |
| `include/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp` | AUTH 计算 shard 并存入 ctx |
| `include/mail_system/back/mailServer/session/smtps_session.tpp` | 使用 ctx.shard_index |
| `src/mail_system/back/mailServer/smtps/smtps_server.cpp` | 通过 router 创建所有子组件 |
| `src/mail_system/back/mailServer/imaps/imaps_server.cpp` | 通过 router 创建 FSM |
| `include/mail_system/back/persist_storage/persistent_queue.h` | 删除 db_pool_/storage_provider_，新增 m_shardRouter |
| `src/mail_system/back/persist_storage/persistent_queue.cpp` | shard_from_mail，所有 DB/存储操作走 router |
| `include/mail_system/back/outbound/smtp_outbound_client.h` | 删除 db_pool_，新增 m_shardRouter + steal_shard_order_ |
| `src/mail_system/back/outbound/smtp_outbound_client.cpp` | 多 shard 轮询，本地优先 |
| `include/mail_system/back/outbound/outbox_repository.h` | 去状态化：构造无参，方法接收 DBPool& |
| `src/mail_system/back/outbound/outbox_repository.cpp` | 重构所有方法签名 |

## 验证

1. `cmake --build build` 三目标 (smtpsServer / imapsServer / mailServer) 编译通过
2. 单 shard 部署：`shard_count=1` 时所有 `route()` 返回 0，`get_db_pool(0)` 返回唯一池，行为与重构前完全一致
3. 多 shard 部署：配置 `router_config.json` 中 `shard_count=N` 并配好各 shard 的 DB/存储，不同用户的请求自动路由到对应 shard
