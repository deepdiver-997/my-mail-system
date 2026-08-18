# ProtoRelay 架构演进文档

> 本文档记录了项目从单线程 SMTP 原型到分布式邮件系统的完整演进过程。
> 基于 v1-v8 各版本 git 历史整理，2026-06-25；v8 阶段 I 更新至 2026-08-18。

---

## 版本总览

| 版本 | Git 提交数 | 时间跨度 | 核心主题 |
|------|-----------|---------|---------|
| v1-v2 | 无 git | 2025 年早期 | 单文件 SMTP 原型 |
| v3 | 3 | 2025-07-10 ~ 07-16 | 状态机架构，CMake 构建 |
| v4 | 9 | 2025-07-10 ~ 11-08 | 共享连接池，事件处理，日志系统 |
| v5 | 10 | 2025-07-10 ~ 12-21 | unique_ptr 重构，架构文档 |
| v6 | 无 git | 2025 年末 | 功能完整的 SMTPS 快照 |
| v7 | 16 | 2026-01-06 ~ 03-12 | RAII、持久化、出站投递、跨平台 |
| v8 | 47 | 2026-01-06 ~ 06-25 | 分布式、分片、IMAP、反垃圾、流水线 |

---

## 详细演进

### v1-v2：原型验证（2025 年初，无 git）

**架构**：单文件服务器，`mail_server.hpp` + `mail_client.hpp` 控制台交互。

```
[客户端] --telnet--> [mail_server.hpp]
                       ├── 简单的 SMTP 命令解析
                       └── 本地文件存储邮件
```

**特征**：
- 无并发，单连接阻塞式处理
- 邮件存储为内存中的字符串拼接
- 无网络抽象层

---

### v3：状态机引入（2025-07-10 ~ 07-16）

**架构演进**：引入 `TraditionalSmtpsFsm` 状态机模式。

```
[SMTPS 会话]
     │
     ▼
┌──────────────────────────────────────┐
│  SmtpsState 枚举 (11 个状态)          │
│  INIT → GREETING → WAIT_EHLO → ...  │
│                                      │
│  SmtpsEvent 枚举 (11 个事件)          │
│  CONNECT / EHLO / AUTH / DATA / ... │
│                                      │
│  StateTransitionTable               │
│  (state, event) → next_state        │
└──────────────────────────────────────┘
```

**关键变化**：
- CMake 构建系统取代手动编译
- SMTP 协议状态被形式化建模
- 状态迁移表驱动的事件处理

---

### v4：连接池与日志（2025-07-10 ~ 11-08）

**架构演进**：共享基础设施，日志系统独立。

```
[SMTP 服务器 1] ─┐
                 ├──→ [共享 DB 连接池]
[SMTP 服务器 2] ─┘
                 │
                 └──→ [Logger 单例]
                       ├── 分级日志 (TRACE/DEBUG/INFO/WARN/ERROR)
                       ├── 文件轮转
                       └── 控制台输出
```

**关键变化**：
- 多服务器实例共享同一个 DB 连接池
- 独立的 `Logger` 系统，支持多级别输出
- 修复了 `std::cin`/`std::cout` 缓冲区混用问题

---

### v5：所有权语义与文档（2025-07-10 ~ 12-21）

**架构演进**：资源管理现代化。

**关键变化**：
- 裸指针 → `std::unique_ptr` 全面重构
- 添加架构设计文档和编码规范
- 内存安全大幅提升

---

### v6：功能快照（2025 年末）

**角色**：v7 的起点快照，一个功能完整的单机 SMTP 服务器。

---

### v7：RAII、持久化、出站投递（2026-01-06 ~ 03-12）

**架构演进**：从"收下邮件"到"转发邮件"。

```
                       ┌──────────────────────┐
客户端 ──→ [SMTP 服务器] ──→ [PersistentQueue] │
            │    │        │  ┌─── 入队          │
            │    │        │  ├─── 持久化 Worker  │
            │    │        │  └─── 出队投递       │
            │    │        └──────────────────────┘
            │    │
            │    └──→ [SmtpOutboundClient]
            │          ├── c-ares DNS 解析 (MX 查询)
            │          ├── DKIM 签名
            │          ├── SMTP 出站握手
            │          └── 重试/退避策略
            │
            └── [RAII 连接管理]
                 └── ScopedConnection 自动归还
```

