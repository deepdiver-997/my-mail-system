# ProtoRelay 测试文档

## 测试架构

```
test/
├── unit/                        # C++ 单元测试（零 I/O，MockConnection）
│   ├── smtps_fsm_test.cpp       # SMTP FSM 状态机测试（串行）
│   ├── smtps_fsm_concurrency_test.cpp  # SMTP 入站异步并发测试（MockIoContext + TSan）
│   ├── imaps_fsm_test.cpp       # IMAP FSM 状态机测试
│   ├── pop3_fsm_test.cpp        # POP3 FSM 状态机测试（11 命令 + 锁心跳/sweeper）
│   ├── test_inbound_verifier.cpp# InboundVerifier 组件测试 (86 tests)
│   ├── sql_queries_test.cpp     # SQL 查询生成器测试
│   ├── outbound_smoke.cpp       # 出站类型/FSM 烟雾测试
│   ├── mime_parser_test.cpp     # MIME 解析器测试
│   ├── buffered_upload_stream_test.cpp # 远程后端整对象缓冲上传流测试
│   ├── io_error_test.cpp        # IoError 错误分类测试（errno/HTTP → retryable/permanent）
│   ├── async_storage_provider_test.cpp # 远程后端真异步装饰器测试
│   ├── fsm_base_test.cpp        # FsmBase 分发语义（terminal/非法转换/缺 handler）
│   ├── fast_fsm_base_test.cpp   # FastFsmBase O(1) 分发 + fallback 优先级
│   ├── intrusion_detector_test.cpp # 入侵检测（私网过滤/封禁阈值/LRU/持久化）
│   ├── metrics_core_test.cpp   # MetricsServer counter/gauge/histogram 语义不变量（counter N 次增 = N）
│   ├── server_config_test.cpp   # pr::ServerConfig JSON 加载/校验/listener 工具
│   ├── mapped_file_test.cpp     # MappedFile/MappedReadStream mmap 只读映射
│   ├── thread_pool_test.cpp     # BoostThreadPool submit/post + IOThreadPool get_io_context
│   ├── server_base_test.cpp     # ServerBase 生命周期/计数/入侵集成/reload_config
│   ├── session_base_test.cpp    # SessionBase 读写/pause-drain/缓冲上限/trace/错误码
│   ├── test_session.h           # 最小 TestSession（SessionBase<MockConnection> 具体化）
│   ├── mock_io_context.h        # 简化 asio io_context（任务队列+线程）
│   ├── mock_connection.h        # 零 I/O Mock 连接（任务投递模型）
│   ├── mock_dns_resolver.h      # Mock DNS（Sync/Manual/AutoDelay 三模式）
│   ├── mock_db_pool.h           # Mock DB 连接池（延迟 async_query）
│   └── mock_outbound_stream.h   # 出站流 Mock
├── bench/                       # 性能基准测试
│   ├── fsm_bench.cpp            # FSM 吞吐基准
│   ├── smtp_client.cpp          # 高性能 SMTP 客户端
│   ├── bench.sh / run_bench_all.sh
│   └── bench-report.md
├── fuzz/                        # libFuzzer harness（默认关闭，见 CMakeLists ENABLE_FUZZING）
│   ├── mime_fuzz.cpp            # MIME 解析 fuzz（纯函数、零 I/O，首推目标）
│   ├── smtp_data_fuzz.cpp       # SMTP DATA 解析核心 fuzz（分块/流式双模式）
│   └── corpus/                  # 种子+回归语料（regress-*.eml 是真实崩溃样本）
├── e2e/                         # Python 端到端测试
│   ├── test_smtp_flow.py        # SMTP 全流程测试 (端口 25/465/587)
│   ├── test_dual_server.py      # 双服务器互通测试 (带 static route)
│   ├── test_outbound.py         # 出站投递
│   ├── test_metrics_exposure.py # 验 /metrics 端点 + counter 语义修复（不依赖 DB）
│   ├── test_pop3_flow.py        # POP3 流程 + metrics 端点（TCP-only 不依赖 DB）
│   ├── test_pipeline.py         # SMTP 流水线
│   └── test_tcp_sticky.py       # TCP 粘包/截断/延迟
├── server/                      # 服务器入口（main）
│   ├── smtps_test.cpp           # SMTP 服务器
│   ├── imaps_test.cpp           # IMAP 服务器
│   └── mail_server_combined.cpp # 合并服务器
├── scripts/                     # 测试脚本
│   ├── integration_test.sh      # 集成测试
│   ├── setup_test_env.py        # 测试环境初始化
│   ├── cl.py                    # SMTP 压力测试器
│   ├── test_auth.sh             # SMTP AUTH 测试
│   ├── test_ports.sh            # 端口连通性测试
│   └── cleanup.sh               # 数据清理
├── config/                      # 测试配置
│   ├── smtps_test.json
│   └── imaps_test.json
├── tools/                       # 开发工具
│   └── hash_tool.cpp            # bcrypt 密码哈希工具
└── README.md
```

