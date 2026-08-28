# POP3 Server (RFC 1939)

## Overview

POP3 是继 SMTP / IMAP 之后补齐的第三个协议服务器，定位是 IMAP 的"lite"：
单 mailbox（INBOX）、无 SELECT 概念、10 条命令。本质是协议翻译 + 复用既有
基础设施（`ServerBase` / `SessionBase` / DB 连接池 / storage provider / FSM 框架），
验证框架的多协议复用能力。

```
┌──────────────────────────────────────────────────┐
│                  ServerBase                      │
│ (acceptor loop, SSL/TCP, metrics, connection mgt)│
├─────────────────┬────────────────────────────────┤
│  SmtpsServer    │  ImapsServer   Pop3Server      │
│  (SMTP FSM)     │  (IMAP FSM)    (POP3 FSM)      │
└─────────────────┴────────────────────────────────┘
```

## File Layout

```
include/mail_system/back/mailServer/
├── fsm/pop3/
│   ├── pop3_types.hpp            ← Pop3State/Pop3Event enums, Pop3Context, Pop3Message
│   ├── traditional_pop3_fsm.h    ← TraditionalPop3Fsm declaration
│   └── traditional_pop3_fsm.tpp  ← Transition table + handlers + 锁/心跳/sweeper
├── session/
│   ├── pop3_session.h            ← Pop3Session declaration
│   └── pop3_session.tpp          ← 命令解析 + close()（取消心跳）
├── pop3_server.h                 ← Pop3Server 声明（含锁 sweeper）
src/mail_system/back/mailServer/
└── pop3/pop3_server.cpp          ← 装配 + start/stop + 锁 sweeper 定时器
config/
└── pop3Config.json               ← 监听 110、metrics
config/sql/
└── migration_pop3_locks.sql      ← pop3_session_lock 表
```

## State Machine

RFC 1939 §3 四状态，`+OK`/`-ERR` 单行响应，多行响应以单独 `.` 行结束：

```
INIT ──(CONNECT, 自动 +OK banner)──▶ AUTHORIZATION
                                         │
                                    (USER + PASS 成功)
                                         │
                                         ▼
                                    TRANSACTION ──(QUIT)──▶ UPDATE ──▶ CLOSED
                                         │
                              (RETR 需 dot-stuffing)
```

| 命令 | 状态 | 说明 |
|------|------|------|
| CAPA | AUTH/TRANS | 能力列表（多行，`.` 结束） |
| USER | AUTH | 暂存用户名 |
| PASS | AUTH | 认证 + 抢 mailbox 锁 + 载入邮件快照 |
| STAT | TRANS | `+OK N total_octets` |
| LIST / LIST n | TRANS | 多行清单或单条 |
| UIDL / UIDL n | TRANS | snowflake mail_id 作 UID |
| RETR | TRANS | 读正文 + **dot-stuffing** |
| DELE | TRANS | 标记删除（不立即删，QUIT 才提交） |
| NOOP / RSET | TRANS | RSET 清空已标记删除 |
| QUIT | AUTH/TRANS | AUTH 直接 Bye；TRANS apply 删除 + 释放锁 |
| ERROR / TIMEOUT | 内部 | 未知命令回 -ERR 自环；超时释放锁+关闭 |

## Database Schema

三协议共用 `users`（认证）+ `mailboxes`/`mail_mailbox`/`mails`（邮箱层）。
POP3 独有新增一张锁表（RFC 1939 §6 单会话锁定）：

```sql
CREATE TABLE pop3_session_lock (
  user_id        BIGINT UNSIGNED PRIMARY KEY,
  session_id     VARCHAR(64)     NOT NULL,   -- snowflake，持有者 token
  acquired_at    DATETIME        NOT NULL,
  last_heartbeat DATETIME        NOT NULL,
  INDEX idx_heartbeat (last_heartbeat)
) ENGINE=InnoDB;
```

## 锁 → 租约（2026-08-28 更新）

v1 只有 acquire/release，无时间语义：会话被 SIGKILL/断电硬崩溃时不走
QUIT/超时，锁永远占着 → 该用户从此无法登录 POP3。本次补上租约的两环
（与 outbound 投递租约同构，见 `mailbox-concurrency.md`）。

### 抢锁（acquire）— upsert + verify

