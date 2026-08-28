# SMTP Outbound Client 设计表

> **实现状态 (2026-07-29)**: 本文档为原始 MVP 设计。实际实现已演进：
> - `OutboundOrchestrator` → `OutboundServer`（事件驱动，消除常驻轮询线程）
> - 轮询 + 通知混合 → 纯事件驱动（completion_cb + submit + 退避定时器）
> - 水位预占 + CAS 单拉取周期替代定时轮询
> - 详见 [`ARCHITECTURE.md`](../../architecture/ARCHITECTURE.md) §4.2 和
>   [`architecture-evolution.md`](../../architecture/architecture-evolution.md) §阶段H

## 1. 设计范围

本文档定义 Mail System V8 的出站投递模块（SMTP Client）的最小可行设计（MVP）。

目标：
- 将系统内邮件可靠投递到外部 SMTP 服务器
- 支持多节点并发投递
- 支持异常恢复和重试
- 为后续分布式演进预留接口

非目标：
- 不实现外部 IMAP/POP3 拉取增强
- 不引入消息队列（Kafka/RabbitMQ）
- 不改造现有 IMAP/SMTPS 入站能力

## 2. 模块设计表

| 模块 | 职责 | 输入 | 输出 | 备注 |
|------|------|------|------|------|
| `OutboundOrchestrator` | 协调通知、轮询、任务分发 | 新邮件事件、定时器 | 批量任务派发 | 中央调度器 |
| `OutboxRepository` | Outbox 读写、原子 claim、状态迁移 | DB 查询参数 | 任务记录、更新结果 | 并发安全核心 |
| `SmtpDeliveryWorker` | 执行 SMTP 投递 | 出站任务、邮件内容 | 成功/失败结果 | 多 worker 并行 |
| `DeliveryDbExecutor` | 异步执行 DB 状态更新和批量 flush | 投递结果事件 | 状态更新结果 | 运行在任务线程池 |
| `DeliveryRetryPolicy` | 计算重试和回退时间 | 失败类型、尝试次数 | 下一次执行时间 | 指数退避 + 抖动 |
| `DeliveryNotifier` | 进程内通知 worker 快速拉取任务 | outbox_id/mail_id | 唤醒信号 | 低延迟触发 |
| `StorageProvider` | 读取邮件体与附件数据 | 存储键或路径 | 可读内容流 | 先本地，后对象存储 |
| `DeliveryMetricsLogger` | 记录投递指标、审计日志 | 执行阶段事件 | 结构化日志、指标 | 可观测性基础 |

## 3. 入口策略表

| 方案 | 实时性 | 可靠性 | 复杂度 | 结论 |
|------|--------|--------|--------|------|
| 仅轮询数据库 | 中 | 高 | 低 | 不单独使用 |
| 仅内存通知 | 高 | 低 | 低 | 不单独使用 |
| 对外监听端口接收任务 | 高 | 中 | 中高 | 当前不建议 |
| `Push + Poll` 混合 | 高 | 高 | 中 | 推荐 |

推荐落地：
- 内部函数入口（双轨）：`notifyNewOutboxItem(std::unique_ptr<mail> mail_ptr, reserved_outbox_records)`
- 快路径优先使用进程内已持有的邮件对象与已保留租约的 outbox 记录
- 慢路径（重启恢复/热路径失败）继续通过 `claim_batch()` 从数据库接管
- 保留定时轮询作为兜底（通知丢失或进程重启恢复）

## 4. 运行时线程模型（v1）

推荐 v1 采用单调度线程模型：

- `Client 主线程（单线程）`:
	- 轮询/通知唤醒
	- 维护出站会话状态
	- 轮询完成队列，汇总成功/失败结果
- `Asio IO 线程池`:
	- 负责 SMTP 网络 I/O（连接、握手、读写）
- `任务线程池（非 IO）`:
	- 负责耗时数据库操作（`markSent/markRetry/markDead`）
	- 支持批量 flush，减少数据库连接竞争

状态回传机制：

- 投递任务结束后，先写入线程安全完成队列（`CompletionQueue`）
- 主线程轮询完成队列，决定后续动作
- 实际数据库更新由任务线程池异步执行
- 更新完成后可回写对象状态（如 `DONE/FAILED`）供主线程观察

## 5. Outbox 表结构设计

建议表名：`mail_outbox`

说明：
- `mail_outbox` 为新增表，用于出站投递状态机，不替代原有 `mails/attachments`。
- 该方案可以最大限度减少对现有入站代码的改动。

