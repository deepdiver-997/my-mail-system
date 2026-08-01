# IMAP4rev1 协议流程指南

> 本文档描述 IMAP (Internet Message Access Protocol) 的基本协议流程，
> 用于理解 MailFront 客户端与服务端 ProtoRelay IMAP 之间的交互。
> 协议标准：RFC 3501 (IMAP4rev1)。

## 目录

1. [IMAP 概述](#1-imap-概述)
2. [连接与状态机](#2-连接与状态机)
3. [认证阶段](#3-认证阶段)
4. [邮箱管理](#4-邮箱管理)
5. [邮件检索](#5-邮件检索)
6. [邮件操作](#6-邮件操作)
7. [IMAP 与 SMTP 的核心区别](#7-imap-与-smtp-的核心区别)
8. [常见调试方法](#8-常见调试方法)

---

## 1. IMAP 概述

IMAP 是一种**拉取**协议——客户端连接到远程服务器上的邮箱，浏览、
检索和管理邮件。与 SMTP（推送）不同，IMAP 的邮件始终保留在服务器上。

### 核心概念

| 概念 | 说明 |
|------|------|
| **邮箱/文件夹 (Mailbox)** | 邮件的容器，如 INBOX、Sent、Trash |
| **消息序号 (Sequence Number)** | 1-based，在当前邮箱中按时间排序的位置 |
| **UID** | 每邮箱唯一且单调递增的 ID，不随删除而变化 |
| **标记 (Flag)** | 邮件的状态标记（\Seen, \Deleted, \Flagged 等） |
| **标签 (Tag)** | 每个命令的唯一前缀，用于匹配响应 |

### 响应格式

```
* <untagged 响应>              ← 服务器主动发送
+ <继续响应>                    ← 等待 literal 数据
tag <OK/NO/BAD> <描述>         ← 命令完成响应
```

---

## 2. 连接与状态机

### 连接方式

| 方式 | 端口 | 说明 |
|------|------|------|
| 明文 IMAP | 143 | 不加密，可使用 STARTTLS 升级 |
| IMAPS (SSL) | 993 | 连接即建立 SSL/TLS |

### 协议状态

```
                       +───────────+
                       │   INIT    │  连接建立
                       +─────┬─────+
                             │ CONNECT
                             ▼
                   ┌─────────────────┐
          ┌──────▶│ NOT_AUTHENTICATED│  ← 未认证
          │       └────────┬────────┘
          │                │ LOGIN 成功
          │                ▼
          │       ┌─────────────────┐
          │       │  AUTHENTICATED  │  ← 已认证，无选中邮箱
          │       └────────┬────────┘
          │                │ SELECT / EXAMINE
          │                ▼
          │       ┌─────────────────┐
          │       │    SELECTED     │  ← 已选中一个邮箱
          │       └────────┬────────┘
          │                │ CLOSE / LOGOUT
          │                ▼
          │       ┌─────────────────┐
          └───────│    LOGOUT       │  ← 会话结束
                  └─────────────────┘
```

### 状态间允许的命令

| 命令 | NOT_AUTHENTICATED | AUTHENTICATED | SELECTED |
|------|:---:|:---:|:---:|
| CAPABILITY | ✅ | ✅ | ✅ |
| LOGIN | ✅ | — | — |
| LOGOUT | ✅ | ✅ | ✅ |
| STARTTLS | ✅ | — | — |
| SELECT / EXAMINE | — | ✅ | ✅ |
| CREATE / DELETE / RENAME | — | ✅ | ✅ |
| LIST / LSUB / STATUS | — | ✅ | ✅ |
| FETCH / STORE / SEARCH | — | — | ✅ |
| COPY / MOVE | — | — | ✅ |
| EXPUNGE / CLOSE | — | — | ✅ |
| APPEND | — | ✅ | ✅ |
| IDLE | — | ✅ | ✅ |
| NOOP | ✅ | ✅ | ✅ |

---

## 3. 认证阶段

### 完整握手示例

```
C:  <TCP 连接建立>
S:  * OK IMAP4rev1 Server Ready

C:  a1 CAPABILITY
S:  * CAPABILITY IMAP4rev1 AUTH=LOGIN STARTTLS IDLE UIDPLUS MOVE
S:  a1 OK CAPABILITY completed

C:  a2 LOGIN t1@mail.hgmail.xin 123456
S:  a2 OK LOGIN completed
```

### LOGIN 命令

```
格式:  tag LOGIN <username> <password>
响应:  tag OK LOGIN completed     (成功)
       tag NO LOGIN failed         (失败)
```

> **注意**: LOGIN 命令使用明文密码传输。生产环境建议使用 STARTTLS 或
> IMAPS 加密连接。

### STARTTLS（可选）

```
C:  a3 STARTTLS
S:  a3 OK Begin TLS negotiation now
    ← TLS 握手开始，之后所有通信加密 →
S:  * OK IMAP4rev1 Server Ready   (新会话开始)
```

---

## 4. 邮箱管理

### 列出邮箱 (LIST)

```
C:  a4 LIST "" "*"
S:  * LIST (\HasNoChildren) "/" INBOX
S:  * LIST (\HasNoChildren) "/" "收件箱"
S:  * LIST () "/" "发件箱"
S:  * LIST () "/" "垃圾箱"
S:  * LIST () "/" "已删除"
S:  * LIST () "/" "草稿箱"
S:  a4 OK LIST completed
```

> 邮箱名使用 modified UTF-7 编码（RFC 3501 §5.1.3）。
> 客户端收到后需解码为 UTF-8。

### 创建邮箱 (CREATE)

```
C:  a5 CREATE "新文件夹"
S:  a5 OK CREATE completed
```

### 删除邮箱 (DELETE)

```
C:  a6 DELETE "旧文件夹"
S:  a6 OK DELETE completed
```

### 重命名邮箱 (RENAME)

```
C:  a7 RENAME "旧名称" "新名称"
S:  a7 OK RENAME completed
```

### 选择邮箱 (SELECT)

```
C:  a8 SELECT INBOX
S:  * 3 EXISTS                ← 邮箱中有 3 封邮件
S:  * 3 RECENT                ← 其中 3 封是新的
S:  * OK [UNSEEN 1]           ← 第 1 封未读
S:  * OK [UIDVALIDITY 1]      ← UID 有效性标记
S:  * OK [UIDNEXT 4]          ← 下一个 UID 为 4
S:  * OK [READ-WRITE]         ← 可读写（EXAMINE 则为 READ-ONLY）
S:  a8 OK SELECT completed
```

---

## 5. 邮件检索

### 获取邮件列表 (FETCH)

```
C:  a9 FETCH 1:3 (FLAGS RFC822.SIZE ENVELOPE)
S:  * 1 FETCH (FLAGS (\Seen) RFC822.SIZE 1234
       ENVELOPE ("01-Jan-2025" "Hello" ...))
S:  * 2 FETCH (FLAGS (\Seen) RFC822.SIZE 5678
       ENVELOPE ("02-Jan-2025" "Re: Hello" ...))
S:  * 3 FETCH (FLAGS () RFC822.SIZE 9012
       ENVELOPE ("03-Jan-2025" "New message" ...))
S:  a9 OK FETCH completed
```

| FETCH 属性 | 说明 |
|-----------|------|
| `FLAGS` | 邮件标记 |
| `INTERNALDATE` | RFC 3501 格式的日期时间 |
| `RFC822.SIZE` | 邮件大小（字节） |
| `ENVELOPE` | 信封信息（日期、主题、发件人、收件人等） |
| `BODY[]` | 完整邮件正文（RFC 822 格式） |
| `BODY[HEADER]` | 仅邮件头部 |
| `BODY[TEXT]` | 仅邮件正文部分 |

### 获取邮件正文

```
C:  a10 FETCH 1 BODY[]
S:  * 1 FETCH (BODY[] {450}
S:  Date: 01-Jan-2025 10:00:00 +0000
S:  From: alice@example.com
S:  To: t1@mail.hgmail.xin
S:  Subject: Hello
S:  
S:  This is the message body.
S:  )
S:  a10 OK FETCH completed
```

### 搜索邮件 (SEARCH)

```
C:  a11 SEARCH UNSEEN
S:  * SEARCH 2 3            ← 序号 2 和 3 未读
S:  a11 OK SEARCH completed
```

| 搜索键 | 匹配条件 |
|--------|---------|
| `ALL` | 全部 |
| `UNSEEN` / `NEW` | 未读 |
| `SEEN` | 已读 |
| `DELETED` | 标记删除 |
| `UNDELETED` | 未标记删除 |
| `FLAGGED` | 标星 |

### UID 模式

```
C:  a12 UID FETCH 100:102 (FLAGS RFC822.SIZE)
S:  * 1 FETCH (UID 100 FLAGS (\Seen) RFC822.SIZE 1234)
S:  a12 OK UID FETCH completed
```

> UID 模式下，序号、序列集都使用 UID 而非消息序号。
> SEARCH 和 FETCH 返回的结果也包含 UID。

---

## 6. 邮件操作

### 标记已读/未读 (STORE)

```
C:  a13 STORE 1 +FLAGS (\Seen)     ← 标记为已读
S:  * 1 FETCH (FLAGS (\Seen))
S:  a13 OK STORE completed

C:  a14 STORE 1 -FLAGS (\Seen)     ← 取消已读标记
S:  * 1 FETCH (FLAGS ())
S:  a14 OK STORE completed
```

### 标记删除

```
C:  a15 STORE 1 +FLAGS (\Deleted)
S:  * 1 FETCH (FLAGS (\Seen \Deleted))
S:  a15 OK STORE completed
```

### 彻底删除 (EXPUNGE)

```
C:  a16 EXPUNGE
S:  * 1 EXPUNGE               ← 序号 1 被删除
S:  a16 OK EXPUNGE completed
```

> CLOSE 等效于 EXPUNGE + 关闭邮箱，回到 AUTHENTICATED 状态。

### 复制/移动邮件 (COPY / MOVE)

```
C:  a17 COPY 1:2 "发件箱"
S:  a17 OK COPY completed (2 messages)

C:  a18 MOVE 3 "垃圾箱"
S:  a18 OK MOVE completed (1 messages)
```

> MOVE = COPY + 源标记 \Deleted

### 追加邮件 (APPEND)

```
C:  a19 APPEND INBOX (\Seen) {123}
S:  + Ready for literal data
C:  Date: 05-Jan-2025 12:00:00 +0000
C:  From: me@example.com
C:  To: me@example.com
C:  Subject: Archived message
C:  
C:  This message is being appended.
S:  a19 OK [APPENDUID 1 2056301874306351104] APPEND completed
```

> APPEND 常用于存草稿或归档。响应中的 APPENDUID 反馈了
> UIDVALIDITY 和分配的 mail_id。

### IDLE 命令（实时等待）

```
C:  a20 IDLE
S:  + idling
     ← 连接保持，服务器可推送新邮件通知 →
C:  DONE
S:  a20 OK IDLE terminated
```

> IDLE 允许客户端在等待新邮件时保持连接而不需要轮询。
> 退出 IDLE 后客户端应发送 NOOP 或重新 SELECT 来刷新状态。

---

## 7. IMAP 与 SMTP 的核心区别

| 维度 | IMAP | SMTP |
|------|------|------|
| **方向** | 拉取（服务器 → 客户端） | 推送（客户端 → 服务器） |
| **邮件位置** | 始终在服务器 | 投递后即完成 |
| **状态** | 有状态（选中邮箱、标记等） | 无状态（每个交易独立） |
| **认证时机** | 连接后先登录 | 邮件发送前认证 |
| **连接时长** | 长连接（可 IDLE 等通知） | 短连接（发完即断） |
| **命令模型** | 标签式异步 | 顺序请求-响应 |
| **加密升级** | STARTTLS (RFC 2595) | STARTTLS (RFC 3207) |
| **端口** | 143/993 | 25/587/465 |

### 对应关系（同一"邮件"的客户端视角）

```
发件流程（SMTP）                   收件流程（IMAP）
┌────────────┐                    ┌────────────┐
│ 写邮件内容 │                    │ LOGIN 认证  │
│ 发送(SMTP) │                    │ LIST 文件夹  │
│ 存到 Sent  │                    │ SELECT INBOX│
│ (APPEND)   │                    │ FETCH 邮件   │
└────────────┘                    │ 标记已读     │
                                  │ (STORE)     │
                                  └────────────┘
```

---

## 8. 常见调试方法

### 使用 nc 手动测试

```bash
# 连接 IMAP 明文端口
printf "a1 LOGIN t1@mail.hgmail.xin 123456\r\na2 LIST \"\" \"*\"\r\na3 LOGOUT\r\n" | nc localhost 143
```

### 使用 openssl 测试 IMAPS

```bash
openssl s_client -connect localhost:993 -crlf -quiet
# 然后输入 IMAP 命令
```

### 使用 openssl 测试 STARTTLS

```bash
openssl s_client -connect localhost:143 -starttls imap -crlf -quiet
```

### 使用 Python 脚本测试

```python
import socket, time
s = socket.socket()
s.connect(('localhost', 143))
print(s.recv(4096))           # greeting
s.send(b'a1 LOGIN user pass\r\n')
time.sleep(0.3)
print(s.recv(4096))           # login result
s.send(b'a2 LIST "" "*"\r\n')
time.sleep(0.5)
print(s.recv(8192))           # folder list
s.close()
```

### 客户端测试工具

本项目提供了 `test_imap` 命令行工具（位于 MailFront 项目）：

```bash
cd Qt/mail_front
cmake --build build --target test_imap
./build/test_imap
```

该工具自动完成连接、认证、SELECT INBOX 和 LIST 全部文件夹，
并打印详细的调试输出。

---

> 文档版本: v1.0 · 对应 ProtoRelay IMAP 服务器 Phase 2 实现
