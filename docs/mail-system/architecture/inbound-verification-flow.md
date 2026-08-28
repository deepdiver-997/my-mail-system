# 入站校验（SPF/DKIM/DMARC）时序与异步化设计

## 背景

SMTP 入站（25 端口）在投递前做 SPF/DKIM/DMARC 三重验证，结果写入 `Authentication-Results` 头（RFC 8601）。这三者都涉及 **DNS TXT 查询**，且校验对象是**发件服务器（MTA）的 IP 和域名**而非单个客户端，复用率高。

2026-08-18 起改为**异步校验**：`InboundVerifier` 直接使用异步 `IDnsResolver`（c-ares），不再用 `SyncDnsWrapper` 同步阻塞 io_context。

---

## SMTP 会话状态机 + 校验时序

```
连接        INIT → GREETING → WAIT_EHLO       220 greeting
EHLO        WAIT_EHLO                         记录 ctx.ehlo_domain
MAIL FROM   WAIT_MAIL_FROM ──发起 SPF 异步──→ SPF TXT (+ include/a/mx 递归)
              └─SPF回调─→ hard-fail 且 spf_mode=hard → 550 拒
                         └→ 否则 250 OK → WAIT_RCPT_TO
RCPT TO     WAIT_RCPT_TO                      校验收件人（可多次）
DATA        WAIT_DATA → IN_MESSAGE            收正文（流式落盘）
DATA_END    IN_MESSAGE ──发起完整校验异步──→ DKIM: selector._domainkey TXT + 验签
                                              DMARC: _dmarc TXT + 评估(依赖 SPF/DKIM)
                                              SPF: 复用 MAIL FROM 结果 或补查
              └─校验回调─→ 组装 Authentication-Results 头 → 入队 → 250 OK → WAIT_MAIL_FROM
QUIT        WAIT_QUIT                         221 Bye
```

## 校验时机

| 校验 | 最早开始 | 必须完成 | 依赖 | DNS 查询 |
|---|---|---|---|---|
| SPF | MAIL FROM（有 client_ip + mail_from + ehlo_domain） | MAIL FROM 回复前（提前拒）或 DATA_END | 无 | SPF TXT + 递归 a/mx/include/redirect（深度 ≤10，RFC 7208） |
| DKIM | DATA_END（需完整 header+body） | 组装 Authentication-Results 前 | 完整 body | `selector._domainkey` TXT |
| DMARC | DATA_END（需 From 头域） | 组装 Authentication-Results 前 | From 域 + SPF 结果 + DKIM 结果 | `_dmarc` TXT |

### 说明

- **SPF 最早可在 MAIL FROM 阶段发起**（只需 `client_ip` + `mail_from` + `ehlo_domain`），不必等 DATA。SPF hard-fail 且配置 `inbound_spf_mode=hard` 时可在 MAIL FROM 提前 550 拒绝，节省带宽。
- **DKIM 必须等 DATA 结束**（需要完整 header + body 计算 hash）。
- **DMARC 必须等 DATA 结束**（需要 From 头域），且策略评估依赖 SPF/DKIM 结果，故在 SPF/DKIM 完成后评估。
- `Authentication-Results` 头必须在邮件**落盘/入队前**写入（写在正文前），因此 DATA_END 的校验必须在 `submit_mail_to_queue()` 之前完成。

---

## 异步化设计

### 接口

`InboundVerifier` 不再持有 `SyncDnsWrapper`，直接持 `IDnsResolver&`：

```cpp
class InboundVerifier {
    // 异步入口（callback 在 c-ares 线程触发，FSM 需 post 回 io_context）
    void check_spf_only_async(IDnsResolver&, client_ip, mail_from, helo, SpfCallback cb);
    void verify_all_async(...);            // 内存版
    void verify_all_from_file_async(...);  // body 文件流式版（生产 DATA_END 用）
};
```

### 关键约束：SPF 递归必须 CPS 异步化

`IDnsResolver` 回调在 **c-ares 单线程 event loop** 上触发。若在回调里同步等待另一次 DNS 查询（`SyncDnsWrapper`），会死锁。因此：

- SPF 的 `include`/`redirect`/`a`/`mx` 机制递归全部改为 **continuation-passing（CPS）**：每个 DNS 查询触发异步回调，回调里继续评估下一个机制，最终回调返回 `SpfResult`。
- `ip4`/`ip6`/`all` 等纯本地机制保持同步返回。
- include 深度上限 10 不变。

### 串行化（无需 post 回 io_context）

FSM 状态机函数（io_context 线程）发起异步校验前先 `session->set_paused(true)`，返回后 io_context 线程**不会继续处理该 session**（do_async_read 流水线循环检查 `is_paused()`，且无该 session 的 pending 异步操作）。因此 DNS 回调（c-ares 线程）**独占**操作 session，直接在回调里 `do_async_write`/`drain_buffered_commands` 即可，**无需 post 回 io_context**。shared_ptr 持有 session 保证其不被提前析构。

> 注意：asio 允许从非 executor 线程发起 async 操作（executor 类型兼容即可，跨 io_context 实例也兼容）。此处不 post 的安全前提是 `set_paused(true)` 排除并发——若改用手写多线程并发操作 session，仍需 strand 或回到 io_context 线程。

### 异步期间流水线处理

复用现有 `user_exists_async` 模式：异步校验期间 `session->set_paused(true)`，回调里 `drain_buffered_commands()` 恢复。

---

## 缓存设计（已实现 2026-08-18）

### 结论：值得加，但缓存**永远带 TTL**，不永久缓存

三重校验针对 MTA（发件服务器 IP/域名），复用率高。已实现 **DNS TXT 记录缓存**，复用通用线程安全 `LruCache`（[lru_cache.h](../../../include/mail_system/back/common/lru_cache.h)，shared_mutex+mutex 双锁 + LRU 淘汰），value 携带自定义 TTL。

| 缓存对象 | key | TTL（固定） |
|---|---|---|
| SPF 记录 | `domain` | 5 min |
| DMARC 记录 | `domain` | 15 min |
| DKIM 公钥 | `selector._domainkey` | 1 h |
| 负缓存（无记录/查询失败） | 各 key | 1 min |

> 注：SPF **结果**缓存（key = `client_ip + domain`）暂未做——DNS 记录缓存已覆盖同一发件域重复发信的主要收益，且结果依赖 ip/递归，语义复杂。如后续需要可再加。

### 线程安全

复用 `LruCache` 的双锁（`shared_mutex` 保护 hashmap + `mutex` 保护 LRU 链表），多个 c-ares 回调线程并发访问安全。超限时 **LRU 淘汰**（淘汰最久未用，而非清空），缓存大小稳定。缓存为 `InboundVerifier` 的**静态共享**（`inbound_dns_cache()`，跨校验复用），`clear_dns_cache()` 供测试隔离。

### 关于"永久有效/证书过期"

SPF/DMARC/DKIM 都是 **DNS TXT 记录**（不是 X.509 证书），本身有 DNS TTL。当前 `IDnsResolver` 接口**不返回 TTL**，故缓存用**固定 TTL** + 负缓存短 TTL。只要缓存有过期机制，DNS 记录更新/失效后会自动重新查询，**不会永久用旧值**。DKIM 公钥变化极低频，1h TTL 足够安全。

（可选后续：改造 `IDnsResolver` 返回真实 DNS TTL，用 min(记录 TTL, 配置上限) 替代固定值。）
