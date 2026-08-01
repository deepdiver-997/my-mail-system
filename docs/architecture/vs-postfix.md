# ProtoRelay vs Postfix 生态对比

## 定位差异

| | Postfix | ProtoRelay |
|---|---|---|
| **类别** | MTA（邮件传输代理） | 一体化邮件系统 |
| **范围** | SMTP 收发 + 队列 + 投递 | SMTP + IMAP + 存储 + 认证 + 反垃圾 + 分片 |
| **理念** | Unix 哲学：每个程序做一件事 | 单体高效：一个二进制完成全链路 |
| **对标** | Sendmail / Exim | Coremail / iRedMail / Mailcow |

Postfix 本身只做 SMTP/队列/投递。一个完整的 Postfix 邮件系统通常需要：

```
Postfix (MTA) + Dovecot (IMAP) + SpamAssassin (反垃圾)
  + ClamAV (反病毒) + Roundcube (Webmail) + fail2ban (安全)
```

ProtoRelay 将这些功能整合在一个二进制中。

---

## 架构对比

### 进程模型

| | Postfix 栈 | ProtoRelay |
|---|---|---|
| 运行进程数 | 20-50 个（master + smtpd × N + qmgr + Dovecot × N + …） | 1 个（多线程） |
| I/O 模型 | Postfix 自实现 event loop、Dovecot libevent、Amavis Net::Server 三种不同实现 | 统一 `boost::asio::io_context`（epoll/kqueue） |
| 进程间通信 | SMTP/LMTP/Sieve 协议桥接 + Unix socket | 内存内函数调用 |
| 故障域 | 单进程崩溃不波及全部 | 崩溃影响所有协议 |

Postfix 多进程模型隔离了故障：一个 SMTP 会话崩溃不影响 IMAP 或队列。ProtoRelay 单进程崩溃则全部中断，需要靠 systemd/containerd 自愈。

### 配置管理

**Postfix 栈（典型部署配置分布）：**

```
/etc/postfix/main.cf         — 800+ 行，MTA 核心行为
/etc/postfix/master.cf       — 进程定义
/etc/dovecot/dovecot.conf    — IMAP/POP3/Sieve
/etc/dovecot/conf.d/         — 10-15 个分离的配置文件
/etc/amavis/conf.d/          — 反垃圾反病毒
/etc/postfix/mysql_virtual_*.cf — 数据库映射
/etc/fail2ban/jail.local     — 入侵防护
/etc/roundcube/config.inc.php — Webmail
/etc/nginx/sites-enabled/    — 反向代理
```

**ProtoRelay：**

```
config/smtpsConfig.json      — 全部服务器配置
config/db_config.json        — 数据库连接（可选独立文件）
config/router_config.json    — 分片配置（可选）
```

Postfix 配置分散在多个软件的目录中，不同文件之间彼此耦合（Dovecot 的 SASL socket 路径要写进 Postfix，Amavis 的监听端口要两边都配）。ProtoRelay 所有配置使用一种语法，在一处修改即可生效，支持热重载。

### 水平扩展

| | Postfix + Dovecot | ProtoRelay |
|---|---|---|
| 分片方法 | Dovecot Director 代理 + 外部路由表 | 内置 IShardRouter（hash/table/static） |
| 额外组件 | director 进程 + `user_shards` 映射维护 | 零额外组件 |
| 投递队列 | 每个 Postfix 实例独立队列 | SmtpOutboundClient 多 shard 任务窃取 |

---

## 功能对比

### 协议与安全

| 能力 | Postfix 栈 | ProtoRelay |
|---|---|---|
| SMTP + STARTTLS / SMTPS | Postfix ✅ | ✅ |
| Submission (587) | Postfix ✅ | ✅ 可配 per-port auth policy |
| IMAP + STARTTLS / IMAPS | Dovecot ✅ | ✅ |
| SPF | 需外挂 policyd-spf 或 milter | ✅ 内置 |
| DKIM (出站签名 + 入站验证) | OpenDKIM (独立守护进程) | ✅ 内置 |
| DMARC | OpenDMARC (独立守护进程) | ✅ 内置 |
| 入侵检测 / IP 封禁 | fail2ban 扫日志（反应式，minute 级延迟） | ✅ accept 层实时封禁 |
| DNSBL | postscreen / 外挂 | ✅ 内置(Spamhaus Zen) |
| 反垃圾 | SpamAssassin (Perl, 独立进程) | 内置静态检测 + LLM 模式接口(可选) |
| 反病毒 | ClamAV (独立进程, via Amavis) | ❌ 未实现 |
| bcrypt 密码 | Dovecot 支持 | ✅ |