## Mock 模型与并发测试写法

> 写 SMTP/IMAP 协议 FSM 的并发/健壮性测试前先读本节。mock 已重构为**还原 asio
> 投递语义**的任务模型（见 `smtps_fsm_concurrency_test.cpp`），串行 FSM 测试
> （smtps/imaps_fsm_test）零改动。

**Mock 三层**：

- **`mock_io_context.h`** — 简化 asio io_context（任务队列 + 独立工作线程）。
  - 同步模式（默认，未 `start()`）：`post()` 内联执行 → 串行测试。
  - 线程模式（`start()` 后）：任务由独立线程 FIFO 执行 → 真实异步投递。
- **`mock_connection.h`** — 异步读写包装为任务投递到 MockIoContext，工作线程把
  数据跨线程拷进 session 的 `read_buffer_`；`written()` 返回加锁副本。
- **`mock_dns_resolver.h`** — 三模式：
  - `Sync`（默认）：立即回调，兼容 `SyncDnsWrapper`（`test_inbound_verifier` 依赖）。
  - `Manual`：存回调（按 domain 入 deque，支持多会话同域），测试线程 `fire_*()` 手动触发。
  - `AutoDelay`：后台线程延迟触发（压力用；delay 须匹配生产时序，否则产生假竞争）。
- **`mock_db_pool.h`** — `async_query/async_execute` 延迟回调模拟 DB worker。
  同步分支持回调必须在**锁外**调用（否则回调链重入非递归锁死锁）。

**驱动时序（关键不变量）**：

```
run_on_io(h) = conn->start()                        // 线程模式
             + context().post(process_read)         // 命令链在独立线程执行
             + wait_idle()                          // ★ 等 io 线程完全退出命令链
    ↓
wait_until(dns->has_pending_txt("domain") / db->has_pending_query())
    ↓
fire_*() 触发异步回调（手动/后台线程）                // ★ 必须在 wait_idle 之后
```

- ★ 回调触发**必须**等 io 线程完全退出命令链（`wait_idle` 保证）。否则回调线程与
  io 线程并发访问 session（`paused_`/`state_`/`command_read_buffer_`）产生真竞争。
  生产时序（DNS 延迟 >> io 退出）不会触发，但测试会。
- 断言回调 pending 用 `wait_until(has_pending_*)` 轮询；读响应用 `h->captured`
  （`capture_to` 镜像，session 析构后仍可断言）。
- **已修进代码的会话不变量**（不是 bug）：`paused_`/`state_` 是 `std::atomic`；
  FSM 回调 `drain_buffered_commands()` 后 `!s->is_paused()` 才 `do_async_read()`。

**TSan**：`cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON`，
跑并发测试；判定用 `grep -c "WARNING: ThreadSanitizer"`（PASS 文案里的"无 data
race"会被 `grep "data race"` 误计）。

**写 IMAP 并发测试迁移清单**：
1. mock 三层已在位，IMAP 串行测试用同步模式即可。
2. 盘点 IMAP 异步路径（认证 / STORE / FETCH 是否走 async DB/DNS）；若无，重点
   测 session 生命周期与关闭时序（IDLE/DONE deferred read、LOGOUT、异常关闭）。
3. 沿用 `run_on_io`（post+wait_idle）+ Manual fire 模式。
4. 边界用例照抄 `boundary_callback_after_session_release`（shared_ptr 保活）。

## 运行测试

### 快速运行
```bash
# 1. 初始化测试环境（拷贝配置，修改路径）
python3 test/scripts/setup_test_env.py

# 2. 构建
./build.sh Release

# 3. 运行单元测试
cd build && ctest && cd ..

# 4. 运行集成测试
bash test/scripts/integration_test.sh

# 5. 运行 Python 端到端测试
python3 test/e2e/test_outbound.py    # 出站投递测试
python3 test/e2e/test_pipeline.py    # 流水线测试
python3 test/e2e/test_tcp_sticky.py  # TCP 粘包截断测试
python3 test/e2e/test_pop3_flow.py   # POP3 流程 + metrics
```