**关键变化**：
- **RAII 连接管理**：`ScopedConnection` 析构时自动归还连接池
- **持久化队列**：邮件先落盘再投递，防止进程崩溃丢信
- **SmtpOutboundClient**：完整的 SMTP 客户端实现
  - c-ares 异步 DNS 解析 MX 记录
  - DKIM 签名出站邮件
  - 指数退避重试
- **跨平台**：macOS + Linux (x64) 双平台构建

---

### v8：分布式、多协议、生产化（2026-01-06 ~ 06-25）

这是当前版本，演变可分为多个子阶段。

#### 阶段 A：分布式架构（2026-03-25）

```
                  ┌──────────────┐
                  │  ShardRouter │
                  └──┬───┬───┬──┘
                     │   │   │
              ┌──────┘   │   └──────┐
              ▼          ▼          ▼
        [Shard 0]  [Shard 1]  [Shard 2]
        ├─DB Pool  ├─DB Pool  ├─DB Pool
        ├─Storage  ├─Storage  ├─Storage
        └─Queue    └─Queue    └─Queue
```

#### 阶段 B：热投递与背压（2026-04-25 ~ 04-28）

```
邮件接收
    │
    ├── after_enqueue 模式 → 250 OK (快速确认)
    │         │
    │         └── PersistentQueue → OutboundClient (异步投递)
    │
    ├── 连接限流: maxConnections 达到阈值 → 拒绝新连接
    │
    └── 延迟应答: compute_reply_delay() → 负载高时故意慢响应
                 ├── 0ms: 负载 < 50%
                 ├── 250ms: 负载 50-75%
                 └── 500ms: 负载 > 75%
```

#### 阶段 C：入站验证（2026-05-01 ~ 05-13）

```
EHLO 命令
    │
    ├── PTR 反向 DNS 查询
    │   └── EHLO 域名 vs PTR 记录 → is_trusted_server
    │
MAIL FROM 命令
    │
    ├── SPF 验证 (DNS TXT 查询)
    │   └── 注入 Authentication-Results 头
    │
DATA_END (邮件接收完毕)
    │
    ├── DKIM 签名验证
    ├── DMARC 策略检查
    └── AUTH 策略:
        ├── auto: PTR 通过 or AUTH LOGIN
        ├── mandatory_auth: 必须 AUTH
        ├── mta_verify: PTR + SPF + DKIM 全部通过
        └── none: 来者不拒
```

#### 阶段 D：IMAP 服务成熟（2026-05-19）

```
[IMAP 服务器]
    │
    ├── FSM 状态机 (TraditionalImapsFsm)
    │   ├── NOT_AUTHENTICATED → AUTHENTICATED → SELECTED → LOGOUT
    │   └── 完整命令支持:
    │       APPEND / COPY / MOVE / SEARCH / STARTTLS
    │       SELECT / EXAMINE / FETCH / STORE / EXPUNGE
    │
    ├── IMAP-UTF-7 邮箱名编解码
    │
    └── LRU 缓存
        ├── AuthCache: email → {hash, status}, TTL 5min
        └── MailboxCache: SELECT/STATUS 结果缓存 (stale-while-revalidate)
```

#### 阶段 E：反垃圾与加固（2026-06-05 ~ 06-11）

```
连接建立
    │
    ├── DNSBL 检查 (Spamhaus Zen)
    │   └── 查询 <reversed_ip>.zen.spamhaus.org
    │       ├── 127.0.0.2 → SBL 已知垃圾源 → 拒绝
    │       ├── 127.0.0.3 → CSS 雪鞋攻击 → 拒绝
    │       └── 127.0.0.10-14 → PBL 动态IP → 拒绝
    │
    ├── 入侵检测
    │   ├── 跟踪每 IP 的失败认证次数
    │   ├── 达到阈值 → 封禁 IP
    │   └── 持久化封禁列表 (定期刷盘)
    │
    └── 邀请注册 (仅限指定域名)
```

