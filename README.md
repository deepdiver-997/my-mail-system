# ProtoRelay

ProtoRelay is a C++17 mail relay core focused on SMTP protocol execution and delivery pipeline foundations.

## Framework / Application Separation

The codebase is structured as a reusable framework + application:

```
include/framework/          # 可复用框架（与邮件协议无关）
├── server_base.h           #   服务器生命周期、配置、DB/存储/分片初始化
├── tcp_server_base.h       #   TCP/SSL 多协议监听器模板
├── session_base.h          #   异步读写 + FSM 集成 + 超时管理
├── fsm_base.h              #   通用状态机 (std::map dispatch)
├── fast_fsm_base.h         #   编译期数组 O(1) dispatch 变体
├── connection/             #   IConnection 抽象 + TCP/SSL 实现
├── thread_pool/            #   IO + Worker 线程池
├── metrics_server.h        #   嵌入式 Prometheus 指标
└── intrusion_detector.h    #   基于 IP 的失败尝试跟踪与封禁

include/mail_system/back/   # 邮件系统（框架的成熟应用）
├── mailServer/             #   SMTP/IMAP 协议服务器 + FSM 状态机
├── db/                     #   数据库抽象层 (IDBConnection/DBPool)
├── storage/                #   存储提供者 (local/s3/hdfs/null)
├── outbound/               #   出站投递引擎 (MX路由/DNS/DKIM)
├── persist_storage/        #   持久化队列 + Bloom 去重
├── inbound/                #   SPF/DKIM/DMARC 入站验证
└── ...
```

邮件系统目前是框架上最成熟的应用。框架组件可在其他协议服务中复用。

## Current Implemented Scope

At this stage, ProtoRelay intentionally focuses on core SMTP capabilities:

- SMTP state machine (session lifecycle and command flow)
- SMTP parsing (commands, message body handling, envelope flow)
- Delivery pipeline (queue + outbound relay path)

This is a deliberate narrow scope: the project is building a robust relay core before adding broader protocol surface.

## Extensibility by Design

ProtoRelay is structured around replaceable modules instead of monolithic logic:

- Database pool abstraction (`mysql`, `null` for benchmarks)
- Storage provider abstraction (`local`, `null`, `s3`/MinIO, `distributed`, `hdfs_web`)
- Outbound delivery and DNS routing modules
- Config-driven wiring in server bootstrap

This means new providers and strategies can be added with minimal impact on SMTP FSM core behavior.

## CLI (Large-Project Style)

ProtoRelay now follows a stable CLI contract similar to mature tools:

- `--help` / `-h`: consistent usage text
- `--version` / `-V`: build-time injected metadata (version, commit, target, compiler)
- `--config` / `-c <path>`: explicit config file path
- Backward compatibility: one positional `config_path` is still supported

Example:

```bash
./build/smtpsServer --help
./build/smtpsServer --version
./build/smtpsServer -c config/smtpsConfig.json
```

## Inbound ACK and Persistence Tuning

Inbound SMTP acceptance is now configurable at runtime:

- `inbound_ack_mode=after_persist`: reply `250 OK` only after persistence succeeds.
- `inbound_ack_mode=after_enqueue`: reply `250 OK` as soon as the message is accepted by the persistence queue.

Related tuning fields:

- `inbound_persist_wait_timeout_ms`: max wait time in `after_persist` mode before returning timeout failure.
- `persist_max_inflight_mails`: cap for total owned messages in persistence pipeline.
- `persist_min_available_memory_mb`: reject enqueue below free-memory threshold.
- `persist_min_db_available_connections`: reject enqueue when DB pool is under pressure.

Example:

```json
{
  "inbound_ack_mode": "after_enqueue",
  "inbound_persist_wait_timeout_ms": 5000,
  "persist_max_inflight_mails": 2048,
  "persist_min_available_memory_mb": 256,
  "persist_min_db_available_connections": 1
}
```

Operational note:

- `after_enqueue` improves throughput and tail latency, but `250 OK` no longer guarantees durable persistence.
- `after_persist` is safer for durability, but throughput is bounded by persistence completion latency.
- **Full benchmark matrix (C++ `smtp_client`)**: see `test/bench/bench-report.md`
  - seq+reuse (local file storage, no DB): **18,278 msg/s** @ 8 threads
  - pipe+reuse (null storage + null DB): **72,303 msg/s** @ 32 threads — 纯 FSM+TCP 上限
  - pipe+reuse (real disk + MySQL, refactor前): **12,502 msg/s** @ 32 threads
  - FSM mock (零 I/O, refactor后): **15,913 msg/s** @ 4 threads (旧: 4,127)
  - localhost per-conn limited by ephemeral port pool (~16384); `--local-ips` 绕过
- M2 Pro (12-core) macOS single-machine figures, not a production SLA
- **72303 msg/s 不含任何磁盘/数据库开销**（null storage + null DB），仅 FSM + TCP loopback
- Use `"storage": {"provider": "null"}` + `"use_database": false` for ceiling benchmarks
- C++ `smtp_client` is the primary bench tool; `test/scripts/cl.py` for TLS/AUTH smoke tests

### Performance Tuning Notes

1. **IO pool distribution**: TCP sockets must bind to `IOThreadPool::get_io_context()` (round-robin across N io_contexts), not `ServerBase::get_io_context()` (single listener context). A prior regression routed all TCP connections to one thread, cutting throughput 7×.
2. **Connection reuse**: Benchmark script reuses connections by default. `--per-conn` for realistic per-message connections.
3. **Auth cache**: `LruCache` in SmtpsFsm (TTL 5min, cap 10000) avoids DB queries for repeat auth.
4. **Lock-free queue**: `boost::lockfree::queue` replaced `deque + mutex + cv` in PersistentQueue, with exponential backoff for the worker.
5. **Log level**: INFO-level spdlog synchronous stdout writes become the bottleneck under concurrency; use `warn` for benchmarks.
6. **SMTP pipelining**: `do_async_read` checks the command buffer first — complete lines are processed immediately (one FSM round-trip each), only falling back to network read when exhausted. Incomplete commands wait for the next TCP chunk.

## Current Outbound Hot-Dispatch Semantics

The persistence queue now writes `mail_outbox` in the same DB transaction as `mails`, `mail_recipients`, and `attachments`.

- If the local node can immediately own outbound delivery, it inserts those `mail_outbox` rows as locally leased `SENDING`
- After commit, it hands `unique_ptr<mail>` plus the reserved outbox records to the local outbound client
- If local handoff fails, the reservations are released back to `PENDING` so other nodes can claim them

This hot path currently optimizes for local ownership and one less DB claim round. MIME construction still falls back to reading `body_path`, so it is not yet a fully file-free in-memory outbound path.

## Build-Time Version Injection

Version/build metadata is generated during CMake configure and injected into the binary, including:

- semantic version
- git short commit
- build timestamp (UTC)
- target triple-ish info (`OS-ARCH`)
- compiler identity/version
- feature toggles

## Build

```bash
./build.sh Debug
./build.sh Release
```

The build script auto-creates `build/` and keeps generated CMake artifacts out of source root.

## Runtime Prerequisites (Summary)

- Linux/macOS
- CMake 3.10+
- GCC 9+ / Clang 10+
- Boost, OpenSSL, MySQL client, spdlog, c-ares
- If `hdfs_web` storage is enabled: also require libcurl

## Project Conventions

See style and engineering conventions:

- `docs/PROJECT_STYLE.md`

## A Note for Students

If you're a student looking for a course project reference — feel free to study, borrow, or adapt any part of this codebase. No need to ask. That said, a GitHub star would be much appreciated. Credit where it's due, but mostly it helps me know this project was useful to someone.

## License

MIT. See `LICENSE`. Boost license in `COPYING_BOOST.txt`.
