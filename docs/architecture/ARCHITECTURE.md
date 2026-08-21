# ProtoRelay Architecture

## 1. Overview

ProtoRelay is a C++17 multi-protocol mail server focused on reliable mail ingest, local persistence,
asynchronous outbound delivery, and remote mailbox access.

**Supported protocols:**
- **SMTP/SMTPS** (RFC 5321) — inbound mail receipt + outbound delivery
- **IMAP4rev1** (RFC 3501) — remote mailbox access (reading, searching, managing folders)

Current implementation status:

- Inbound (SMTP): protocol parsing, state machine, MIME/attachment handling.
- Inbound (IMAP): protocol state machine, folder listing, mail retrieval, flag management.
- Persistence: local filesystem, distributed filesystem roots, optional WebHDFS provider.
- Outbound (SMTP): queue-driven delivery worker with retry/backoff and lease-style claiming.
- Operations: service-manager-first deployment (systemd/launchd), configurable log sinks.

## 2. Design Principles

- Foreground process first: let systemd/launchd/Docker supervise lifecycle.
- Async I/O + bounded workers: separate network loop and business execution.
- Interface-driven storage and DB access: swap implementations with limited blast radius.
- Fail-fast configuration: invalid or unsupported runtime options are rejected at startup.
- Practical operability: explicit build features, clear runtime diagnostics, and flexible logging.

## 3. Layered Architecture

```text
+--------------------------------------------------------------+
| Application Entry                                             |
| test/server/smtps_test.cpp → build/smtpsServer                |
+-------------------------------+------------------------------+
                                |
+-------------------------------v------------------------------+
| Business Layer (mail_system/back/)                            |
| smtp/         SMTP FSM + session + server                    |
| imap/         IMAP FSM + session + server                    |
| outbound/     SMTP outbound delivery client                  |
| persist_storage/  Inbound persistence queue                   |
| router/       Shard routing strategies                       |
| storage/      Storage abstraction (read/write streams, local/S3/HDFS) — see storage-abstraction.md |
| db/           DB connection pool + SQL builders              |
| entities/     mail, usr data structs                          |
+-------------------------------+------------------------------+
                                |
+-------------------------------v------------------------------+
| Framework (include/framework/)                                |
| SessionBase   — async I/O, command buffering, lifecycle      |
| FsmBase       — transition table, dispatch, virtual hooks    |
| ServerBase    — config, metrics, intrusion, lifecycle        |
| TcpServerBase — TCP/SSL acceptors, listener thread            |
| Connection    — IConnection, TcpConnection, SslConnection    |
| ThreadPool    — ThreadPoolBase, IOThreadPool, BoostThreadPool|
+-------------------------------+------------------------------+
                                |
+-------------------------------v------------------------------+
| Platform                                                        |
| Boost.Asio, OpenSSL, MySQL client, spdlog, nlohmann/json       |
+--------------------------------------------------------------+
```

### 3.1 Framework vs Business

The `include/framework/` directory contains **transport-and-protocol-agnostic** infrastructure:
- `SessionBase<ConnectionType>` — manages one TCP/SSL connection lifecycle, async read/write, command buffering
- `FsmBase<ConnectionType, State, Event>` — transition table + handler registry + dispatch loop
- `ServerBase` — config loading, metrics, intrusion detection, lifecycle (start/stop/run)
- `TcpServerBase<TcpSession, SslSession>` — TCP/SSL acceptor loop, STARTTLS handoff

Business code in `include/mail_system/back/` inherits from these:
- `SmtpsSession` / `ImapsSession` : public `SessionBase`
- `TraditionalSmtpsFsm` / `TraditionalImapsFsm` : public `FsmBase`
- `SmtpsServer` / `ImapsServer` : public `TcpServerBase`

See [docs/framework-refactor.md](framework-refactor.md) for the extraction history.

## 4. Core Components

## 4.1 Inbound SMTP/SMTPS

Responsibilities:

- Accept SSL and/or TCP listeners based on config.
- Drive SMTP command state transitions.
- Parse headers/body, including MIME multipart attachments.
- Persist mail metadata/content and enqueue outbound tasks.