#### 阶段 F：多监听器与分片抽象（2026-06-12 ~ 06-13）

**这是架构最重大的一次重构**：

```
                      ┌─────────────────────────-────┐
                      │       ServerBase             │
                      │  ┌───────────────────────┐   │
                      │  │ Listener 1 (port 25)  │   │
                      │  │  policy: mta_verify   │   │
                      │  ├───────────────────────┤   │
                      │  │ Listener 2 (port 587) │   │
                      │  │ policy: mandatory_auth│   │
                      │  ├───────────────────────┤   │
                      │  │ Listener 3 (port 993) │   │
                      │  │  type: IMAP SSL       │   │
                      │  └───────────────────────┘   │
                      │                              │
                      │  ┌───────────────────────┐   │
                      │  │    IShardRouter       │   │
                      │  │  ├─ route(email)→shard│   │
                      │  │  ├─ get_db_pool(shard)│   │
                      │  │  └─ get_storage(shard)│   │
                      │  └───────────────────────┘   │
                      └────────────────────────────-─┘

重构前: ServerBase 直接持有 m_dbPool, m_storageProvider
重构后: ServerBase 通过 m_shardRouter 间接访问所有后端资源
```

**关键变化**：
- **多监听器**：一个进程可以同时监听多个端口，每个端口独立配置认证策略
- **IShardRouter 抽象**：屏蔽底层分片细节
  - `HashShardRouter`：按邮箱哈希分片 (prod)
  - `StaticShardRouter`：固定分片 (dev/test)
  - `TableShardRouter`：按路由表分片
- **每分片指标**：Prometheus 标签带 `shard` 维度
- **JSON `/status` 端点**：取代纯文本 metrics，机器可读
- **移除跨分片耦合**：`OutboxRepository` 完全无状态化

#### 阶段 G：性能优化与流水线（2026-06-18 ~ 06-25）

```
do_async_read 入口
    │
    ├── has_buffered_input()?
    │   ├── YES → take_buffered_input() → handle_read() → process_read()
    │   │                                        ↑
    │   │                         解析一条完整命令，触发 FSM 事件
    │   │                         FSM 处理 → do_async_write 应答
    │   │                         应答完成 → 再次 do_async_read
    │   │                              │
    │   │                              └── 发现缓冲区还有命令 → 循环
    │   │
    │   └── NO → 发起网络读取
    │
    └── command_read_buffer_ (SessionBase 共享缓冲)
         ├── SMTP 使用
         └── IMAP 使用
```

**关键变化**：
- **IO 线程池修复**：TCP socket 之前绑定在单一 listener context，修复后使用 `IOThreadPool::get_io_context()` 的 4 线程轮询池
- **无锁队列**：`PersistentQueue` 用 `boost::lockfree::queue<capacity<16384>>` 取代 `deque+mutex+cv`
- **认证 LRU 缓存**：`LruCache<string, AuthCacheEntry>` 容量 10000，TTL 5 分钟，避免每次 AUTH 查 DB
- **SMTP/IMAP 命令流水线**：`command_read_buffer_` 提升到 `SessionBase`，`has_buffered_input()` + `take_buffered_input()` 实现命令批处理，无需修改 FSM
- **安全关闭流程**：`work_guard.reset() → io_context->stop() → join listener thread → close acceptors → stop subsystems → stop thread pools`
- **perf_mode**：一键跳过 SPF/DKIM/DMARC/DNSBL，自动覆写连接数/队列上限

#### 阶段 H：异步数据库接口与暂停流水线（2026-07-28 ~ 07-29）

