# 运维/测试操作手册

## 服务器信息

| 项目 | 值 |
|------|-----|
| 服务器 IP | `<SERVER_IP>` |
| SSH 登录 | `ssh root@<SERVER_IP>`（免密） |
| 项目根目录 | `/opt/smtpServer/` |
| 对象树 | `/opt/smtpServer/obj/`（当前）、`obj.old/`（上一版） |

## 服务列表

| systemd 服务名 | 二进制路径 | 配置路径 | 端口 |
|---------------|-----------|---------|------|
| `imapserver` | `/opt/smtpServer/imapsServer` | `/opt/smtpServer/config/imapsConfig.json` | 993 (SSL), 143 (TCP) |
| `smtpserver` | `/opt/smtpServer/smtpsServer` | `/opt/smtpServer/config/smtpsConfig.json` | 25, 465, 587 |

## 数据库

MySQL 裸连：`ssh root@<SERVER_IP> 'mysql mail'`

## 测试账号

| 邮箱 | 密码 |
|------|------|
| `qt@<DOMAIN>` | `<PASSWORD>` |
| `test@<DOMAIN>` | `<PASSWORD>` |

## Git 仓库

```
git@github.com:deepdiver-997/ProtoRelay.git
```

## 部署（deploy.sh 一键，推荐）

部署入口是仓库根目录的 `deploy.sh`（详见 `cross-compile-guide.md` 的交叉编译部分）。
它已内置：交叉编译 → rsync 增量上传到 `obj.new/` → 服务器链接成 `*.new` → 产物校验 →
自动备份 → 原子替换 → 重启 → 冒烟测试 → 失败自动回滚。

```bash
# 空跑：构建/上传/链接/校验全跑，但不替换线上二进制
DEPLOY_SERVER=root@<SERVER_IP> ./deploy.sh clean --dry-run

# 完整部署（发布一律带 clean，避免 make 同秒时间戳跳过重编）
DEPLOY_SERVER=root@<SERVER_IP> ./deploy.sh clean
```

**发布务必先 `--dry-run` 再完整部署。** 空跑第一次运行就曾抓到一个会把旧代码链接进二进制的
路径 bug（入口 `.o` 少一层目录 + `find` 模糊匹配命中历史对象树）。

### 手工回滚

```bash
ssh root@<SERVER_IP> "cd /opt/smtpServer && \
  cp -p \$(ls -1t smtpsServer.bak-* | head -1) smtpsServer && \
  cp -p \$(ls -1t imapsServer.bak-* | head -1) imapsServer && \
  systemctl restart smtpserver imapserver"
```

备份自动保留最近 5 份（只回收 `deploy.sh` 生成的 `.bak-<日期>-<时刻>`，手工打标签的
如 `.bak-tls13-...` 不会被误删）。

## 查看日志

### IMAP 日志
```bash
# 实时跟踪
ssh root@<SERVER_IP> "journalctl -u imapserver -f --no-pager"

# 最近 N 分钟
ssh root@<SERVER_IP> "journalctl -u imapserver --since '5 minutes ago' --no-pager"

# 最近 200 行
ssh root@<SERVER_IP> "journalctl -u imapserver -n 200 --no-pager"

# 文件日志
ssh root@<SERVER_IP> "tail -200 /opt/smtpServer/../logs/imap_server.log"
```

### SMTP 日志
```bash
ssh root@<SERVER_IP> "journalctl -u smtpserver -f --no-pager"
```

## 服务控制

```bash
# 重启
ssh root@<SERVER_IP> "systemctl restart imapserver"
# 停止
ssh root@<SERVER_IP> "systemctl stop imapserver"
# 启动
ssh root@<SERVER_IP> "systemctl start imapserver"
# 查看状态
ssh root@<SERVER_IP> "systemctl status imapserver --no-pager"
```

## 测试 IMAP

### 使用 openssl 快速测试
```bash
# SSL 连接 (993)
openssl s_client -connect <SERVER_IP>:993 -crlf -quiet 2>/dev/null

# TCP 连接 (143)
nc <SERVER_IP> 143
```

### 典型登录测试
```
A1 LOGIN test@<DOMAIN> <PASSWORD>
A2 LIST "" "*"
A3 SELECT INBOX
A4 UID FETCH 1:* (UID FLAGS INTERNALDATE RFC822.SIZE BODY[])
A5 LOGOUT
```

### 网易邮件大师测试
1. 在网易邮件大师客户端中添加账号 `test@<DOMAIN>`（IMAP 服务器 `<SERVER_IP>`，端口 993 SSL）
2. 先用其他邮箱发一封测试邮件到 `test@<DOMAIN>`
3. 等待测试邮件到达后，在网易客户端刷新
4. 查看服务器日志确认请求是否正常

## 注意事项

1. **发布带 `clean`**：make 判断重编要求依赖「严格新于」目标，同秒修改会被静默跳过（曾因此拿到陈旧二进制）。
2. **`link.sh` 要显式 `--obj-root`**：不传会收集整个目录下所有 `.o`，服务器上若同时存在 `obj/` 与历史 `build-obj/` 会混链出重复符号。deploy.sh 已内置。
3. **config/ 同步不能加 `--delete`**：会删掉服务器侧独有的 `dkim/*.private.pem`（DKIM 私钥）、`crt/*` 等。只有 `obj.new/`（纯构建产物）可以用 `--delete`。
4. **入口文件**：IMAP 入口是 `test/server/imaps_test.cpp.o`，SMTP 是 `test/server/smtps_test.cpp.o`（注意 `server/` 这一层）。
5. **`--exclude` 是子串匹配**：`--exclude smtps` 会误排除所有含 "smtps" 的 .o。用 `smtps_test` / `imaps_test` 这种精确些的模式。
6. **数据库**：两个服务共用 MySQL，通过 db_config.json 配置。