Key points:

- Connection/session abstractions isolate transport details.
- Session logic is built around async callbacks and strict ownership rules.
- Validation and protocol responses are generated with SMTP utility helpers.

**Multi-listener architecture:** `TcpServerBase` manages vectors of TCP and SSL acceptors, each bound to a listener config that specifies per-port security policy. IP ban checks run at accept time (before session creation), reducing FSM complexity. Accepted sockets are bound to `IOThreadPool` io_contexts for load distribution.

| Port | Type | STARTTLS | AUTH policy | SPF/DKIM/DMARC | Use case |
|------|------|----------|-------------|----------------|----------|
| 25 | TCP | yes | off | hard | Server-to-server delivery |
| 465 | SSL (implicit TLS) | no | on | off | Client submission |
| 587 | TCP | yes | on | off | Client submission (STARTTLS) |

### 4.1.1 Inbound Sender Verification (SPF/DKIM/DMARC)

At `MAIL FROM` and `DATA_END` stages, the inbound path performs sender identity verification:

- **SPF** (Sender Policy Framework): checks at `MAIL FROM` whether the connecting IP is authorized by the envelope sender's domain. On hard-fail (`-all`), the session rejects with `550 5.7.1` before accepting the message body.
- **DKIM** (DomainKeys Identified Mail): at `DATA_END`, verifies the DKIM signature in the message headers against the signer's public key retrieved via DNS TXT.
- **DMARC** (Domain-based Message Authentication, Reporting and Conformance): at `DATA_END`, evaluates DKIM and SPF alignment against the `From` header domain and applies the domain's published policy.

The verification result is injected into the message as an `Authentication-Results` header (RFC 8601).

**SPF-at-MAIL-FROM optimization**: to enable early rejection without blocking the IO thread, the MAIL FROM stage performs a quick SPF check using a DNS TXT cache. If the cache is hot, verification completes inline; a miss falls through and a full verification (including DKIM/DMARC) runs on a worker thread at `DATA_END`, reusing the SPF result if already computed.

**DNS TXT caching**: DKIM and SPF lookups use an in-memory cache with a fixed 300-second TTL. Cache entries are keyed by domain, and cache hits avoid repeated DNS round trips.

**Per-check mode configuration**: each check (SPF/DKIM/DMARC) supports three modes:
- `off`: skip this check entirely.
- `soft`: perform the check and record the result in `Authentication-Results`, but do not reject.
- `hard`: perform the check and reject the message on failure.

### 4.1.2 SMTP AUTH and EHLO Verification

The server supports three AUTH policies via `inbound_auth_policy`:

- `off` (default): never require AUTH — pure relay/MTA mode. MAIL FROM is accepted without authentication.
- `on`: always require AUTH — pure MSA (Mail Submission Agent) mode. Clients must authenticate with AUTH LOGIN/PLAIN before sending.
- `auto`: hybrid mode. Use PTR (reverse DNS) lookup on the connecting IP to identify trusted servers:
  - PTR record matches the EHLO domain → skip AUTH (server-to-server traffic)
  - No PTR match → require AUTH (client submission)

**EHLO verification flow** (auto mode): at MAIL FROM, the server performs a cached PTR lookup on the connecting IP. If any returned hostname matches the EHLO domain (exact or suffix), the connection is marked as trusted and AUTH is skipped. The PTR cache shares the 300-second TTL with other DNS caches.

**AUTH implementation**: supports `AUTH LOGIN` (base64-encoded username/password). Credentials are verified against the `users` table. On success, `last_login_time` is updated. The `status` column allows disabling accounts without deletion.

## 4.2 Outbound Delivery Pipeline

The outbound engine uses the same FsmBase + SessionBase pattern as inbound:

- `OutboundSmtpFsm` — 12-state SMTP client state machine (INIT → CONNECT → EHLO → MAIL → RCPT → DATA → ... → CLOSED)
- `OutboundSmtpSession` — manages one MX TCP connection, pipeline delivery with in_callback queue
- `OutboundServer` — MX connection pool, event-driven watermark-based task pulling
- `OutboxRepository` — stateless DB access for outbox table