```
IDBConnection 异步接口:
  ┌─────────────────────────────────────────────┐
  │ async_query(sql, params, callback)          │  ← 默认同步回调
  │ async_execute(sql, params, callback)        │     子类可重写为非阻塞
  │ async_begin_transaction(callback)           │
  │ async_commit(callback)                      │
  │ async_rollback(callback)                    │
  └─────────────────────────────────────────────┘

OutboundServer 事件驱动拉取:
  轮询常驻线程 → 水位预占 + CAS + 回调链
  try_pull(): 水位检查 → CAS抢占 → 预占BATCH_SIZE → do_claim_batch
                 │                                          │
      completion_cb ──→ try_pull()          on_claim_complete: 修正水位
      submit() ──────→ try_pull()               ├─ 有数据 → 分发 → 续拉/释放
      retry_timer ───→ 重新拉取                   └─ 无数据 → 指数退避(5→60s)

PersistentQueue 异步事务链:
  persist_mail_transactional_async()
    ├─ is_duplicate_async()
    ├─ async_begin_transaction()
    ├─ batch_insert_metadata_async()     ← 3层async_execute链
    ├─ batch_insert_attachments_async()
    ├─ enqueue_outbox_tasks_async()      ← 递归迭代收件人列表
    └─ async_commit()
    shared_ptr<ScopedConnection> 穿越回调链保持连接存活

FSM 暂停流水线 (paused gate):
  SessionBase 新增 paused_ 标志:
  handle_read 循环: while(buffer && !paused) { handle_read; process_read; }
  FSM handler: set_paused(true) → async DB → return (不阻塞IO线程)
  async回调: 发送响应 → drain_buffered_commands() (清paused + 排空缓冲命令)

  ⚠️ 设了paused的路径必须回调drain, 否则session永久卡死
```

**关键变化**：
- **`IDBConnection` 异步接口**：4 个 async 方法 + 3 个事务方法，默认同步实现，为未来非阻塞 MySQL 预留接口
- **`OutboundServer` 消除常驻线程**：`poll_loop` 改为事件驱动水位预占，`completion_cb`/`submit()`/退避定时器触发 `try_pull()`
- **`PersistentQueue` 异步事务链**：`persist_mail_transactional` 改为 callback 链式调用，`shared_ptr<ScopedConnection>` 保持连接生命周期
- **FSM 暂停流水线**：`SessionBase::paused_` 标志位，DB 查询期间暂停流水线消费，回调中 `drain_buffered_commands()` 恢复
- **SMTP/IMAP FSM handler 改造**：`auth_user → auth_user_async`(callback)，IMAP `handle_create/delete/rename` 改为 paused 模式
- **sync-bridge helpers**：`sq()`/`se()` 封装 async API → sync 结果，供非关键路径使用

#### 阶段 I：入站校验彻底异步化 + 并发测试（2026-08-18）

> 提交 `b34b10e` / `6140ea8` / `fbd07ff` / `734ac64`

**背景**：阶段 C 的 SPF/DKIM/DMARC 校验在 io_context 线程上同步执行——DNS 查询用
`SyncDnsWrapper` 阻塞等待，高负载下 io 线程被 DNS 延迟卡死。

**改进**：改用 `IDnsResolver` 原生异步接口（c-ares）+ CPS 续传。**不在 io 线程
阻塞，也不丢给工作线程**，回调直接在 c-ares 线程触发；session 用 paused 互斥保证
回调独占操作。

```
MAIL FROM 异步 SPF                     DATA_END 异步全量校验
    │                                    │
    ├─ set_paused(true) ← 暂停流水线       ├─ set_paused(true)
    │                                    ├─ verify_all_from_file_async
    ├─ check_spf_only_async              │   ├─ SPF (CPS 递归, include≤10层)
    │    └─ dns.async_resolve_txt        │   ├─ DKIM (verify_dkim_with_pubkey)
    │         └─ c-ares 线程触发回调       │   │    └─ 流式 body hash
    │              ├─ set_paused(false)  │   └─ DMARC
    │              └─ 续写 250/550        └─ c-ares 回调链独占 session
    │                                    ├─ set_paused(false)
    RCPT user_exists / AUTH 同模式        └─ verify→submit→ack 整体续传
    (DB async_query 回调续传)
```

**关键变化**：
- **InboundVerifier 异步入口**：`check_spf_only_async` / `verify_all_async` /
  `verify_all_from_file_async`；SPF `include/redirect/a/mx` 机制递归 CPS（深度 ≤10）
