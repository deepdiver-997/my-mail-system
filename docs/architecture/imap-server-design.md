# IMAP4rev1 Server Implementation

## Overview

The IMAP4rev1 (RFC 3501) server is a new protocol handler added alongside the existing SMTP server.
It shares the same infrastructure — `ServerBase`, `SessionBase`, `IConnection`/`TcpConnection`/`SslConnection`,
database connection pool, storage provider, and logger — while implementing a parallel FSM-based
protocol state machine.

```
┌──────────────────────────────────────────────────┐
│                  ServerBase                      │
│ (acceptor loop, SSL/TCP, metrics, connection mgt)│
├─────────────────┬────────────────────────────────┤
│  SmtpsServer    │       ImapsServer              │
│  (SMTP FSM)     │       (IMAP FSM)               │
└─────────────────┴────────────────────────────────┘
```

## Architecture

### File Layout

```
include/mail_system/back/mailServer/
├── fsm/imaps/
│   ├── imaps_fsm.hpp               ← ImapState/ImapEvent enums, ImapContext, base class + DB helpers
│   ├── traditional_imaps_fsm.h     ← TraditionalImapsFsm declaration
│   └── traditional_imaps_fsm.tpp   ← Transition table + all command handlers
├── session/
│   ├── imaps_session.h             ← ImapsSession declaration
│   └── imaps_session.tpp           ← IMAP command parser, literal handling
├── imaps_server.h                  ← ImapsServer declaration
src/mail_system/back/mailServer/
└── imaps/imaps_server.cpp          ← Accept loop, session creation
```

### State Machine

IMAP protocol states (RFC 3501 §3):

```
INIT ──(CONNECT)──▶ NOT_AUTHENTICATED
                        │
                   (LOGIN success)
                        │
                        ▼
                  AUTHENTICATED
                        │
                   (SELECT/EXAMINE)
                        │
                        ▼
                    SELECTED
                        │
                   (CLOSE / LOGOUT)
                        ▼
                  AUTHENTICATED  or  LOGOUT
```

### Supported Commands (Phase 1)

| Command | Status | RFC Reference |
|---------|--------|--------------|
| CAPABILITY | ✅ Complete | RFC 3501 §6.1.1 |
| LOGIN | ✅ Complete | RFC 3501 §6.2.3 |
| LOGOUT | ✅ Complete | RFC 3501 §6.1.5 |
| SELECT | ✅ Complete | RFC 3501 §6.3.1 |
| EXAMINE | ✅ Complete | RFC 3501 §6.3.2 |
| LIST | ✅ Complete | RFC 3501 §6.3.8 |
| LSUB | ✅ Complete | RFC 3501 §6.3.9 |
| STATUS | ✅ Complete | RFC 3501 §6.3.10 |
| FETCH | ✅ Complete | RFC 3501 §6.4.5 |
| STORE | ✅ Complete | RFC 3501 §6.4.6 |
| EXPUNGE | ✅ Complete | RFC 3501 §6.4.3 |
| CLOSE | ✅ Complete | RFC 3501 §6.4.2 |
| SEARCH | ✅ 支持常用过滤 | RFC 3501 §6.4.4 |
| UID | ✅ Basic | RFC 3501 §6.4.8 |
| NOOP | ✅ Complete | RFC 3501 §6.1.2 |
| CHECK | ✅ Complete | RFC 3501 §6.4.1 |
| CREATE | ✅ Complete | RFC 3501 §6.3.3 |
| DELETE | ✅ Complete | RFC 3501 §6.3.4 |
| RENAME | ✅ Complete | RFC 3501 §6.3.5 |
| SUBSCRIBE | ✅ Stub | RFC 3501 §6.3.6 |
| UNSUBSCRIBE | ✅ Stub | RFC 3501 §6.3.7 |
| IDLE | ✅ 可进出，无推送 | RFC 2177 |
| STARTTLS | ✅ 已实现 | RFC 3501 §6.2.1 |
| APPEND | ✅ 完整实现 | RFC 3501 §6.3.11 |
| COPY | ✅ 完整实现 | RFC 3501 §6.4.7 |
| MOVE | ✅ 完整实现 | RFC 6851 |

### FETCH Attributes Supported

- `FLAGS` — maps \Seen (mail_recipients.status), \Flagged (is_starred), \Deleted (is_deleted)
- `INTERNALDATE` — send_time formatted per RFC 3501 date-time
- `RFC822.SIZE` — mail body file size
- `ENVELOPE` — date, subject, from, sender, reply-to, to, cc, bcc, in-reply-to, message-id
- `BODY[]` — full message body (read from storage)