### 4.2.1 Event-Driven Pull with Watermark Pre-allocation

The previous design used a `poll_loop()` that permanently occupied a worker thread,
polling on a 30-second interval. The new design is fully event-driven:

```
try_pull() 入口:
  1. pending_count >= LOW_WATERMARK(16)? → return (快速路径, 无CAS开销)
  2. CAS(is_pulling_) → 失败则return (已有一个拉取周期在进行)
  3. pending_count += BATCH_SIZE(32)     (预占水位, 防止重复拉取)
  4. do_claim_batch() → worker池

on_claim_complete(records):
  5. pending_count -= (BATCH_SIZE - records.size())  (修正预占)
  6. records为空? → schedule_retry (5s→10s→20s→...→60s 指数退避)
  7. 加载邮件 → 分发到MX session
  8. pending < LOW_WATERMARK? → 链式续拉(不释放is_pulling_)
     else → is_pulling_ = false         (水位够了, 释放拉取周期)
```

Triggers (all call `try_pull()` as lightweight probe):
- **completion_cb**: fires on each successful/failed delivery
- **submit()**: fires when new mail arrives locally
- **retry timer**: fires after exponential backoff when DB is empty

Idle MX connection eviction runs on an independent 30-second `steady_timer`.

### 4.2.2 MX Connection Pool

- Up to 4 concurrent sessions per MX domain (`DEFAULT_MAX_SESSIONS_PER_MX`)
- Session selection: prefer shortest queue (load-aware routing)
- Idle eviction: 120 seconds of inactivity with no queued/active tasks
- Global cap: 256 MX entries (`DEFAULT_MAX_MX_ENTRIES`), LRU eviction on overflow
- Connection reuse: up to 100 mails per TCP connection, RSET between mails

### 4.2.3 Related Components

- **Bloom dedup**: lock-free 1024-bit filter replaces `mutex + unordered_map` for inbound duplicate detection
- **PersistentQueue** persists mail metadata + outbox records in one transaction, then submits `MailDeliveryTask` to `OutboundServer`

Configuration (via `outbound` section in smtpsConfig.json):

- `outbound.max_attempts`
- `outbound.ports`
- `outbound.helo_domain` / `mail_from_domain` / `rewrite_header_from`
- `outbound.dkim.enabled` / `selector` / `domain` / `private_key_file`

## 4.3 Storage Provider Abstraction

Runtime selectable via `storage.provider`:

- `local`: standard local directories.
- `null`: no-op, discards all writes. Used for ceiling benchmarks.
- `s3`: S3/MinIO object storage via HTTP PUT/GET/DELETE + AWS Signature V4 (libcurl).
- `distributed`: multiple configured roots with replica count.
- `hdfs_web`: WebHDFS-backed provider (libcurl).

Build-time gating:

- CMake option: `ENABLE_HDFS_WEB_STORAGE` / `ENABLE_S3_STORAGE`
- ON: compile/link the provider and libcurl.
- OFF: selecting `hdfs_web` / `s3` at runtime fails fast.

## 4.4 Database Access

### 4.4.1 Connection Model

- **`IDBConnection`**: abstract interface — `query()`, `execute()`, `begin_transaction()`, `commit()`, `rollback()`, `escape_string()`
- **`DBPool::acquire_connection()`** → `ScopedConnection` (RAII): auto-returns on destruction
- **`IShardRouter`**: routes `email → shard_index`, each shard has independent `DBPool`
- **`OutboxRepository`**: stateless utility; receives `DBPool&` at call time, never holds a pool

### 4.4.2 Async Database Interface (db_service.h)

`IDBConnection` provides async variants of all data operations with default
synchronous implementations:

```
// 异步查询（默认同步执行 + 同步回调，子类可重写为非阻塞）
virtual void async_query(sql, params, QueryCallback);    // → shared_ptr<IDBResult>
virtual void async_execute(sql, params, ExecuteCallback); // → bool
virtual void async_begin_transaction(ExecuteCallback);     // → bool
virtual void async_commit(ExecuteCallback);                // → bool
virtual void async_rollback(ExecuteCallback);              // → bool
```