### 仅运行单元测试
```bash
./build.sh Release
./build/smtps_fsm_test
./build/imaps_fsm_test
./build/pop3_fsm_test
./build/test_inbound_verifier
./build/outbound_smoke
./build/sql_queries_test
```

### 运行性能基准
```bash
cd test/bench && bash bench.sh
```

## Python E2E 测试

与单元测试不同，E2E 测试启动真实的 `smtpsServer` 进程，通过 TCP 连接验证完整的 SMTP 协议交互。所有测试自动创建临时配置和存储目录，不会污染项目源文件。

### test_smtp_flow.py — SMTP 全流程测试

验证三个端口的不同认证策略和投递流程：

| 端口 | 认证策略 | 测试内容 |
|------|---------|---------|
| 25 | `auth_policy: off` | MTA 直投（免认证）：EHLO → MAIL FROM → RCPT TO → DATA |
| 587 | `auth_policy: on` | 客户端提交：拒绝未认证 MAIL FROM、广告 AUTH |
| 465 | SSL + AUTH | SSL 握手、拒绝未认证命令 |

**特点**：
- 自动创建临时配置（`perf_mode=false` 启用安全检查，独立存储路径）
- 测试后删除临时目录
- 验证投递后邮件文件确实写入磁盘

```bash
python3 test/e2e/test_smtp_flow.py
# 保留临时文件以便调试:
python3 test/e2e/test_smtp_flow.py --keep-temp
```

### test_dual_server.py — 双服务器互通测试

启动两个 `smtpsServer` 实例模拟 MTA 间邮件投递：

```
Server A (:10025) ──static route b.local→127.0.0.1:10026──→ Server B (:10026)
     │                                                            │
  smtplib.sendmail()                                        verify_mail() ✓
```

**流程**：
1. Server A 配置 `outbound.static_routes: {"b.local": {"host": "127.0.0.1", "port": 10026}}`
2. Server B 在 10026 端口监听，免认证接受一切
3. 通过 Server A 投递给 `user@b.local`
4. Server A 的 static route 跳过 DNS 直接连接 Server B
5. 验证 Server B 的文件系统收到了邮件

```bash
python3 test/e2e/test_dual_server.py
```

### test_metrics_exposure.py — `/metrics` 端点 + counter 语义修复

**故事**：2026-08-27 修了一个 counter 语义 bug —— `ServerBase::increment_*` 三个 helper
把 `fetch_add` 返回的累计值 `v`（1,2,3,...）传给 `inc_counter` 当 delta，导致
counter map 累加成 `1+2+...+N = N*(N+1)/2`（三角形数），`/metrics` 渲染出错的数字。
详见 [counter 三角形 bug 复盘](../docs/mail-system/bugfixes/2026-08-27-counter-triangle-bug.md)。

**测试策略**（不依赖 database，`use_database=False`，避免 DB 启动成本）：
- 起一个真 `smtpsServer`（`metrics_enabled=True`，`metrics_port=19090`）
- 发 N 条 TCP 连接 + EHLO/QUIT（不发邮件 — 本地 RCPT 校验需 DB，目标是验 counter 语义 + 端点，不是 SMTP 协议）
- 拉 `/metrics`、`/status`、`/health/live`，断言：
  - `protorelay_connections_total` diff == N（不是三角形数）
  - `/metrics` 文本格式正确
  - `/status` 返回 JSON，`/health/live` 返回 OK

```bash
python3 test/e2e/test_metrics_exposure.py
# 保留临时文件:
python3 test/e2e/test_metrics_exposure.py --keep-temp
```

### test_pop3_flow.py — POP3 流程 + metrics 端点（TCP-only）

**故事**：POP3 服务器（RFC 1939）落地后的端到端验收，同时验证 `protorelay_pop3_*`
metrics 随真实流量递增。

**测试策略**（不依赖 database，`use_database=False`）：
- 起真 `pop3Server`（metrics 19090），验证协议流程：greeting 格式
  `+OK ProtoRelay POP3 server ready <ts@pop3>`、CAPA 多行列表、USER、
  PASS（无 DB → 预期 `-ERR`）、QUIT。
- 一条异常断连会话（收 greeting 后直接断开）→ `sessions_total{result=err}`。
- 拉 `/metrics` 断言 `sessions_total{result=ok/err}` 与 `auth_total` 递增。
- `retr/dele/lock_conflict` 三个指标需 TRANSACTION+DB（e2e 不依赖 DB），
  由 `pop3_fsm_test` 单测覆盖代码路径。

