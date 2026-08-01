# 运维/测试操作手册

## 服务器信息

| 项目 | 值 |
|------|-----|
| 服务器 IP | `120.24.169.213` |
| SSH 登录 | `ssh root@120.24.169.213`（免密） |
| 项目根目录 | `/opt/smtpServer/` |
| 编译链接目录 | `/opt/imapServer/`（存放 .o 和 link.sh） |

## 服务列表

| systemd 服务名 | 二进制路径 | 配置路径 | 端口 |
|---------------|-----------|---------|------|
| `imapserver` | `/opt/smtpServer/imapsServer` | `/opt/smtpServer/config/imapsConfig.json` | 993 (SSL), 143 (TCP) |
| `smtpserver` | `/opt/smtpServer/smtpsServer` | `/opt/smtpServer/config/smtpsConfig.json` | 25, 465, 587 |

**注意**：systemd 的 WorkingDirectory 是 `/opt/smtpServer`，但 .o 文件上传和链接都在 `/opt/imapServer/` 进行。
链接完成后必须 **先 stop 服务** 再把二进制 cp 到 `/opt/smtpServer/`（否则 "Text file busy"）。

## 数据库

MySQL 裸连：`ssh root@120.24.169.213 'mysql mail'`

## 测试账号

| 邮箱 | 密码 |
|------|------|
| `qt@scut.email` | `202330552361qtest123` |
| `test@scut.email` | `qtest123` |

## Git 仓库

```
git@github.com:deepdiver-997/ProtoRelay.git
```

## 完整部署流程

### 1. 本地交叉编译

```bash
cd /Users/zhuhongrui/Desktop/code/c++/project/mail-system/v8
bash build.sh Release cross-x64
```

产物在 `artifacts/linux-x86_64/Release/obj/`。

### 2. 上传 .o 文件到服务器

```bash
scp -r artifacts/linux-x86_64/Release/obj/* root@120.24.169.213:/opt/imapServer/
```

### 3. 在服务器上链接

**IMAP 服务器：**
```bash
ssh root@120.24.169.213 "cd /opt/imapServer && bash link.sh imaps_test.cpp.o -o imapsServer --exclude smtps_test --exclude smtp_test --exclude mail_server_combined --exclude test_inbound_verifier"
```

**SMTP 服务器：**
```bash
ssh root@120.24.169.213 "cd /opt/imapServer && bash link.sh smtps_test.cpp.o -o smtpsServer --exclude imaps_test --exclude mail_server_combined --exclude test_inbound_verifier"
```

### 4. 部署二进制并重启

**IMAP：**
```bash
ssh root@120.24.169.213 "systemctl stop imapserver && cp /opt/imapServer/imapsServer /opt/smtpServer/imapsServer && systemctl start imapserver && sleep 2 && systemctl status imapserver --no-pager | head -5"
```

**SMTP：**
```bash
ssh root@120.24.169.213 "systemctl stop smtpserver && cp /opt/imapServer/smtpsServer /opt/smtpServer/smtpsServer && systemctl start smtpserver && sleep 2 && systemctl status smtpserver --no-pager | head -5"
```

### 5. 一键部署（IMAP + SMTP）

```bash
# 编译 + 上传 + 链接 + 部署
bash build.sh Release cross-x64 && \
scp -r artifacts/linux-x86_64/Release/obj/* root@120.24.169.213:/opt/imapServer/ && \
ssh root@120.24.169.213 "
  cd /opt/imapServer &&
  bash link.sh imaps_test.cpp.o -o imapsServer --exclude smtps_test --exclude smtp_test --exclude mail_server_combined --exclude test_inbound_verifier &&
  bash link.sh smtps_test.cpp.o -o smtpsServer --exclude imaps_test --exclude mail_server_combined --exclude test_inbound_verifier &&
  systemctl stop imapserver &&
  cp imapsServer /opt/smtpServer/imapsServer &&
  systemctl start imapserver &&
  systemctl stop smtpserver &&
  cp smtpsServer /opt/smtpServer/smtpsServer &&
  systemctl start smtpserver
" && \
ssh root@120.24.169.213 "systemctl status imapserver smtpserver --no-pager | grep -E '(Active|service)'"
```

## 查看日志

### IMAP 日志
```bash
# 实时跟踪
ssh root@120.24.169.213 "journalctl -u imapserver -f --no-pager"

# 最近 N 分钟
ssh root@120.24.169.213 "journalctl -u imapserver --since '5 minutes ago' --no-pager"

# 最近 200 行
ssh root@120.24.169.213 "journalctl -u imapserver -n 200 --no-pager"

# 文件日志
ssh root@120.24.169.213 "tail -200 /opt/smtpServer/../logs/imap_server.log"
```

### SMTP 日志
```bash
ssh root@120.24.169.213 "journalctl -u smtpserver -f --no-pager"
```

## 服务控制

```bash
# 重启
ssh root@120.24.169.213 "systemctl restart imapserver"
# 停止
ssh root@120.24.169.213 "systemctl stop imapserver"
# 启动
ssh root@120.24.169.213 "systemctl start imapserver"
# 查看状态
ssh root@120.24.169.213 "systemctl status imapserver --no-pager"
```

## 测试 IMAP

### 使用 openssl 快速测试
```bash
# SSL 连接 (993)
openssl s_client -connect 120.24.169.213:993 -crlf -quiet 2>/dev/null

# TCP 连接 (143)
nc 120.24.169.213 143
```

### 典型登录测试
```
A1 LOGIN test@scut.email qtest123
A2 LIST "" "*"
A3 SELECT INBOX
A4 UID FETCH 1:* (UID FLAGS INTERNALDATE RFC822.SIZE BODY[])
A5 LOGOUT
```

### 网易邮件大师测试
1. 在网易邮件大师客户端中添加账号 `test@scut.email`（IMAP 服务器 `120.24.169.213`，端口 993 SSL）
2. 先用其他邮箱发一封测试邮件到 `test@scut.email`
3. 等待测试邮件到达后，在网易客户端刷新
4. 查看服务器日志确认请求是否正常

## 注意事项

1. **部署路径陷阱**：二进制链接在 `/opt/imapServer/`，但 systemd 使用 `/opt/smtpServer/imapsServer`，必须手动 cp
2. **"Text file busy"**：直接 cp 会失败，必须先 `systemctl stop` 再 cp
3. **exclude 陷阱**：link.sh 的 `--exclude` 是子串匹配，`--exclude smtps` 会误排除所有含 "smtps" 的 .o 文件。务必使用精确模式如 `--exclude smtps_test`
4. **入口文件**：IMAP 入口是 `imaps_test.cpp.o`（因为历史原因在 test 目录），SMTP 入口是 `smtps_test.cpp.o`
5. **IMAP 日志关键字**：`[IMAP]` 前缀，含 state/event/tag 等信息
6. **数据库**：两个服务共用 MySQL，通过 db_config.json 配置
