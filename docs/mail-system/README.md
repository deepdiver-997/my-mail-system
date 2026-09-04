# 邮件系统业务文档

邮件系统（SMTP/IMAP/POP3 + 出站投递）的业务文档目录。框架/基础设施类文档在
[`../architecture/`](../architecture/)，构建运维在 [`../build-deploy/`](../build-deploy/)。

## 阅读顺序（新人建议）

1. **[protocol-flow.md](protocol-flow.md)** — 协议入门：三个协议各干什么、会话怎么流转。
   不熟悉 SMTP/IMAP/POP3 的人从这篇开始。
2. **[architecture/mailbox-concurrency.md](architecture/mailbox-concurrency.md)** —
   多客户端并发与邮箱一致性：为什么 SMTP 没事、POP3 怎么解决、IMAP 要怎么做。
   **理解历史问题的钥匙**。
3. **[architecture/pop3-server-design.md](architecture/pop3-server-design.md)** —
   POP3 服务器设计（含锁→租约）。
4. **[architecture/imap-server-design.md](architecture/imap-server-design.md)** — IMAP 服务器设计。
5. 按需深入其余架构 / bug 复盘。

## 架构

| 文档 | 内容 |
|------|------|
| [protocol-flow.md](protocol-flow.md) | 三协议会话流程入门（新人友好） |
| [architecture/mailbox-concurrency.md](architecture/mailbox-concurrency.md) | 多客户端并发对照 + 三层心智模型 |
| [architecture/pop3-server-design.md](architecture/pop3-server-design.md) | POP3 服务器（RFC 1939）设计 |
| [architecture/imap-server-design.md](architecture/imap-server-design.md) | IMAP（RFC 3501）服务器设计 |
| [architecture/imap-protocol-flow.md](architecture/imap-protocol-flow.md) | IMAP 协议流程细节 |
| [architecture/database-async-design.md](architecture/database-async-design.md) | DB 真异步化设计：MariaDB Connector/C 非阻塞 + prepared stmt 缓存 + mysql_ping 保活 |
| [architecture/smtp-outbound-client-design.md](architecture/smtp-outbound-client-design.md) | SMTP 发件引擎设计 |
| [architecture/inbound-verification-flow.md](architecture/inbound-verification-flow.md) | 入站校验（SPF/DKIM/DMARC）流程 |
| [architecture/vs-postfix.md](architecture/vs-postfix.md) | 与 Postfix 的对比 |

## Bug 复盘

| 文档 | 内容 |
|------|------|
| [bugfixes/2026-09-04-subject-too-long-silent-loss.md](bugfixes/2026-09-04-subject-too-long-silent-loss.md) | 超长主题 1406 回滚静默丢信 + 行折叠加固 |
| [bugfixes/2026-08-27-counter-triangle-bug.md](bugfixes/2026-08-27-counter-triangle-bug.md) | counter 累加成 N*(N+1)/2 |
| [bugfixes/2026-08-21-mail-body-rotation-fix.md](bugfixes/2026-08-21-mail-body-rotation-fix.md) | 正文文件错位写坏 |
| [bugfixes/2026-08-15-gmail-delivery-fix.md](bugfixes/2026-08-15-gmail-delivery-fix.md) | Gmail 投递（STARTTLS/证书） |
| [bugfixes/2026-08-02-smtp-imap-mime-fixes.md](bugfixes/2026-08-02-smtp-imap-mime-fixes.md) | SMTP/IMAP MIME 修复 |
| [bugfixes/2026-08-01-smtp-imap-deploy-fixes.md](bugfixes/2026-08-01-smtp-imap-deploy-fixes.md) | SMTP/IMAP 部署修复 |
| [bugfixes/imap-cpu-busyloop-fix.md](bugfixes/imap-cpu-busyloop-fix.md) | IMAP CPU 忙等 |

> 框架级 bug（编译器 / DB 连接池）留在 `../bugfixes/`。新 bugfix 按
> `YYYY-MM-DD-简短描述.md` 命名。

## 测试

测试文档见 [`test/README.md`](../../test/README.md)。