## Database Schema Mapping

The IMAP server uses the existing database schema **without any modifications**:

```
users ──┬── mailboxes (INBOX, Sent, Trash, etc.)
         │      │
         │      └── mail_mailbox (mail ↔ mailbox mapping + flags)
         │
         └── mail_recipients (mail ↔ recipient, includes read/unread status)
                 │
                 └── mails (subject, body_path, send_time)
```

### IMAP Flag Mapping

| IMAP Flag | Database Field |
|-----------|---------------|
| `\Seen` | `mail_recipients.status` = 0 (read) / 1 (unread) |
| `\Flagged` | `mail_mailbox.is_starred` = 1 |
| `\Deleted` | `mail_mailbox.is_deleted` = 1 |
| `\Draft` | `mail_recipients.status` = 3 |
| `\Answered` | Not yet mapped (future) |
| `\Recent` | Computed: EXISTS - UNSEEN |

### UID Assignment

UID is the Snowflake `mail_id` (globally unique, monotonically increasing).
`UIDVALIDITY` = `mailbox_id`. `UIDNEXT` = `MAX(mail_id) + 1` in the mailbox.

## Configuration

### IMAP listener configuration (in smtpsConfig.json)

IMAP 监听器通过主配置文件中的 `listeners` 数组配置：

```json
{
  "listeners": [
    { "type": "imap_ssl",  "port": 993 },
    { "type": "imap_tcp",  "port": 143 }
  ],
  "enable_tcp": true,
  "tcp_port": 143,
  "certFile": "crt/server.crt",
  "keyFile": "crt/server.key",
  "use_database": true,
  "db_config_file": "config/db_config.json",
  "system_domain": "mail.example.com",
  "mail_storage_path": "../mail/",
  "attachment_storage_path": "../attachments/",
  "log_file": "../logs/imap_server.log"
}
```

Key differences from SMTP config:
- **No outbound/DKIM fields** — IMAP doesn't deliver mail
- **No persistent queue settings** — IMAP reads, doesn't write through the pipeline
- **No inbound auth policy** — IMAP always requires authentication
- **Longer timeouts** — IMAP sessions last longer (30 min idle vs 5 min SMTP)

## Integration

IMAP 和 SMTP 共用同一个配置文件 `config/smtpsConfig.json`，通过 `listeners` 中的 `type` 区分协议。

```cpp
#include "mail_system/back/mailServer/imaps_server.h"

// 与 SMTP server 共用 config
ServerConfig cfg;
cfg.loadFromFile("config/smtpsConfig.json");

// Share the same thread pools & DB pool for efficiency
auto imap_server = std::make_shared<ImapsServer>(
    imap_cfg,
    io_pool,     // shared IO thread pool
    work_pool,   // shared worker thread pool
    db_pool      // shared DB pool
);
imap_server->start();
```

### Testing with openssl

```bash
# IMAPS (SSL, port 993)
openssl s_client -connect localhost:993 -crlf -quiet

# IMAP (STARTTLS, port 143)
openssl s_client -connect localhost:143 -starttls imap -crlf -quiet
```

Once connected, try:

```
. LOGIN user@domain.com password
. LIST "" "*"
. SELECT INBOX
. FETCH 1:* (FLAGS INTERNALDATE RFC822.SIZE ENVELOPE)
. FETCH 1 BODY[]
. STORE 1 +FLAGS (\Seen)
. LOGOUT
```

## Known Limitations (Phase 2)

1. **IDLE 推送** — 能进出 IDLE，但不会主动推送新邮件 EXISTS 通知（需后台轮询机制）
2. **BODYSTRUCTURE** — 未实现，仅支持 BODY[]
3. **INTERNALDATE** — 目前使用 mails.send_time；建议在 Date header 可用时优先使用
4. **UID** — 直接使用 mail_id (Snowflake)；未来可改为每邮箱自增 UID
5. **SEARCH** — 支持 UNSEEN / SEEN / DELETED 过滤，尚未支持 SUBJECT/FROM 等文本匹配
6. **ACL / QUOTA** — 未实现
7. **SORT / THREAD** — 未实现
8. **LIST-EXTENDED** — 未实现

## Future Roadmap

- **Phase 3**: IDLE 推送通知, BODYSTRUCTURE, ACL, QUOTA
- **Phase 4**: SORT, THREAD, LIST-EXTENDED, MULTISEARCH, full-text SEARCH