- **FSM 暂停互斥**：发起校验前 `set_paused(true)`（io 线程不再处理该 session），
  c-ares 回调独占操作 session，**无需 post 回 io_context**
- **DNS 记录缓存**：复用线程安全 `LruCache`（双锁+LRU），per-record TTL
  （SPF 5min / DMARC 15min / DKIM 1h，负缓存 1min），静态共享，到期自动重查
- **shared_ptr 保活**：`auth_user_async` / `user_exists_async` 回调持有 session
  shared_ptr，DB 真异步时 session 不会提前析构（`6140ea8`）
- **并发测试 + TSan**（`fbd07ff` / `734ac64`）：
  - 新 mock 还原 asio 投递语义：`MockIoContext`（任务队列+独立线程）、
    `MockDnsResolver`（Sync/Manual/AutoDelay 三模式）、`MockDbPool`（延迟 async_query）
  - 9 个并发用例覆盖 SPF / user_exists / auth / DMARC / DKIM 异步路径，含
    "回调晚于 session 结束"边界用例（shared_ptr 保活）
  - 暴露并修复真实 bug：`paused_`/`state_` 跨线程竞争（改 `std::atomic`）；
    MAIL/RCPT 回调 `drain` 后未检查 `is_paused` 导致过早 EOF 关闭 session
  - TSan（`-fsanitize=thread`）下零 data race

---

## 模块演进图

```
v1-v2        v3            v4            v7               v8
───────     ──────       ────────      ──────────       ──────────────
mail_       SmtpsFsm     Logger        PersistentQueue  IShardRouter
server.hpp  (状态机)      (日志)        (持久化队列)      (分片抽象)
                          │
            CMake         DBPool        OutboundClient   Multi-Listener
            (构建系统)     (连接池)      (出站客户端)      (多端口)
                                        │
                          RAII          DKIM             IMAP Server
                          (连接管理)     (签名)           (IMAP 协议)
                                        │
                          unique_ptr    c-ares DNS       DNSBL + IDS
                          (所有权)       (异步解析)       (反垃圾)
                                                        │
                                        Linux/macOS      Pipeline
                                        (跨平台)         (流水线)
```

---

## 关键架构决策记录

### ADR-1：状态机 vs 协程
**决策**：使用显式 FSM 状态机处理 SMTP/IMAP 协议，而非 C++20 协程。
**原因**：Boost 1.78 兼容性，C++17 标准，显式状态更易调试邮件协议。

### ADR-2：先持久化再确认 vs 先确认再持久化
**决策**：提供两种模式，默认 `AFTER_PERSIST`。
**原因**：生产环境优先数据安全，性能测试可用 `AFTER_ENQUEUE` 快速确认。

### ADR-3 (superseded by ADR-6)：会话使用 unique_ptr
**初始决策**：FSM 使用 `shared_ptr` 共享，会话使用 `unique_ptr` 独占。
**原因**：FSM 无状态逻辑，会话独占连接。
**问题**：流水线处理时 `unique_ptr` 传递引发深层递归（`do_async_read → process_read → handler → do_async_write → do_async_read → ...`），命令多时爆栈。

### ADR-6：会话改为 shared_ptr + while 循环消费流水线
**决策**：`SessionBase` 继承 `enable_shared_from_this`，`do_async_read/write` 改为成员函数。
**原因**：
- 读回调中 while 循环同步消费流水线命令，避免递归
- `shared_ptr` 让回调可安全持有 session 引用，不再需要 `unique_ptr` 所有权传递
- FSM 所有 handler 签名改为 `shared_ptr<SessionBase>`，`std::move` 消除
- 代码减少 ~900 行

### ADR-4：IShardRouter 抽象 vs 直接访问 DBPool
**决策**：引入路由抽象层。
**原因**：解耦分片策略，支持 hash/static/table 三种路由方式可切换。

### ADR-5：流水线不修改 FSM
**决策**：流水线复用在 IO 层（SessionBase）而非 FSM 层。
**原因**：FSM 是单命令单事件模型，不适合改动。IO 层缓冲积压命令，逐条喂给 FSM 处理。

---