```bash
python3 test/e2e/test_pop3_flow.py
```

### pop3_fsm_test — 11 命令 + 锁心跳/sweeper（33 case）

**故事**：POP3 FSM 零 I/O 状态机测试（仿 `imaps_fsm_test` 夹具）。除 11 命令 /
状态错 / dot-stuffing / 3 次失败关闭外，重点覆盖 **锁租约**（见
[mailbox-concurrency 文档](../docs/mail-system/architecture/mailbox-concurrency.md)）：

- `heartbeat_timer_armed` — PASS 后定时器装配
- `heartbeat_renew_keeps_lock` / `heartbeat_renew_lock_lost` — 条件 UPDATE+verify 续约
- `heartbeat_lock_lost_closes` — **剧情测试**：锁被外部回收后，续约 verify 得 0
  → 会话必须关闭而非带着失效锁继续（防死锁）
- `heartbeat_stops_after_close` — close 幂等取消
- `sweep_expired_locks_ok` — sweeper SQL 执行

**Mock 技巧**：POP3 greeting 由 CONNECT 事件主动发出，写完成后的 `do_async_read`
在 Mock 空缓冲下会 EOF 提前关闭会话 → `make_session` 里 `set_deferred_read(true)`
让读挂起；`Handle` 析构幂等 `close()` 防 deferred-read 引用环（LSan 泄漏）。

```bash
./build/pop3_fsm_test
```

### 真实 DB 测试后清理

```bash
# 仅清理文件和进程
bash test/scripts/cleanup.sh

# 清理 DB 非用户表（保留 users 账号）
bash test/scripts/cleanup.sh --all

# 完全重建 DB（包括 users 表 + schema）
bash test/scripts/cleanup.sh --hard
```

---

## 重要注意事项

### 清理海量邮件文件务必使用 `rm -rf`

当 `mail/` 或 `attachments/` 目录下有数万甚至数十万文件时，
`find -delete` 或逐个 `rm` 会极其缓慢（每个 `stat()` + `unlink()` 系统调用）。

**正确做法**: `rm -rf mail/ && mkdir -p mail/`

- 内核调用 `unlinkat(AT_REMOVEDIR)` 递归删除目录树，仅 O(目录深度) 次系统调用
- `find -delete` 在 10 万文件下可能需要数分钟，`rm -rf` 仅需毫秒

### 真实 DB 测试注意

- 集成测试会产生大量 mails / mail_recipients / attachments / mail_outbox 记录
- **不要在生产 DB 上跑压测**，使用测试专用数据库
- 测试后务必执行 `cleanup.sh --all` 清理非用户表
- `users` / `mailboxes` 表不会被清理（`--all` 模式），保留下次测试复用
- 若需要完全重置，使用 `--hard` 模式（会重建所有表）

### Mock 模式不使用真实文件系统/数据库

Mock 测试（`smtps_mock.json` / `imaps_mock.json`）配置 `use_database: false`，
所有存储落在 `/tmp/protorelay_test/`（tmpfs/临时目录，重启自动清除），
不会有文件堆积问题。

---

## 已知问题

### 1. Apple clang 17 Debug 模式 `std::make_shared` 编译失败

**现象**：`-O0` 时 `OutboundSmtpSession` / `SmtpsSession` / `ImapsSession` 的
`std::make_shared` 报错：
```
error: incompatible pointer types assigning to '__shared_weak_count *'
from 'std::__shared_ptr_emplace<...> *'
```

**原因**：Apple clang 17 (Xcode 17) 的 libc++ 实现在 `-O0` 优化级别下，
处理带 `std::enable_shared_from_this` 继承链的 `std::make_shared` 时存在
内部控制块类型转换 bug。`-O3` 时优化器消除了相关中间路径，不会触发。

**影响范围**：
- `OutboundSmtpSession` — 间接继承 `SessionBase` → `enable_shared_from_this`
- `SmtpsSession` — 同样继承链
- `ImapsSession` — 同样继承链

**解决方案**：使用 Release 模式编译（`build.sh Release`），或在不改源码的前提下
使用 `EXTRA_CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Release"` 覆盖。

**不推荐的 workaround**：将 `std::make_shared<T>(args...)` 替换为
`std::shared_ptr<T>(new T(args...))`。这会绕过 libc++ 的优化路径，
但：
- `make_shared` 单次分配（对象+控制块），`new` + `shared_ptr` 两次分配
- 对 `enable_shared_from_this` 语义无影响，但性能稍差
- 源码不必要复杂化

