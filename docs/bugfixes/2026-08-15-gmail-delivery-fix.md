# 2026-08-15 Gmail → <DOMAIN> 投递失败排查与修复

## 背景

Gmail 发信到 `test3@<DOMAIN>` 持续失败。从 2026-08-12 起多轮排查，先后处理 DNSBL、证书、TLS 版本、EHLO 合规，最终根因收窄到**服务器 IP 信誉**（PTR 缺失 + Barracuda RBL 命中）。本文记录排查过程、已部署的修复，以及遗留待办。

---

## 症状演变

| 时间 | Gmail 退信错误 |
|---|---|
| 08-14 | `TLS Negotiation failed: FAILED_PRECONDITION: starttls error(104): Connection reset by peer` |
| 08-15 后 | `The recipient server did not accept our requests to connect. [<MX_HOSTNAME>. <SERVER_IP>: timed out]` |

两种都是**网络/TLS 层错误**，不是 PTR 拒收（无 `4.7.23`/`5.7.25`）。

### "timed out" 而非 "Connection reset" 的原因

tcpdump 证实 Gmail 对该 IP 是**多路径多连接**投递（`mail-pz2`/`mail-qv2`/`mail-oa2` 等不同 MX 节点、间隔 1-3 小时多次重试）：

- 部分连接：建立成功后 **STARTTLS 阶段 Gmail 主动 RST**（ClientHello 后 ~53μs，服务器从未发出 ServerHello）
- 部分连接：**连接超时**（Gmail 侧 SYN/SYN-ACK 或会话交互超时，服务器可能未收到）

Gmail 退信显示的是**最终/代表性失败**。`timed out` 不是服务器代码新引入的问题，而是 Gmail 对该 IP 投递失败的另一种表现。**根因未变：IP 信誉。**

---

## 排查过程（已排除项）

### 1. DNSBL（已解决 08-12）
- `dnsbl_enabled: true → false`（Gmail Google Cloud IP 命中 Spamhaus CSS 被拒）。

### 2. 证书（已解决 08-12/08-15）
- 自签 → Let's Encrypt 有效证书（`<SMTP_HOSTNAME>`）。
- 08-15 用 DNS-01 重签，SAN 含裸域 `<DOMAIN>`（`<MX_HOSTNAME>, <DOMAIN>, <SMTP_HOSTNAME>`）。
- `openssl s_client` 465/25 均 `Verify return code: 0`。

### 3. TLS 版本（已排除 08-15）
- 服务器 OpenSSL 3.0.2 曾硬编码 `no_tlsv1_3`（强制 TLS1.2）；Gmail 2026 年发 TLS1.3 ClientHello 且无降级回退。
- 部署 `enable_tls1_3` 后 Gmail 仍 ClientHello 后 RST（01:44:45、15:07 均复测）。**TLS 版本无关。**

### 4. EHLO 响应合规（已修复 08-15，曾假设为根因）
- **问题**：服务器 EHLO 响应首行**回显客户端域名**：
  ```cpp
  // 旧代码（RFC 5321 违规）
  std::string response = "250-" + session->get_last_command_args() + " Hello\r\n";
  ```
  Gmail 发 `EHLO mail-pz2-f0.google.com`，服务器回 `250-mail-pz2-f0.google.com Hello` → **服务器自称是 `*.google.com`** → Gmail 安全机制判定异常。
- **修复**：新增 `helo_hostname` 配置，EHLO 首行改用服务器自身域名 `<MX_HOSTNAME>`。
- **验证**：部署后 01:52 抓包确认 EHLO 响应首行为 `250-<MX_HOSTNAME> Hello`，**但 Gmail 仍 ClientHello 后 RST**。故 EHLO 不是最终根因（但修复本身是 RFC 5321 合规的必要修正）。

---

## 已部署修复（本次提交）

1. **`enable_tls1_3` 配置开关**（[server_config_base.h](../../include/framework/server_config_base.h#L55)）
   - 默认启用 TLS1.3，替代硬编码 `no_tlsv1_3`。
   - [tcp_server_base.h](../../include/framework/tcp_server_base.h#L271) `load_certificates` 按配置条件设置。
2. **`helo_hostname` 配置**（[server_config.h](../../include/framework/server_config.h#L174)）
   - 入站 EHLO 响应首行主机名，默认空则回退 `system_domain`。
3. **EHLO 响应合规**（[traditional_smtps_fsm.tpp](../../include/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp#L262)）
   - 首行用服务器域名，不再回显客户端。
4. **配置**（[smtpsConfig.json](../../config/smtpsConfig.json)）：`enable_tls1_3: true`、`helo_hostname: <MX_HOSTNAME>`。
5. **测试断言更新**（[smtps_fsm_test.cpp](../../test/unit/smtps_fsm_test.cpp)）：4 处 `250-* Hello` → `250-test.local Hello`。

smtpsServer + imapsServer 均已交叉编译部署（二进制备份 `smtpsServer.bak-tls13-*`）。

---

## 最终根因（未解决，需外部操作）

**Gmail 在 ClientHello 后 ~53μs 主动 RST，无视服务器所有响应**（RTT 13ms，不可能基于服务器响应决策）→ 必然基于**连接前可查信息**判断该 IP 不可投递：

1. **PTR 缺失**：`<SERVER_IP>` 反向 DNS NXDOMAIN（Google DNS 8.8.8.8 视角确认）。
   - Gmail 严格要求 FCrDNS（PTR 指向 `<MX_HOSTNAME>` 且 A 记录回指同 IP）。修复需阿里云 EIP 反向 DNS 设置（`<MX_HOSTNAME>`，阿里云收费 1 元/天/条）。
2. **Barracuda RBL 命中**：`b.barracudacentral.org` 返回 `127.0.0.2`。
3. 阿里云深圳 IP 段 + 历史投递失败缓存。

---

## 遗留待办

- [ ] 开通阿里云 PTR（`<MX_HOSTNAME>`），验证 Gmail 投递恢复。
- [ ] Barracuda RBL 移除申诉（barracudacentral.org/lookups）。
- [ ] IMAP 用自签证书（Verify 18），用户邮件客户端连 IMAP 需信任，可换 Let's Encrypt 证书。

---

## 监控

- 日志：`/tmp/logs/smtp-server.log`（垃圾扫描器 IP 178.16.55.89 等频繁探测，非 Gmail）。
- tcpdump：`tcpdump -l -X -i eth0 'port 25 or port 465 or port 587'`（此机 tcpdump 写文件必须 `-l` 行缓冲，否则 0 包）。
- Gmail 连接源 IP 段：74.125.x（Gmail MX），SNI = `<MX_HOSTNAME>`。