### 邮件处理

| 能力 | Postfix 栈 | ProtoRelay |
|---|---|---|
| 持久化队列 | Postfix qmgr (文件系统队列) | ✅ 数据库持久化队列 |
| 出站投递 | Postfix smtp (C, 高度优化) | ✅ 租约式出站客户端 |
| ESMTP 8BITMIME / SMTPUTF8 | ✅ | ✅ |
| 附件处理 | Amavis 解码 | ✅ 流式写盘，内存可控 |
| Sieve 过滤 | Dovecot Pigeonhole | ❌ |
| 全文搜索 | Dovecot FTS (Solr/Flatcurve/xapian) | ❌ 可外挂实现 |

### 存储

| 能力 | Postfix 栈 | ProtoRelay |
|---|---|---|
| 默认存储 | Maildir | LocalFileProvider |
| 分布式存储 | Dovecot object storage plugin | DistributedFileProvider (多节点 + 副本) |
| 对象存储 | Dovecot S3 plugin | S3StorageProvider (MinIO/S3) / HdfsWebProvider (WebHDFS) |
| 存储与用户分片联动 | Dovecot Director 独立配置 | ✅ 存储与 DB 同 shard，Router 统一管理 |

---

## 运维对比

| 维度 | Postfix 栈 | ProtoRelay |
|---|---|---|
| 部署方式 | 包管理器 + 手动编辑 5-10 个配置文件 | 部署脚本 + JSON 配置 |
| 启动/停止/重启 | `systemctl restart postfix dovecot ...` | 单一 systemd 单元或直接运行 |
| 监控指标 | 各组件分散暴露（Postfix queue exporter 等） | 内置 `/metrics` (Prometheus) + `/status` (JSON) |
| 健康检查 | 逐组件检查 | `/health/live` + `/health/ready` |
| 配置热重载 | postfix reload / doveadm reload | POST `/reload` |
| 日志 | syslog（多组件，格式不统一） | spdlog（带级别、文件轮转） |
| 社区 / 文档 | 25 年积累，海量教程 | 自建文档 |
| 运维人员认知 | 广泛普及 | 需要学习 |

---

## 成熟度与生态

| 维度 | Postfix 栈 | ProtoRelay |
|---|---|---|
| 首次发布 | 1998 年 | 2024 年 |
| 生产验证 | 全球数十亿封邮件 | 有限 |
| 第三方集成 | milter 协议、数十个插件 | 无 |
| 客户端自动发现 | Thunderbird/Outlook 自动配置 | ❌ |
| 证书自动管理 | certbot 外部脚本 | ❌ 需手动 certbot |
| ACME 自动续签 | 无（需外部） | ❌ 需外部 |

---

## 各自适合的场景

### Postfix 栈更适合

- **大规模传统部署**：ISP、高校、企业已有多年的 Postfix 运维经验
- **需要隔离故障域**：多进程模型下，反垃圾进程崩溃不影响 MTA
- **需要 milter 生态**：自定义的邮件内容过滤/修改逻辑
- **团队已有 Postfix/Dovecot 运维能力**

### ProtoRelay 更适合

- **新项目/小团队**：一个人即可维护整个邮件系统
- **需要水平分片**：内置 shard router，无需 Dovecot Director
- **容器化/K8s 部署**：单二进制无外部依赖，天然适合容器
- **定制化场景**：自有用户体系、需要深度二次开发
- **统一监控**：一个 HTTP 端点暴露所有指标

---

## 总结

Postfix 栈是经过 25 年验证的工业标准，生态丰富但组件众多、配置分散、认知负担高。ProtoRelay 用现代 C++20/Asio 重新实现了一体化邮件系统，架构更简洁、配置更集中、水平扩展原生支持，但缺乏生产验证积累和第三方集成生态。

两者不是替代关系——Postfix 是 MTA，ProtoRelay 是邮件系统。选择取决于场景：需要 milter 生态和成熟运维体系选 Postfix 栈；需要简洁部署、快速迭代、水平扩展能力选 ProtoRelay。