Default behavior: callback fires before `async_xxx` returns (= synchronous).
Subclasses (e.g., a future non-blocking MySQL implementation) override these
to post work to a thread pool or integrate with OS async I/O.

**Callback types** (in `db_service.h`):
```cpp
using QueryCallback   = std::function<void(std::shared_ptr<IDBResult>)>;
using ExecuteCallback = std::function<void(bool success)>;
```

### 4.4.3 Async Adoption Strategy

1. **Leaf-level**: FSMs call `conn->async_query/execute()` via `sq()`/`se()` sync-bridge helpers (callbacks fire immediately with default impl, ready for future non-blocking override).
2. **Transaction flow**: `PersistentQueue::persist_mail_transactional_async()` chains async callbacks through `begin → insert_metadata → insert_attachments → enqueue_outbox → commit`. `shared_ptr<ScopedConnection>` keeps the connection alive across the callback chain.
3. **FSM handlers**: use the **paused pipeline** pattern (see §5.2) to initiate async DB and return without blocking the IO thread.

## 4.5 Logging Subsystem

Built on spdlog with module-oriented loggers (15 modules × 6 levels = 90 macros).

**Log compression toolchain** (see [log-transform-toolchain.md](log-transform-toolchain.md)):
build-time transformation replaces `LOG_*` macros with `LOG_PURE(hash, args, ts)`,
outputting compressed `hash|arg|ts` lines. A post-processing restore tool reconstructs
readable logs from an incremental mapping table.

Runtime controls:

- `log_level`
- `log_to_console`
- `log_to_file`
- `log_file`

Sink strategy:

- Console sink for supervisor capture (journald/launchd stdout/stderr).
- Rotating file sink for standalone or file-centric environments.
- If both sinks are disabled by mistake, logger falls back to console sink.

Recommended production profile under systemd:

- `log_to_console=true`
- `log_to_file=false`

## 4.6 Intrusion Detection

Per-IP failed-authentication tracking with automatic ban and lazy disk persistence.

**Tracking:** each session's authentication outcome is reported to `IntrusionDetector` at session close. SMTP/IMAP session destructors sync the FSM context's `is_authenticated` flag into the base class before `SessionBase::close()` records the result. Private/local IPs (127.0.0.1, 10.x, 192.168.x, 172.16-31.x) are skipped.

**Ban check:** at `MAIL FROM` stage, before the AUTH policy check, the IP is tested against the ban threshold. Banned IPs receive `550 5.7.1 Too many authentication failures, access denied temporarily` and the session is closed.

**Memory bound:** LRU eviction caps the in-memory record count at `intrusion_max_records` (default 10000). When exceeded, the least-recently-seen entry is evicted.

**Lazy persistence:** instead of a timer thread, `record_session()` increments a dirty counter. When the counter exceeds `intrusion_persist_dirty_threshold` (default 256), the current time is checked. If more than `intrusion_persist_interval_sec` (default 60s) has elapsed since the last flush, the full record set is written to JSON synchronously under the lock. Additionally, a full flush is triggered on graceful server shutdown.

**Config keys:**

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `intrusion_detection_enabled` | bool | false | Enable IP failure tracking |
| `intrusion_ban_threshold` | int | 0 | Failed count to trigger ban (0=disabled) |
| `intrusion_max_records` | int | 10000 | Max IP records before LRU eviction |
| `intrusion_persist_interval_sec` | int | 60 | Min seconds between lazy flushes |
| `intrusion_persist_dirty_threshold` | int | 256 | Record count to trigger flush check |

## 4.7 Registration Service

A companion FastAPI service (`register-service/`) provides invite-code-based account registration with automatic expiry and cleanup.