| 字段 | 类型建议 | 说明 |
|------|----------|------|
| `id` | bigint PK | Outbox 主键 |
| `mail_id` | bigint/uuid | 邮件主键 |
| `status` | enum | `PENDING`/`SENDING`/`SENT`/`RETRY`/`DEAD` |
| `priority` | tinyint | 优先级（默认 0） |
| `attempt_count` | int | 已尝试次数 |
| `max_attempts` | int | 最大尝试次数 |
| `next_attempt_at` | datetime | 下次可投递时间 |
| `lease_owner` | varchar | 任务持有 worker |
| `lease_until` | datetime | 租约过期时间 |
| `last_error_code` | varchar | 最近错误码 |
| `last_error_message` | text | 最近错误详情 |
| `smtp_response` | text | 远端 SMTP 响应 |
| `sent_at` | datetime | 成功投递时间 |
| `created_at` | datetime | 创建时间 |
| `updated_at` | datetime | 更新时间 |
| `version` | int | 乐观锁版本（可选） |

建议索引：
- `(status, next_attempt_at, priority)`
- `(lease_until)`
- `(mail_id)`

## 6. 状态流转表

| 当前状态 | 触发条件 | 下一个状态 | 说明 |
|----------|----------|------------|------|
| `PENDING` | claim 成功 | `SENDING` | 写入租约信息 |
| `RETRY` | 到达重试时间并 claim 成功 | `SENDING` | 继续投递 |
| `SENDING` | SMTP 成功 + 本地落库成功 | `SENT` | 记录响应和时间 |
| `SENDING` | 临时失败（网络/4xx） | `RETRY` | 退避重试 |
| `SENDING` | 永久失败（5xx/策略拒绝） | `DEAD` | 进入死信 |
| `SENDING` | worker 崩溃且租约过期 | `RETRY` | 由其他节点接管 |

## 7. 并发与一致性策略

| 问题 | 方案 | 说明 |
|------|------|------|
| 多节点重复消费 | 原子 claim + 租约 | 同一时刻仅一个 worker 持有 |
| 节点宕机任务悬挂 | `lease_until` 超时回收 | 定时将过期任务转回 `RETRY` |
| 投递成功但宕机未写回 | 至少一次语义 | 接受极小概率重复投递 |
| 高频并发争锁 | 批量 claim + 小批次处理 | 降低锁持有时间 |

说明：
- SMTP 出站系统通常采用“至少一次”投递语义。
- 严格“恰好一次”在远端 SMTP 不可控条件下成本很高，不作为 MVP 目标。

## 8. 失败分类与重试策略

| 失败类型 | 示例 | 是否重试 | 策略 |
|----------|------|----------|------|
| 网络错误 | timeout/reset | 是 | 指数退避 |
| 临时错误 | SMTP 4xx | 是 | 限次重试 |
| 永久错误 | SMTP 5xx | 否 | 直接 `DEAD` |
| 本地配置错误 | 凭据/TLS 错误 | 否（或短限次） | 告警并阻断 |
| 内容错误 | 附件缺失/格式错误 | 否 | 标记 `DEAD` |

建议默认：
- `max_attempts = 8`
- 回退：`base = 30s`，上限 `30m`，加入 10%-20% 抖动

## 9. 分布式演进路线

| 阶段 | 数据库 | 文件存储 | 调度方式 | 说明 |
|------|--------|----------|----------|------|
| `v1` | MySQL | 本地磁盘 | Outbox + Push/Poll | 最快落地 |
| `v2` | 分布式 SQL（或主从） | 本地磁盘 | 多节点租约抢占 | 有跨节点文件访问风险 |
| `v3` | 分布式数据库 | 对象存储（S3/MinIO） | Outbox 或 MQ | 真正无状态、多活 |

关键结论：
- 仅替换数据库不足以支持分布式。
- 邮件正文和附件必须抽象为 `StorageProvider`，并迁移到所有节点可访问的统一存储。

## 10. 最小接口设计（不含实现）

| 接口 | 说明 |
|------|------|
| `enqueueOutbox(mail_id, priority)` | 创建出站任务 |
| `notifyNewOutboxItem(mail_ptr, reserved_records)` | 进程内快速唤醒（内存快路径 + DB 慢路径） |
| `pollAndDispatch(limit)` | 定时轮询兜底 |
| `claimBatch(worker_id, limit, lease_sec)` | 原子抢占任务 |
| `drainCompletionQueue(max_items)` | 主线程收敛投递完成事件 |
| `markSent(outbox_id, smtp_resp)` | 标记成功 |
| `markRetry(outbox_id, err, next_time)` | 标记重试 |
| `markDead(outbox_id, err)` | 标记死信 |
| `requeueExpiredLeases(now)` | 回收过期租约 |
| `flushStatusUpdates(batch_size)` | 任务线程池批量落库 |

## 11. MVP 验收清单

- 支持 `PENDING/SENDING/SENT/RETRY/DEAD` 全流程
- 支持并发 worker 且无重复 claim
- 节点重启后可恢复未完成任务
- 失败任务按策略重试并可进入 `DEAD`
- 可观测：日志中能追踪一次投递全链路
- 接口和存储抽象可兼容后续对象存储迁移
- 数据库更新可异步批量执行，不阻塞主线程轮询

## 12. 当前实现对照（2026-04）