**结论**：保持使用 `make_shared`，用 Release 构建（项目默认行为）。

### 2. SMTP FSM 测试中的投递流水线测试被跳过

**现象**：`test_full_delivery_pipeline`、`test_empty_body`、`test_dot_stuffing`、
`test_multiple_transactions` 等测试需要完整的 PersistentQueue + 数据库流水线，
在 Mock 环境下无法工作。

**原因**：`PersistentQueue` worker 线程会尝试访问数据库连接池持久化邮件。
Mock 环境的 `StaticShardRouter` 持有空的 DBPool 列表，`get_db_pool()` 返回
nullptr，导致 SIGSEGV。

**解决方案**：需要 `NullDBPool` 或一个轻量 Mock DBPool 来支持持久化测试。
当前这些测试标记为已知受限（`// 已知: 需要 persist queue`）。

### 3. STARTTLS 测试被跳过

**现象**：`test_starttls` (SMTP) 在 Mock 环境下 SIGBUS。

**原因**：STARTTLS handler 调用 `release_socket()` 后会释放底层连接，
`MockConnection::release_socket()` 返回 `nullptr`，后续操作访问空指针。

**解决方案**：需要 Mock STARTTLS handler 或重写 MockConnection 支持
`release_socket` 后保留写入缓冲区引用。`capture_to()` 机制已存在，
但 handler 路径需要适配。

### 4. IMAP LOGOUT 测试被跳过

**现象**：`test_logout_bye`、`test_logout_without_login` SIGSEGV。

**原因**：LOGOUT handler 置状态为 `LOGOUT`（终端状态），`dispatch()` 调用
`session->close()`。测试在 close 后读取 `h.conn->written()` 时，
MockConnection 已被销毁。

**解决方案**：LOGOUT 测试应在 handler 内截获输出（类似 STARTTLS 的 `capture_to`）。

### 5. IMAP IDLE 测试被跳过

**现象**：Mock 环境下 IDLE handler 行为不确定。

**原因**：IDLE 需要异步等待 `DONE` 事件。Mock 环境的所有 I/O 是同步的，
无法正确模拟 IDLE 的 `async_read` → `DONE` 回调链。

### 6. `smtps_fsm_test` 间歇性 SIGBUS/SIGSEGV —— 非法向下转型读越界（已修复）

**现象**：Release 构建下 `smtps_fsm_test` 约 70% 崩溃（exit 138=SIGBUS / 139=SIGSEGV），
崩溃不稳定。lldb 抓到崩溃点在 `SmtpsSession` 构造函数：
`ldadd x9, x8, [x8]`（ARM64 原子自增），`x8` 是非 8 字节对齐的堆垃圾
（曾解析出 ASCII 残留 `"set OLHM"`）。

**根因（与已知问题 #1 无关）**：`TestServer` 直接继承 `ServerBase`，
但 `SmtpsSession` 构造函数用 `static_cast<SmtpsServer*>(server)->m_persistentQueue`
取持久化队列。`m_persistentQueue` 是 `SmtpsServer` 的成员（`smtps_server.h`），
对非 `SmtpsServer` 的对象做该向下转型是未定义行为：编译器按 `SmtpsServer`
的布局在 `TestServer+0x228` 处读 16 字节堆垃圾当作 `shared_ptr`，构造出野
控制块指针，随后原子自增即崩溃（`ldadd` 要求 8 字节对齐）。

**为何间歇**：`TestServer+0x228` 的垃圾值进程内固定、跨进程随堆布局变化——
运气好读到可读且对齐的地址就静默损坏随机内存（不崩），多数读到非法地址就崩。

**为何 imaps 稳定**：`ImapsSession` 构造函数没有 `persistent_queue_` 成员、
没有该转型（`imaps_session.tpp`），与 enable_shared_from_this 类布局无关。

