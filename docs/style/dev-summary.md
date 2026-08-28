# IMAP 服务端开发总结

> 本文档记录 MailFront / ProtoRelay 项目中 IMAP 服务端的开发过程、
> 技术决策和架构设计，用于面试准备。

## 目录

1. [项目背景](#1-项目背景)
2. [协议基础](#2-协议基础)
3. [核心实现](#3-核心实现)
4. [邮箱名编码踩坑](#4-邮箱名编码踩坑)
5. [性能优化：LRU 缓存](#5-性能优化lru-缓存)
6. [SMTP 投递状态修复](#6-smtp-投递状态修复)
7. [架构决策记录](#7-架构决策记录)
8. [面试要点](#8-面试要点)

---

## 1. 项目背景

**MailFront** 是一个 Qt + VMime 的邮件客户端（C++17），**ProtoRelay** 是对应的邮件服务端（Boost.Asio + MySQL），包含 SMTP 和 IMAP 两个协议处理器。

### 技术栈

| 层次 | MailFront（客户端） | ProtoRelay（服务端） |
|------|-------------------|---------------------|
| 语言 | C++17 | C++17 |
| 网络 | VMime (libvmime) | Boost.Asio |
| UI | Qt6 (Widgets) | — |
| 数据库 | SQLite (本地缓存) | MySQL/MariaDB |
| 并发 | QtConcurrent | Boost.ThreadPool |
| 构建 | CMake | CMake + Bash |

### 架构对比

```
客户端 (MailFront)                   服务端 (ProtoRelay)
┌──────────────────┐                ┌──────────────────────┐
│  Qt UI 线程      │                │  Boost.Asio acceptor  │
│  QtConcurrent    │──── IMAP ────▶ │  ImapsSession (FSM)  │
│  VMime (同步I/O) │◀─── 响应 ──── │  TraditionalImapsFsm  │
└──────────────────┘                │  MySQL + 文件存储     │
                                    └──────────────────────┘
```

---

## 2. 协议基础

### IMAP 与 SMTP 的核心区别

| 维度 | IMAP | SMTP |
|------|------|------|
| **方向** | 拉取（服务器→客户端） | 推送（客户端→服务器） |
| **邮件位置** | 始终在服务器 | 投递后即完成 |
| **状态** | 有状态（邮箱选中、标志位） | 无状态（每个事务独立） |
| **认证时机** | 连接后先 LOGIN | 邮件发送前认证 |
| **连接时长** | 长连接（可 IDLE 等待） | 短连接（发完即断） |
| **命令模型** | 标签式异步 | 顺序请求-响应 |
| **加密** | STARTTLS 或 IMAPS(993) | STARTTLS 或 SMTPS(465) |
| **端口** | 143/993 | 25/587/465 |

### IMAP 状态机

```
INIT ──(CONNECT)──▶ NOT_AUTHENTICATED ──(LOGIN)──▶ AUTHENTICATED
                                                        │
                                              (SELECT/EXAMINE)
                                                        │
                                                        ▼
                                                    SELECTED
```

**关键点**：FETCH/STORE/EXPUNGE/COPY/MOVE 只能在 SELECTED 状态执行。

---

## 3. 核心实现

### 3.1 客户端 IMAP 修复

**问题**：IMAP 连接属性配置缺失，VMime 无法正确认证。

**根因**：VMime 的 IMAP 需要命名空间前缀的属性配置（类似 SMTP 的 `transport.smtp.auth.*`），但原代码只设置了全局 `auth.username` / `auth.password`。

**修复**：在 `imapclient.cpp` 的三个方法中添加：

```cpp
session->getProperties()["store.imap.auth.username"] = user;
session->getProperties()["store.imap.auth.password"] = password;
session->getProperties()["store.imap.options.need-authentication"] = true;

// 明文端口禁用 TLS/STARTTLS
if (!account.imapUseTls) {
    session->getProperties()["store.imap.options.connection.tls"] = false;
    session->getProperties()["store.imap.options.connection.tls-starttls"] = false;
}
```

### 3.2 IMAP 命令实现列表

| 命令 | 状态 | 功能 |
|------|:---:|------|
| CAPABILITY | ✅ | 通告 IMAP4rev1 / AUTH=LOGIN / STARTTLS / IDLE / UIDPLUS / MOVE |
| LOGIN | ✅ | bcrypt 密码验证，支持明文+加密混合 |
| LOGOUT | ✅ | BYE + 毫秒级延迟关闭 |
| SELECT / EXAMINE | ✅ | EXISTS / UNSEEN / UIDVALIDITY / UIDNEXT / READ-WRITE |
| LIST / LSUB | ✅ | IMAP-UTF-7 编码邮箱名 |
| STATUS | ✅ | MESSAGES / UNSEEN / UIDNEXT / UIDVALIDITY |
| FETCH | ✅ | FLAGS / INTERNALDATE / RFC822.SIZE / ENVELOPE / BODY[] |
| STORE | ✅ | +FLAGS / -FLAGS（\Seen / \Deleted / \Flagged） |
| EXPUNGE / CLOSE | ✅ | 数据库 DELETE 打标邮件 |
| CREATE / DELETE / RENAME | ✅ | 数据库操作 + UTF-7 编解码 |
| SEARCH | ✅ | ALL / UNSEEN / SEEN / DELETED |
| UID | ✅ | UID FETCH / STORE / SEARCH / COPY |
| APPEND | ✅ | literal 接收 + 存储 + APPENDUID |
| COPY / MOVE | ✅ | mail_mailbox 复制 + MOVE 标记删除 |
| STARTTLS | ✅ | 复用 ServerBase::handoff_starttls_socket |
| IDLE | ✅ | 进出 IDLE（推送通知待实现） |

### 3.3 数据库 Schema

```
users ──┬── mailboxes (INBOX, 收件箱, 发件箱, etc.)
         │      │
         │      └── mail_mailbox (邮件↔邮箱映射 + is_starred/is_deleted)
         │
         └── mail_recipients (邮件↔收件人 + read/unread status)
                 │
                 └── mails (subject, body_path, send_time)
```

**IMAP Flag 映射**：
- `\Seen` → `mail_recipients.status` = 0(已读) / 1(未读)
- `\Flagged` → `mail_mailbox.is_starred`
- `\Deleted` → `mail_mailbox.is_deleted`

**UID 方案**：使用 Snowflake `mail_id` 作为 UID（全局唯一、单调递增），`UIDVALIDITY` = `mailbox_id`。

---

## 4. 邮箱名编码踩坑

### 4.1 问题链条

```
RFC 3501 §5.1.3 要求 mailbox 名称用 modified UTF-7 (&base64-)
  → VMime 的 fromModifiedUTF7() 把 &...- 转成 +...- (标准 UTF-7)
  → macOS 的 iconv 解码 +...- 时不消耗末尾 '-' 分隔符
  → 客户端显示 "收件箱-" (多了个拖尾 '-')
```

### 4.2 解决方案

**三管齐下**：

1. **修 VMime 的 fromModifiedUTF7()**（`IMAPUtils.cpp`）：
```cpp
// 原代码：B64 序列结束符 '-' 总是输出
// 修复：仅在非 B64 序列中输出 '-'，B64 序列中静默消耗
case '-': {
    if (inB64sequence && prev == '&') out += '&';
    else if (!inB64sequence) out += '-';
    // 否则: B64 序列终止符，消耗不输出
    inB64sequence = false;
    break;
}
```

2. **服务端 encode_mailbox_name()**：实现完整的 modified UTF-7 编码（UTF-16BE → Base64 → `&...-`）

3. **服务端 decode_mailbox_name()**：手动解码 `&...-`，不依赖 iconv

4. **服务端 quote_string()**：IMAP-UTF-7 编码的名称强制加引号（VMime 只对 quoted-string 做 UTF-7 解码）

### 4.3 教训

- macOS 的 iconv 实现有 bug（不消耗 UTF-7 的 `-` 终止符）
- 关键路径编码不能用系统 iconv，需要自实现
- VMime 的 fromModifiedUTF7 本身也有同款 bug

---

## 5. 性能优化：LRU 缓存

### 5.1 动机

IMAP 的 SELECT/STATUS 每次都要 `SELECT COUNT(*)` 查询 MySQL，在高频轮询下（客户端每 30 秒 NOOP 一次）产生大量冗余 DB 查询。

```
无缓存：SELECT → MySQL (10-50ms) → 返回
有缓存：SELECT → LRU (0.001ms) → 返回
```

### 5.2 设计

**stale-while-revalidate + single-flight**（借鉴 HTTP Cache-Control）：

```
缓存命中且 TTL 内 ──→ 立即返回（0.001ms）
缓存 miss/stale ──→ 拿 flight 锁：
                     已有回源在途 → 挂到等待列表（owner 完成后共享结果）
                     无在途 → 复查缓存（owner 可能刚写完还没摘 flight）
                              仍 miss → 当 owner 发异步回源查询链
owner 完成 ──→ 写缓存 → 摘 flight → 统一通知所有等待者
```

关键点：同一 (user,mailbox) 任意时刻**至多一条回源查询链在途**，并发 SELECT
全部合并到等待列表，防 miss/stale 时 N 个连接各自查库打爆数据库、也防等者挂死。
锁序 flight→cache、两锁不嵌套；回调在释放 flight 锁后触发（等者会 re-enter）。
DB 失败也收敛（查询失败回调默认值，owner 照常通知）。

**核心实现**：

```cpp
struct MailboxCacheEntry {
    uint64_t exists;
    uint64_t unseen;
    uint64_t uidnext;
    uint64_t uidvalidity;
};

template <typename Key, typename Value>
class LruCache {
    // 线程安全 LRU
    // get(key, value, stale) → bool
    // put(key, value)
    // invalidate(key)
    std::shared_mutex m_mutex;  // 读多写少，读不互斥
};
```

**注入方式**：

```cpp
// ImapsServer 构造时自动创建并注入两个 FSM
auto cache = std::make_shared<MailboxStatsCache>(20000, 8s);
m_tcp_fsm->set_mailbox_stats_cache(cache);
m_ssl_fsm->set_mailbox_stats_cache(cache);
```

### 5.3 为什么只缓存 SELECT 计数而不缓存 FETCH 邮件列表？

| 维度 | SELECT 缓存 | FETCH 缓存 |
|------|:---:|:---:|
| 数据量 | 4 个整数 | 整个邮箱所有邮件的元数据向量 |
| 可变性 | 低频变化（新邮件到达） | 高频变化（STORE/EXPUNGE 改标志） |
| 一致性要求 | 弱（秒级滞后可接受） | 强（序号/UID 错误 = 删错邮件） |
| 失效范围 | 仅更新 4 个数字 | 全量重建序号→UID 映射 |
| **结论** | ✅ 低成本高收益 | ⚠️ 需要谨慎设计 |

SELECT 缓存覆盖了 90%+ 的重复查询，FETCH 属于"下载语义"——用户拉取邮件时允许更长的等待时间。

### 5.4 为什么不引入 Redis

1. **用户黏性**：用户长期连接同一台 IMAP 服务器，跨实例同步没必要
2. **本地 LRU 延迟更低**：0.001ms vs Redis 1ms（localhost 网络开销）
3. **零运维依赖**：不需要额外进程
4. **后续扩展性**：接口一致，换成 Redis 只需改 getter/setter 实现

### 5.5 SMTP 联动

**方案**：依赖注入 + 可选通知

```cpp
// SMTP 投递完成后
if (m_cache) {
    m_cache->notify_change(recipient_user_id, inbox_id);
}
// 如果 IMAP 没注入 cache，m_cache 为 nullptr，SMTP 照常工作
```

---

## 6. SMTP 投递状态修复

### 6.1 问题

客户端 `SmtpClient` 使用 `QFutureWatcher<void>` 异步发送邮件。`doSendEmail()` 中 catch 到异常后 emit `errorOccurred`，但 `onTaskFinished()` **无条件 emit `emailSent()`**。

```
流程：SMTP 中途出错 → catch 块 emit errorOccurred → 线程退出
      → onTaskFinished → isCanceled()==false → emit emailSent()  ← BUG
```

结果：即使 SMTP 连接失败/认证失败/DATA 被拒，UI 仍然显示"发送成功"。

### 6.2 修复

```cpp
// 改前
QFutureWatcher<void> *m_watcher;

// 改后
QFutureWatcher<bool> *m_watcher;  // doSendEmail 返回成功/失败

bool doSendEmail(...) {
    try {
        tr->connect(); tr->send(msg); tr->disconnect();
        return true;   // 完整 SMTP 流程成功
    } catch (vmime::exception &e) {
        emit errorOccurred(e.what());
        return false;  // 异常：不标记为已发送
    }
}

void onTaskFinished() {
    if (m_watcher->future().result() == true) {
        emit emailSent();
    }
}
```

**关键点**：`tr->send()` 是 VMime 的阻塞调用，内部等待服务器 DATA 命令后的 250 OK 确认。只有完整走完 connect → send → disconnect 无异常，才标记成功。

---

## 7. 架构决策记录

### ADR-1: 用 UID 而非序号操作邮件

- **背景**：IMAP 的序号在 EXPUNGE/新邮件到达后会漂移
- **决策**：客户端 STORE/FETCH 使用 UID 模式（`UID STORE` / `UID FETCH`）
- **效果**：序号漂移不影响数据操作，客户端维护 UID→seq 本地映射

### ADR-2: 邮箱名使用 IMAP-UTF-7 编码

- **背景**：RFC 3501 要求 modified UTF-7，但 iconv 有 bug
- **决策**：自实现编解码，不依赖系统 iconv
- **效果**：中文邮箱名正确显示，跨平台一致

### ADR-3: 共享 MySQL 而非 Dovecot 式索引文件

- **背景**：SMTP 和 IMAP 分离部署，需要单一事实来源
- **决策**：MySQL 为权威数据源，IMAP 缓存层做快照加速
- **效果**：无需 NFS/分布式文件系统，支持多实例部署

### ADR-4: 本地 LRU 而非 Redis

- **背景**：用户长期连同一台服务器，缓存不需要跨实例同步
- **决策**：进程内 LRU 缓存，stale-while-revalidate 模式
- **效果**：SELECT 延迟从 10-50ms 降到 0.001ms，零额外依赖

### ADR-5: SMTP/IMAP 耦合度最小化

- **背景**：SMTP 投递后 IMAP 缓存需要刷新
- **决策**：依赖注入接口 `IMailboxCache`，SMTP 不感知具体实现
- **效果**：SMTP 可独立运行；IMAP 注入则为可选优化

---

## 8. 面试要点

### 8.1 如果被问"IMAP 和 SMTP 的区别"

- IMAP 拉取（有状态、长连接），SMTP 推送（无状态、短连接）
- IMAP 邮件始终在服务器，SMTP 投递后即完成
- IMAP 用 tag 匹配异步响应，SMTP 是顺序请求-响应
- SELECT→FETCH→STORE→EXPUNGE 的完整操作链

### 8.2 如果被问"你的缓存是怎么设计的"

- stale-while-revalidate + single-flight：miss/stale 只发一条回源查询链，并发 SELECT 合并到等待列表
- 锁序 flight→cache 不嵌套；回调在释放 flight 锁后触发
- shared_mutex 优化读多写少场景
- 只缓存 SELECT 计数，不缓存可变邮件列表（一致性原因）
- 为什么不用 Redis：用户黏性、延迟更低、零运维

### 8.3 如果被问"遇到过什么技术难点"

1. **邮箱名编码**：RFC 3501 modified UTF-7 → macOS iconv bug → 自实现编解码 + 修 VMime
2. **SMTP 假完成**：Qt 异步模式 + QFutureWatcher 的线程同步陷阱
3. **IMAP connection 属性**：VMime 需要命名空间前缀的属性配置
4. **STATUS 宏冲突**：MySQL header 的 `STATUS` 宏与 C++ 枚举命名冲突

### 8.4 如果被问"怎么保证数据一致性"

- 邮件表/用户表/邮箱表用 MySQL 外键约束
- IMAP EXPUNGE 是事务性的（先打 \Deleted 标记，再 EXPUNGE 删除）
- UID 用 Snowflake 保证全局唯一单调递增
- 缓存层 stale-while-revalidate + TTL 自愈

### 8.5 关键数字

| 指标 | 值 |
|------|-----|
| SELECT 无缓存延迟 | 10-50ms |
| SELECT 缓存命中延迟 | 0.001ms |
| LRU 容量 | 20000 条 |
| LRU TTL | 8 秒 |
| Snowflake ID 范围 | 41 位时间戳 + 10 位机器ID + 12 位序列号 |
| MySQL charset | utf8mb4 |

---

> 最后更新：2026-05-19
> 对应 ProtoRelay commit: `d1c1a47`
