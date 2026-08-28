# 多客户端并发与邮箱一致性：SMTP / POP3 / IMAP 对照

## 三层心智模型

先分清"安全"由谁保证，这是理解三个协议差异的钥匙：

| 层 | 谁保证 | 例子 |
|----|--------|------|
| **单条 SQL 原子性** | 数据库（行锁 + undo log） | `UPDATE mail_mailbox SET is_deleted=1 WHERE mail_id=?` |
| **多语句流程原子性** | 应用层（事务 / 显式锁） | POP3 的「DELE 标记 → QUIT 提交」 |
| **会话内快照一致性** | 协议设计 + 应用层 | IMAP 序列号漂移 / POP3 位置号身份 |

数据库只保证第一层。第二层要你自己用事务或锁。第三层取决于协议怎么标识消息、
会话怎么持有状态。

## 为什么 SMTP 没问题

SMTP 会话是一次性短事务：`MAIL FROM → RCPT TO → DATA → 入站持久化`。

- **只写不读邮箱状态**：DATA 落盘是往 `mails` + `mail_mailbox` **插入新行**，
  不同邮件是不同行 → 行锁天然并行，无争用。
- **无长生命周期快照**：会话结束即完成，不持有"邮箱当前长什么样"的持久视图。
- **RCPT 校验读 `users`**：读的是账号表（几乎不变的持久数据），不依赖别的会话
  刚改的临时状态。

结论：SMTP 的并发窗口只有"同时投递新邮件"，而这是纯插入，InnoDB 行锁自动解决。
**没有需要应用层协调的状态**。

## 为什么 POP3 必须解决，怎么解决的

### 根因：位置号 = 消息身份

POP3 用**位置号（1..N）**标识消息。两个会话并发修改收件箱时，位置号失去身份语义：

```
A 会话快照 [1001, 1002]          B 会话快照 [1001, 1002]
A DELE 1（删 1001）、QUIT
                                B RETR 2 → 现在"2"是哪封？
```

如果 A 删的是中间一封，B 的 "message 2" 就从 1002 漂移成别的。位置号在并发
修改下**不是稳定的身份**——这是协议层的问题，靠 DB 行锁救不了。

### 解法：单会话锁（RFC 1939 §6）

`pop3_session_lock` 表 + upsert/verify，做成**应用层互斥**：

- 每 user 一行，`session_id` 记持有者。
- 第二个会话 PASS 时抢不到锁 → 直接 `-ERR [IN-USE] try later`。
- 锁覆盖「读快照 + 全部 DELE/QUIT」，会话内位置号永远稳定。

为什么用"try-lock 拒绝"而不是 `SELECT ... FOR UPDATE` 阻塞？因为锁覆盖整个
会话（可能 idle 几分钟），阻塞会让第二个会话挂死；RFC 也要求冲突即拒绝。

### v1 缺口 → v2 补成租约（2026-08-28）

v1 只有 acquire/release：会话硬崩溃（SIGKILL/断电）不跑 QUIT/超时 → 锁泄漏
→ 该用户永久无法登录。补两环变成**带 TTL 的租约**：

- **心跳续约**：PASS 后每 60s 条件 UPDATE `last_heartbeat` + verify。条件
  UPDATE 而非 upsert——锁被回收后不反插抢占，verify 得 0 = 锁转交 → 关会话。
- **sweeper**：每 5min 删除心跳过期的锁，死锁 5min 内回收。

与 outbound 投递租约（`mail_outbox.lease_until=120s` + `requeue_expired_leases`）
同构：都是"DB 里记所有权 + 时间做所有权边界 + 崩溃可接管"。区别在意图——
POP3 是**排他互斥**（一个用户一个写者），outbound 是**分活**（多 worker 并发
claim 不同行）。

细节见 `docs/architecture/pop3-server-design.md` 的「锁 → 租约」节。

## IMAP 要怎么做（现状 + 该改的）

### 现状：无锁 + UID + 每次查库

IMAP 故意允许多会话（RFC 3501），靠两个机制兜一致性：

1. **UID = snowflake mail_id，全局稳定**。`FETCH UID` / `STORE` 按 UID 操作，
   不受别的会话删除影响。
2. **每次 FETCH / EXPUNGE / SEARCH 都重新查 DB**（`get_mailbox_mails`），
   会话 context **不冻结邮件列表快照**。被删的邮件从 `mail_mailbox` 行消失 →
   UID FETCH 直接查不到（正确行为）。

唯一缓存是 **mailbox stats 缓存**（EXISTS/UNSEEN/uidnext 计数，进程内、带
stale 标志 + 后台刷新），只缓存计数不缓存列表。STORE/EXPUNGE 会 invalidate。

### 已知症状（不是损坏，是协议层不一致）

| 场景 | 实际行为 | 是否要处理 |
|------|---------|-----------|
| A 删邮件，B 用**序列号** FETCH | 列表变短，序列号漂移 → B 可能拉到**另一封** | 客户端按 RFC 3501 需处理 EXPUNGE 通知重编号；UID 规避 |
| A 删邮件，B 用 **UID** FETCH | 查不到 → 不返回，客户端看到"没了" | 正确行为，无需处理 |
| B 看 STATUS/EXISTS | 缓存计数短暂不准 | 最终一致，后台刷新收敛 |
| 正文文件被清理 job 删 | UID FETCH 读 body 失败 | 需"先摘行、延迟删文件"或引用计数 |

### 该改的：uidnext 竞态（已修 2026-08-28）

`get_mailbox_uidnext` 原实现 `SELECT COALESCE(MAX(mail_id),0)+1`（`sql_queries.cpp:374`）
生成 UIDNEXT。两个真实缺陷：

1. **expunge 掉最大 mail_id 后 UIDNEXT 回落**——违反 RFC 3501 §2.3.1.1
   （UIDNEXT 必须永不小于已用过的 UID）。
2. **并发/跨实例读者各自 MAX+1**——非原子，读到同一值。

（注：实际 UID = snowflake mail_id，永不冲突，所以不是"算出重复 UID"的机制；
问题在**报告的 UIDNEXT**。）

**修法**：新增 per-mailbox 高水位表 `mailbox_uidnext(mailbox_id PK, uidnext)`，
`get_mailbox_uidnext` 改为原子推进 + 读取：

```sql
INSERT INTO mailbox_uidnext (mailbox_id, uidnext)
SELECT ?, COALESCE(MAX(mm.mail_id),0)+1 FROM mail_mailbox mm WHERE mm.mailbox_id = ?
ON DUPLICATE KEY UPDATE
  uidnext = GREATEST(uidnext,
    (SELECT COALESCE(MAX(mm.mail_id),0)+1 FROM mail_mailbox mm WHERE mm.mailbox_id = ?));
-- 再 SELECT uidnext FROM mailbox_uidnext WHERE mailbox_id = ?
```

- 行锁串行化并发/跨实例读者 → 原子。
- `GREATEST` 保证单调不回退（含 expunge 最大 mail_id 后）。
- 空邮箱聚合查询仍返回一行 uidnext=1。
- migration：`config/sql/migration_imap_uidnext.sql`。

### 跨实例部署的注意

邮箱缓存（stats cache）是**进程内**的，跨实例不共享。因为 FETCH 每次查库、
权威数据在 DB + storage，所以**不共享缓存不会损坏数据**——只是计数短暂不一致
（最终一致）。真正跨实例要防的是上面 uidnext 竞态和正文文件清理时序。