本节用于回答“当前代码到底怎么跑”的问题，避免设计与实现偏差。

### 12.1 谁在查库并分发任务

- 当前是 `SmtpOutboundClient` 内部的 `orchestrator_thread_` 单线程循环负责：
	- `requeue_expired_leases()`
	- `claim_batch(worker_id, limit, lease_sec)`
	- 将 claim 到的记录分发到 IO 线程池或 worker 线程池执行 SMTP 投递
- 也就是说：
	- `client 主线程负责查库 + 分发`
	- 具体 SMTP 网络 I/O 在线程池执行

### 12.2 `mail_outbox` 何时写入

- 当前实现中，`mail_outbox` 与 `mails/mail_recipients/attachments` 已处于同一个事务边界：
	- `PersistentQueue::process_task()` 判断是否存在外域收件人，以及当前节点是否可以做本地热投递
	- `persist_mail_transactional(...)` 开启单个 DB 事务
	- 事务内依次写入：
		- `mails`
		- `mail_recipients`
		- `attachments`
		- `mail_outbox`
	- 只有事务提交成功后，才会把 `unique_ptr<mail>` 和预留的 outbox 记录交给本地 `SmtpOutboundClient`
- 如果当前节点具备热投递条件：
	- `mail_outbox` 会直接插入为 `SENDING`
	- 同时写入本机 `lease_owner` 和短期 `lease_until`
	- 这样其他节点在租约过期前不会抢走这批任务
- 如果当前节点不做热投递：
	- `mail_outbox` 正常以 `PENDING` 写入
	- 后续由各节点通过 `claim_batch()` 竞争获取
- 结论：
	- 不是“持久化完再补写 outbox”
	- 也不是“先本地投递，再决定要不要落 outbox”
	- 当前实现已经满足“持久化元数据和出站任务记录一起提交”

### 12.3 本地热投递到底做了什么

- 当前热投递优先解决的是“本机先拿到投递权”：
	- `PersistentQueue` 在事务里直接为本机写入短租约
	- 事务提交后把 `mail` 所有权和对应的 reserved outbox records 交给本地 `SmtpOutboundClient`
	- `SmtpOutboundClient` 直接调度这些 reserved records，不必再走一次 `claim_batch()`
- 如果本地接管失败：
	- `PersistentQueue` 会释放这些本地租约
	- 对应 outbox 记录回到 `PENDING`
	- 其他节点随后可以正常抢占，不会永久卡死
- 当前热投递还没有做到“完全不读文件”：
	- `build_outbound_message(...)` 仍然通过 `record.body_path` 读取正文
	- 也就是说，热路径目前节省的是跨节点竞争和一次 DB claim，不是完全绕开正文文件 I/O
- 真正的“纯内存热投递”需要进一步改造：
	- 让出站构造逻辑优先消费 `hot_mail_cache_` 里的正文/附件视图
	- 只有缓存缺失时才退回 `body_path` 读取

### 12.4 `mail_recipients.status` 当前语义

- 当前表注释语义：`1=未读，2=未送达`
- 当前代码按“是否本域”决定状态，但本地判断实现仍较粗糙（示例逻辑是固定比较），后续应统一为：
	- 本系统域内收件人：`未读`
	- 外域收件人：`未送达`
	- 与 `system_domain/local_domain` 一致判定

## 13. 竞争与轮询优化建议

### 13.1 单个 client 是否应按负载动态 sleep

建议是。可把每轮 sleep 改为“有任务快轮询、空闲慢轮询”的自适应策略。

示例策略：
- `claim_count == limit`：`sleep 5~20ms`
- `0 < claim_count < limit`：`sleep 30~80ms`
- `claim_count == 0`：指数回退到 `100~1000ms`（被 notify 时立即唤醒）

### 13.2 进程内由单轮询者统一 claim 是否减少竞争

在单进程内通常会减少竞争：
- 优点：
	- 降低同进程内多线程同时抢占 `mail_outbox` 的锁竞争
	- 更稳定的批量 claim 行为
- 代价：
	- 轮询/分发线程成为调度热点，需要保证不被慢操作阻塞

当前架构已接近该方案（单 orchestrator）。建议继续保持“单轮询 + 多执行线程”。

### 13.3 多进程/多节点竞争如何控制

- 保持现有 `claim_batch` 原子更新 + `lease_until` 超时回收
- 通过小批次 claim（例如 16~64）降低锁持有时间
- 避免在 claim 事务中做耗时操作（DNS/SMTP）

## 14. 推荐落地调整（下一步）

1. 让 `build_outbound_message(...)` 优先从 `hot_mail_cache_` 构造 MIME 数据，真正打通“纯内存热投递”。
2. 将本域判定统一封装，修正 `mail_recipients.status` 赋值逻辑，避免硬编码比较。
3. 评估是否需要把“本机保留租约时长”配置化，以便在多机房和慢链路环境下更细调优。