```sql
INSERT INTO pop3_session_lock (user_id, session_id, acquired_at, last_heartbeat)
VALUES (?, ?, NOW(), NOW())
ON DUPLICATE KEY UPDATE
  session_id     = IF(session_id = VALUES(session_id), VALUES(session_id), session_id),
  last_heartbeat = IF(session_id = VALUES(session_id), NOW(), last_heartbeat);
-- 之后 SELECT COUNT(*) ... WHERE user_id=? AND session_id=? 确认所有权
```

`ON DUPLICATE KEY UPDATE` 冲突时静默成功（不报错、async 桥只给 bool），
无法区分"锁是我的"还是"别人占着"，所以必须 verify：只有 session_id 匹配
才放行。行锁 + 主键唯一保证 upsert 和 verify 之间无 TOCTOU 窗口。

### 心跳续约（renew）— 条件 UPDATE，不是 upsert

```sql
UPDATE pop3_session_lock SET last_heartbeat = NOW()
WHERE user_id = ? AND session_id = ?;
-- + verify：行已不存在（被 sweeper 回收并转交他人）→ 失去锁 → 会话关闭
```

**关键**：续约用条件 UPDATE 而非 upsert。若心跳慢了、sweeper 已把锁回收并
转给新会话，upsert 会反插抢占偷回锁；条件 UPDATE + verify 则正确判为"锁丢了"。

### 定时器（start_heartbeat）— 递归弱引用

PASS 拿到锁后在 io_context 启动 `steady_timer`（默认 60s，测试可改短）。
每次 fire → post 到 worker 线程跑续约 SQL → 成功则 re-arm、失败则关闭会话。
递归回调只捕获 `weak_ptr`（`weak_self` / `weak_timer` / `weak_tick`），tick 本体
由 session 持有（`Pop3Context::heartbeat_handler`）——无 `session↔timer↔handler`
强引用环，`Pop3Session::close()` 幂等取消即释放。

### sweeper — 死锁回收

`Pop3Server::start_lock_sweeper()` 每 5min 删除心跳过期的锁：

```sql
DELETE FROM pop3_session_lock WHERE last_heartbeat < NOW() - INTERVAL 5 MINUTE;
```

硬崩溃会话心跳停 → 5min 内锁回收。心跳（60s）与清扫（5min）留足余量，
正常 idle 会话不会被误杀。

### 时序

```
PASS 成功 → start_heartbeat（60s 定时器）
每 60s：条件 UPDATE + verify
  ├─ verify=1 → re-arm（锁保住）
  └─ verify=0 → 锁已被回收/转交 → 异常关闭
sweeper（每 5min）→ 回收心跳过期的死锁
```

## Metrics（5 个）

| 指标 | 类型 | labels |
|------|------|--------|
| `protorelay_pop3_sessions_total` | counter | result (ok/err) |
| `protorelay_pop3_auth_total` | counter | result (ok/wrong_pass/fail_too_many/lock_conflict) |
| `protorelay_pop3_retr_total` | counter | — |
| `protorelay_pop3_dele_total` | counter | — |
| `protorelay_pop3_lock_conflict_total` | counter | — |

遵循 [[feedback-counter-semantics]]：所有 counter 传单次增量 `1`。

## Configuration

`config/pop3Config.json`：监听 110（TCP plain）、metrics、storage local。
v1 默认不开 995（POP3S）。

## Testing

- `test/unit/pop3_fsm_test.cpp` — 33 case 零 I/O FSM 测试。覆盖 11 命令 +
  状态错 + 大小写 + dot-stuffing + 锁（含心跳 6 case：timer 装配/续约保活/
  续约失锁/失锁关闭会话/关闭后取消/sweeper）。
  MockConnection 用 `deferred_read` 让 greeting 后 `do_async_read` 挂起而非
  EOF 提前关闭会话；`Handle` 析构幂等 close 防 LSan 引用环。
- `test/e2e/test_pop3_flow.py` — 9/9，TCP-only 不依赖 DB：验 greeting 格式 /
  CAPA / USER / PASS(-ERR) / QUIT + metrics 端点（sessions_total/auth_total）。

## Known Limitations & Roadmap (v2)

- ❌ **TOP** (RFC 1939 §7.6) — Gmail/Thunderbird 仍广泛用
- ❌ **APOP** (RFC 1939 §7.5) — digest 鉴权
- ❌ **DB e2e 验 RETR 真 body** — 需要 DB + 邮件 seed 脚本
- ❌ **POP3S port 995** — `TcpServerBase` 已支持 SSL listener，加 1 行 config
- ✅ **锁 sweeper** — 本次已实现（原计划 v2 项提前落地）