**Features:**
- Invite codes with per-code usage limits and account expiry (default 90 days)
- Sequential email assignment: `invitor_N@<domain>`
- Rate limiting per IP (configurable)
- Password hashing via C++ `hash_tool` (matching the server's bcrypt implementation)
- Cleanup script (`cleanup.py`) removes expired accounts and all associated data (mails, attachments, mailboxes, outbox entries) from both the database and filesystem

**API endpoints:**

| Method | Path | Description |
|--------|------|-------------|
| GET | `/stats/{code}` | Query remaining uses for an invite code |
| POST | `/register` | Register with `{"invite_code":"...", "password":"..."}` |

**Deployment:** runs as a separate systemd unit (`protorelay-register`) listening on `127.0.0.1:8080`. External access through nginx reverse proxy with TLS.

## 5. Concurrency Model

### 5.1 Thread Pools

- **IO threads** (`IOThreadPool`): round-robin socket dispatch. Handle `async_read`/`async_write` callbacks. Should never block — heavy work must be delegated.
- **Worker threads** (`BoostThreadPool`): CPU-intensive or blocking tasks (DB queries, file I/O, DNS resolution, MIME parsing). `PersistentQueue` and `OutboundServer` post work here.
- **Listener thread**: dedicated thread for TCP/SSL acceptors.

### 5.2 Paused Pipeline Gate (Async DB from IO Thread)

FSM handlers run on the IO thread. To avoid blocking IO while waiting for
a database response, handlers use a `paused` flag on `SessionBase`:

```
handle_read 回调:
  while (has_buffered_input() && !is_paused()) {  // ← paused 时停止消费
      handle_read(extract_one_line());
      process_read();
  }

FSM handler (e.g., AUTH):
  1. session->set_paused(true)          // 闸门关闭
  2. conn->async_query(sql, callback)   // 发起异步DB
  3. return                              // IO线程继续处理其他session

async回调 (当前同步默认, 在async_query返回前触发):
  4. send_response()
  5. session->drain_buffered_commands()  // 清paused + 排空积累的命令
```

**Critical invariant**: any code path that calls `set_paused(true)` MUST call
`drain_buffered_commands()` in its callback. Failing to do so leaves the session
permanently stalled — `do_async_read` returns immediately because the buffer has
data, but the while-loop refuses to consume it.

### 5.3 Outbound Delivery

- **Event-driven**: no permanent worker thread. `try_pull()` is triggered by delivery completion and new mail submission.
- **CAS single-puller**: `compare_exchange_strong` on `is_pulling_` ensures only one thread executes DB claim at a time.
- **Watermark pre-allocation**: adds `BATCH_SIZE` to `pending_count_` before DB claim, corrects afterwards. Prevents redundant `try_pull()` calls during the DB round-trip.
- **Retry backoff**: when `claim_batch` returns empty, an exponential backoff timer (5s→60s) on the IO context retries without releasing `is_pulling_`.

### 5.4 PersistentQueue

- **Lock-free queue**: `boost::lockfree::queue<capacity<16384>>` for multi-producer (SMTP sessions) single-consumer (worker thread).
- **Worker thread**: dequeues tasks and submits DB work to `BoostThreadPool`.
- **Async transaction chain**: `persist_mail_transactional_async` chains `begin → metadata → attachments → outbox → commit` via nested async callbacks. `shared_ptr<ScopedConnection>` keeps the DB connection alive across the chain.

## 6. Configuration and Startup

Primary runtime config is JSON-based and loaded by `ServerConfig`.

Notable SMTP ingest policy knobs:

- `inbound_ack_mode`
  - `after_persist`: durable-style acknowledgment after persistence success
  - `after_enqueue`: queue-accept acknowledgment with weaker durability guarantee
- `inbound_persist_wait_timeout_ms`
  - upper bound for waiting on persistence completion in `after_persist`
- `persist_max_inflight_mails`
  - total owned messages allowed in persistence pipeline
- `persist_min_available_memory_mb`
  - admission backpressure based on free system memory
- `persist_min_db_available_connections`
  - admission backpressure based on DB pool pressure

Inbound sender verification knobs:

- `inbound_spf_mode` (`off`|`soft`|`hard`, default `off`)
  - SPF check mode. `hard` rejects at `MAIL FROM` on SPF fail.
- `inbound_dkim_mode` (`off`|`soft`|`hard`, default `off`)
  - DKIM signature verification mode.
- `inbound_dmarc_mode` (`off`|`soft`|`hard`, default `off`)
  - DMARC policy enforcement mode.
- `inbound_auth_timeout_ms` (default `30000`)
  - max wait time for the asynchronous verification task on the worker thread.
- `inbound_auth_policy` (`off`|`auto`|`on`, default `off`)
  - AUTH enforcement mode. `off`=relay mode (no AUTH), `auto`=EHLO/PTR-verified servers skip AUTH, `on`=always require AUTH before MAIL FROM.

Startup sequence (simplified):

1. Parse config file and resolve relative paths.
2. Validate listener, SSL, timeout, storage, and optional HDFS settings.
3. Initialize logger sinks and level.
4. Initialize DB/storage adapters.
5. Start listeners and background outbound loop.

Failure policy:

- Invalid critical config causes immediate startup failure.
- Feature mismatch (for example, `hdfs_web` with build-time OFF) fails early and explicitly.

Local benchmark note (see `test/bench/bench-report.md` for full matrix):

- C++ `smtp_client` null storage + null DB: **72303 msg/s** — **纯 FSM 上限**（零磁盘/DB 开销）
- Real disk (local) + MySQL: **12502 msg/s** — FSM + 磁盘写 + DB 事务
- Python `cl.py` numbers (~1.9k-3.0k msg/s) are outdated — Python smtplib/GIL overhead
- These describe single-machine throughput, not a production SLA.

## 7. Build-Time Feature Model

Important CMake options:

- `ENABLE_HDFS_WEB_STORAGE` (default ON)

Effects:

- Controls whether WebHDFS provider is compiled.
- Controls whether CURL is required and linked.
- Exposed in version/help feature metadata (`+hdfs-web` or `-hdfs-web`).

## 8. Deployment Topology

Supported supervisor patterns:

- Linux: systemd unit template in `deploy/systemd/protorelay.service`
- macOS: launchd plist in `deploy/launchd/io.protorelay.server.plist`

Recommended production shape:

- One ProtoRelay instance per config profile.
- Dedicated writable directories for logs/mail/attachments.
- External MySQL and optional HDFS cluster endpoints.
- TLS certificates provisioned externally (self-signed for dev, CA-issued for prod).

## 9. Sharding / Horizontal Scaling

ProtoRelay supports user-based sharding via the `IShardRouter` abstraction. Each shard owns an independent database pool and storage provider. The router maps `email → shard_index`, and all components (FSMs, PersistentQueue, OutboundClient) access DB and storage exclusively through the router—no raw DBPool or IStorageProvider references are held anywhere.

Three routing strategies are provided:
- **Hash** — `hash(email) % N`, deterministic, zero-config
- **Table** — queries `user_shards` table, with in-memory `LruCache` (TTL=0, immutable mappings)
- **Static** — config-file domain-to-shard mapping

The delivery worker polls all shards with a local-first policy: claim from the home shard, then steal from higher-latency shards when idle. `OutboxRepository` is fully stateless—it receives `DBPool&` at call time rather than holding a connection pool.

See [docs/sharding-refactor.md](sharding-refactor.md) for the full design discussion and refactoring history.

## 10. Current Scope and Extension Points

Implemented now:

- SMTP state machine and message parsing.
- Message persistence and outbound queue processing.
- Configurable storage providers and optional WebHDFS.
- Service-manager-friendly operation.
- Transactional outbox creation with local lease-first hot dispatch.

Expected extension directions:

- POP3 retrieval plane.
- DKIM signing policy and key rotation tooling.
- Metrics/trace export for observability stacks.
- Certificate hot-reload and rolling restart ergonomics.
- File-free in-memory outbound MIME construction for the hot-dispatch path.

## 11. Related Docs

- `README.md` / `README_zh.md`
- `test/bench/bench-report.md`
- `docs/framework-refactor.md` — framework extraction history (2026-07)
- `docs/sharding-refactor.md`
- `docs/smtp-outbound-client-design.md`
- `docs/vs-postfix.md`
- `BUILD_GUIDE.md`