**修复**：`TestServer` 改为派生自 `SmtpsServer`（转型合法），并在 fixture 绑定
`server->m_persistentQueue = persist_q`。同源缺陷也存在于 `fsm_bench.cpp`
的 `BenchServer`（派生自 `ServerBase` 且丢弃了传入的队列参数），一并修复。
根因分析见 [session/smtps_session.tpp 的 TODO 注释](../../include/mail_system/back/mailServer/session/smtps_session.tpp#L22)。

**遗留加固建议**（未改生产代码）：构造函数内的裸 `static_cast<SmtpsServer*>` 仍是
潜在地雷，可改用 `dynamic_cast` + null 兜底、在 `ServerBase` 加虚访问器，或将队列
作为显式构造参数传入（权衡见上述 TODO 注释）。

---

## FSM 状态覆盖矩阵

### SMTP FSM
| 状态 | CONNECT | EHLO | AUTH | MAIL_FROM | RCPT_TO | DATA | DATA_END | QUIT | STARTTLS | ERROR | TIMEOUT |
|------|---------|------|------|-----------|---------|------|----------|------|----------|-------|---------|
| INIT | ✓ | | | | | | | | | | |
| GREETING | | ✓ | | | | | | | | | |
| WAIT_AUTH | | ✓ | ✓ | ✓ | | | | | ✓ | | |
| WAIT_AUTH_USERNAME | | | ✓ | | | | | | | | |
| WAIT_AUTH_PASSWORD | | | ✓ | | | | | | | | |
| WAIT_MAIL_FROM | | | | ✓ | | | | | | | |
| WAIT_RCPT_TO | | | | ✓ | ✓ | ✓ | | | | | |
| IN_MESSAGE | | | | | | ✓ | | | | | |
| WAIT_DATA | | | | | | ✓ | | | | | |
| WAIT_QUIT | | | | ✓ | | | | ✓ | | | |

✓ = 已测试  · = 跳过（见已知问题）

### IMAP FSM
| 状态 | LOGIN | CAPABILITY | SELECT | FETCH | STORE | SEARCH | CREATE | DELETE | RENAME | LIST | LSUB | STATUS | APPEND | CHECK | EXPUNGE | CLOSE | COPY | MOVE | SUBSCRIBE | UNS | EXAMINE | NOOP | LOGOUT | IDLE | STARTTLS |
|------|-------|------------|--------|-------|-------|--------|--------|--------|--------|------|------|--------|--------|-------|---------|-------|------|------|-----------|-----|---------|------|--------|------|----------|
| NOT_AUTH | ✓ | ✓ | ✓ | ✓ | ✓ | | | | | | | | | | | | | | | | | ✓ | · | | |
| AUTH | | ✓ | ✓ | | | | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | | | | ✓ | ✓ | ✓ | ✓ | · | · | |
| SELECTED | | | | ✓ | ✓ | ✓ | | | | | | | | | ✓ | ✓ | ✓ | ✓ | | | | ✓ | | | |

✓ = 已测试（无 DB 优雅失败）  · = 跳过（见已知问题）

---

## `make_shared` 问题的详细分析

### 触发条件
1. 编译器：Apple clang 17.0.0+ (Xcode 17)
2. 优化级别：`-O0` (Debug)
3. 类型特征：类通过继承链间接包含 `std::enable_shared_from_this<B>`
4. 构造参数：至少一个参数（非无参构造）

### 编译命令对比
```bash
# 会失败
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target smtps_fsm_test

# 正常
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target smtps_fsm_test
```

### 技术细节
`SessionBase` 继承自 `std::enable_shared_from_this<SessionBase<ConnectionType>>`。
`SmtpsSession` → `SessionBase` 的间接继承需要 `make_shared` 在构造时正确设置
`weak_ptr` 内部指针关系。

在 `-O0` 下，libc++ 的 `__shared_ptr_emplace` 的 `__create_with_control_block`
模板展开路径中，控制块指针类型的 static_cast 检查失败。`-O2` 及以上优化级别会
内联并消除该中间转换步骤。

### 验证
```bash
# 最小复现
cat > /tmp/test.cpp << 'EOF'
#include <memory>
struct B : std::enable_shared_from_this<B> { virtual ~B() = default; };
struct D : B { D(int) {} };
int main() { auto p = std::make_shared<D>(42); }
EOF
# Debug: 可能失败（取决于 clang 版本）
c++ -std=c++20 -O0 /tmp/test.cpp
# Release: 正常
c++ -std=c++20 -O3 /tmp/test.cpp
```

> ⚠️ 注意区分：**本节的 Debug 编译失败**与 **`smtps_fsm_test` 的 Release 运行时崩溃
> （已知问题 #6）不是同一回事**。运行时崩溃的根因是 `SmtpsSession` 构造函数里
> `static_cast<SmtpsServer*>(server)` 对非 `SmtpsServer` 对象做非法向下转型，
> 与 libc++ 的 `make_shared` 实现无关，已在 #6 中修复。
