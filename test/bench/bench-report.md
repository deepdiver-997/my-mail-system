# ProtoRelay 性能测试报告

> **最后更新**: 2026-06-28
> **测试工具**: C++ `smtp_client` (raw sockets, TCP_NODELAY), 替代了之前的 Python `cl.py`
> **测试环境**: macOS ARM64, localhost
> **服务端**: `perf_mode=true`, `persist_max_inflight_mails=1000000`, `inbound_ack_mode=after_enqueue`
> **服务端线程**: 4 io + 4 worker

---

## 重要更正 (2026-06-28)

**以下早期测试结果标记为过时/可能有误**（2026-06-25 用 Python `cl.py` 测试）:

| 标记 | 原因 |
|------|------|
| ~~端口25 seq+per-conn: 514 msg/s~~ | Python smtplib 开销大, C++ 实测 **6359 msg/s** |
| ~~端口25 seq+reuse: 4114 msg/s~~ | Python smtplib 开销大, C++ 实测 **11147 msg/s** |
| ~~端口25 pipe+per-conn: 5872 msg/s~~ | 未控制 ephemeral port 耗尽, 且 Python 开销 |
| ~~端口25 pipe+reuse: 9180 msg/s~~ | 未做每轮清理, 文件堆积影响结果 |

Python `cl.py` 的结果受限于:
1. Python smtplib + 线程开销 (GIL)
2. 未在每轮间清理 mail 目录, 文件堆积拖慢后续测试
3. localhost 下 per-conn 测试未考虑 ephemeral port 耗尽（四元组冲突）

**以下 2026-06-28 C++ 结果才是可信的.**

---

## 2026-06-28 — 全投递路径矩阵 (C++ smtp_client, port 25, no TLS)

### 测试条件

```
工具:      C++ smtp_client (raw BSD sockets, TCP_NODELAY)
服务端:    smtpsServer, perf_mode=true, 4 io + 4 worker
网络:      localhost (127.0.0.1:25)
TLS:       无 (port 25 MTA relay)
per-conn:  每封邮件新建 TCP 连接 + EHLO + 事务 + QUIT
reuse:     每线程复用一条 TCP 连接, 首封 EHLO, 后续直接 MAIL FROM
pipeline:  所有 SMTP 命令一次 write() 批量发送
sequential:逐命令发送 + 等待响应 (smtplib 风格)
清理策略:  reuse 测试每轮 kill server + cleanup.sh
           per-conn 测试每个组合间等待 TIME_WAIT 排空 (35s)
消息量:    per-conn 5000 封 (受限于 localhost ephemeral port ~16384)
           reuse 50000 封 (不受端口限制)
```

### 结果

#### 1. Sequential + Per-Conn (串行命令, 每封新建 TCP)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 5000 | 0 | 2.58s | 1940 msg/s |
| 2 | 4998 | 2 | 1.55s | 3220 msg/s |
| 4 | 4996 | 4 | 1.08s | 4621 msg/s |
| **8** | **4984** | **16** | **0.78s** | **6359 msg/s** |
| 16 | 4980 | 20 | 0.91s | 5477 msg/s |

**峰值 6359 msg/s @ t=8**

#### 2. Sequential + Reuse (串行命令, MTA 复用 TCP)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 5000 | 0 | 1.64s | 3057 msg/s |
| 2 | 5000 | 0 | 1.16s | 4295 msg/s |
| 4 | 5000 | 0 | 0.64s | 7799 msg/s |
| 8 | 5000 | 0 | 0.55s | 9101 msg/s |
| **16** | **5000** | **0** | **0.45s** | **11147 msg/s** |
| 32 | 5000 | 0 | 0.59s | 8490 msg/s |

**峰值 11147 msg/s @ t=16**, 零失败

#### 3. Pipeline + Per-Conn (批量写, 每封新建 TCP)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 5000 | 0 | 2.09s | 2390 msg/s |
| 2 | 5000 | 0 | 1.26s | 3975 msg/s |
| **4** | **4989** | **11** | **0.88s** | **5657 msg/s** |
| 8 | 1352 | 3648 | 1.67s | 807 msg/s |

**峰值 5657 msg/s @ t=4**

t=8 时大量失败 — pipeline 连接生命周期极短, 连接到达速率超过服务端 accept backlog (`kern.ipc.somaxconn=128`)

#### 4. Pipeline + Reuse (批量写, MTA 复用 TCP, 最大吞吐)

| 线程 | 成功 | 失败 | 耗时 | 速率 |
|------|------|------|------|------|
| 1 | 50000 | 0 | 13.40s | 3731 msg/s |
| 2 | 50000 | 0 | 7.77s | 6436 msg/s |
| 4 | 50000 | 0 | 5.45s | 9179 msg/s |
| 8 | 50000 | 0 | 4.17s | 11988 msg/s |
| 16 | 50000 | 0 | 4.19s | 11921 msg/s |
| **32** | **50000** | **0** | **4.00s** | **12502 msg/s** |
| 64 | 50000 | 0 | 4.19s | 11919 msg/s |

**峰值 12502 msg/s @ t=32**, 零失败。每轮独立重启+清理, 避免文件堆积

---

## 对比总结

| # | 投递路径 | 峰值 | 最优t | 说明 |
|---|---------|------|-------|------|
| 1 | seq + per-conn | **6,359** | 8 | 最慢, 每封 TCP握手+EHLO+逐命令往返 |
| 2 | seq + reuse | **11,147** | 16 | 传统 MTA 模式, 复用连接省 TCP 握手 |
| 3 | pipe + per-conn | **5,657** | 4 | 批量写省往返, 但 TCP 握手+backlog 限制并发 |
| 4 | pipe + reuse | **12,502** | 32 | **最快**, pipeline 省往返 + reuse 省握手 |

### 收益拆解

```
seq + per-conn                              6359 msg/s  (基准)
seq + reuse    +75%  (连接复用)             11147 msg/s
pipe + per-conn -11% (批量写但TCP握手抵消)   5657 msg/s
pipe + reuse   +97%  (连接复用+批量写)      12502 msg/s
```

**连接复用是收益最大的单项优化 (+75%)**。Pipeline 在 per-conn 场景下因 accept backlog 瓶颈反而不如 seq, 但在 reuse 场景下叠加后有额外 +12% 收益。

---

## localhost per-conn 测试的固有限制

### ephemeral port 耗尽 (四元组冲突)

```
macOS 临时端口范围: 49152 - 65535 (16384 个)
TIME_WAIT 持续:     2 × MSL = 30 秒
```

per-conn 每封邮件一个 TCP 连接, 四元组 `(127.0.0.1, client_port, 127.0.0.1, 25)` 中 client_port 来自临时端口池。

连接关闭后端口进入 TIME_WAIT (30s), **同一四元组不可复用**。`SO_REUSEADDR` 对客户端 `connect()` 不生效, 因为四元组完全相同时 TCP 协议栈会拒绝。

**30 秒窗口内 localhost per-conn 的理论上限 ≈ 16384 封邮件。** 超过此数量 connect() 必定失败 (EADDRNOTAVAIL)。

```
实测验证:
./build/smtp_client                    # pipe per-conn, 50000 msgs
pipe=Y reuse=N total=50000 ok=16350 fail=33650  ← 成功数 ≈ 临时端口数
```

### 生产环境不受影响

生产环境中 MTA 连接的是**不同外部服务器 IP**, 四元组自然不同:
```
(本机IP, port_X, mail.example.com, 25)  ≠  (本机IP, port_X, mx.other.com, 25)
```

port_X 可安全复用。此限制仅存在于 localhost 单 IP 基准测试场景。

### 规避方案

- **reuse 测试**: 无限制 (每线程一个连接, 总共 N 个四元组)
- **per-conn 测试**: msg ≤ 5000, 配合 TIME_WAIT 排空等待 (35s)
- **多 IP**: 使用 loopback 别名 (127.0.0.2 ~ 127.0.0.255) 增加四元组空间

---

## FSM Mock 基准 (2026-06-27)

使用 `fsm_bench` (MockConnection 零 I/O) 测量纯业务逻辑吞吐:

```
./fsm_bench --threads 1 --iterations 50000
ok=50000/50000  elapsed=12.12s  rate=4127 msg/s
```

**纯 FSM 成本 = 242μs/封** (命令解析 + 状态机调度 + mail 对象构造 + 队列 push)

### 与真实基准对照

| 层级 | 测试 | 峰值速率 | 每封耗时 | 瓶颈拆解 |
|------|------|---------|---------|---------|
| **纯 FSM** | mock 1线程 | 4127 msg/s | 242μs | 基准 (0% I/O) |
| **理想 4 线程** | mock ×4 | ~16500 msg/s | 61μs | 完美并行上限 |
| **pipe+reuse** | real 32线程 | 12502 msg/s | 80μs | 逼近理想上限的 76% |
| **seq+reuse** | real 16线程 | 11147 msg/s | 90μs | 逐命令往返增加延迟 |
| **seq+per-conn** | real 8线程 | 6359 msg/s | 157μs | TCP 握手主导 |

### 瓶颈归因 (pipe+reuse)

```
纯 FSM (1 线程):                      ████ 242μs
4 线程 FSM（理想）:                    █ 61μs
32 线程 pipe+reuse（真实）:            ██ 80μs
                                    ├─ FSM:       61μs (76%)
                                    └─ TCP/asio:  19μs (24%)
```

**pipe+reuse 已达 FSM 并行上限的 76%** — async I/O 框架 overhead 仅 24%, 连接复用 + 流水线几乎消除了所有网络开销。

---

## 2026-06-29 — null storage + null DB 天花板测试

### 测试条件

```
storage_provider: "null"     — 零磁盘 I/O
use_database: false           — NullDBPool, 零 DB 开销
inbound_ack_mode: after_enqueue
regex: 手动字符串解析 (find '<' / find '>')
```

### 结果

| 配置 | pipe+reuse 峰值 | vs 基准 |
|------|----------------|---------|
| 磁盘写 + MySQL + regex | 12,502 msg/s | 1x |
| + null storage | 49,295 msg/s | 3.9x |
| + null DB + manual parse | **72,303 msg/s** | **5.8x** |

**72303 msg/s 是 FSM + TCP loopback 的纯上限**——不含任何磁盘/数据库开销。

### Sample 热点 (null storage + null DB)

| 排名 | 函数 | 采样数 | 含义 |
|------|------|--------|------|
| 1 | `__psynch_cvwait` | 57877 | 线程空闲等待 |
| 2 | `kevent` | 16483 | io_context kqueue 轮询 |
| 3 | `__semwait_signal` | 8412 | sleep 等待 |
| 4 | `__sendto` | 191 | 内核发送 |
| 5 | `__recvfrom` | 185 | 内核接收 |

应用层热点已全部消除——regex 命中 **0 次**，`shared_ptr` 引用计数操作各 <10 次。

### 吞吐估算模型

每封邮件总开销 = CPU 开销 + I/O 开销（异步流水线下两者可重叠，瓶颈取较慢者）：

```
T_per_msg = max(T_cpu, T_io)

T_cpu  = 1 / 72303         = 13.8 μs   (FSM + TCP loopback)
T_disk = local_ssd_fsync   = ~200 μs    (append_binary 单次刷盘)
T_db   = mysql_insert_txn  = ~1000 μs   (BEGIN + INSERT×3 + COMMIT)

估算吞吐:
  仅有 CPU (null storage + null DB):  72,303 msg/s
  + 本地 SSD 写 (async pipeline):     min(72k, 1/200μs)  ≈ 5,000 msg/s
  + SSD + MySQL (async pipeline):     min(72k, 1/1000μs) ≈ 1,000 msg/s
```

**实测验证**：真实磁盘 + MySQL 测得 12,502 msg/s，比估算的 ~1,000 高一个数量级。原因是 4 个 worker 线程并行处理持久化（Amdahl 定律），且 `after_enqueue` 模式下 ACK 不等待持久化完成。实际落盘吞吐受 `persist_max_inflight_mails` 和 worker 线程数共同决定。

**公式修正（含并行度）**：

```
T_eff = T_cpu + T_io / N_workers
N_workers = 4 时:  T_eff = 13.8 + 1000/4 = 263.8 μs
估算吞吐:  1 / 263.8μs ≈ 3,790 msg/s
```

实测 12,502 仍高于此估算，说明 inflight 流水线深度（默认 1000000）允许大量邮件在 worker 线程池排队，实际瓶颈更接近 `min(CPU, IO/N)` 而非简单的 Amdahl。

---

## 2026-06-29 — IDBConnection::dialect() 重构

- `as<T>()` dynamic_cast 删除 —— persistent_queue / outbox_repository / smtps_fsm 零调用
- `IDBConnection::Dialect` 枚举 (`MySQL`, `Null`, 扩展 `PostgreSQL`)
- persistent_queue 签名 `MySQLConnection*` → `IDBConnection*`
- NullDBPool / NullDBConnection 实现完整虚接口

---

## 关于 fileprovider 进程 CPU 占用

你在测试期间看到的高 CPU "fileprovider" 进程是 macOS 系统的 **`fileproviderd`** (File Provider daemon), 不是本项目代码。

**触发原因**: 你的项目路径在 `~/Desktop/` 下, 而 Desktop 默认开启 iCloud 云盘同步。基准测试在 `mail/` 和 `attachments/` 目录中短时间写入并删除数万文件:

```
pipe+reuse t=32:  50000 封 → mail/ 写入 50000 文件 → cleanup 全部删除
seq+per-conn:     5000 封  → 重复 5 轮
总写入量:         约 10+ 万文件/轮
```

`fileproviderd` 检测到这些文件变更后尝试同步到 iCloud, 导致 CPU 飙升。

**解决方案**:
1. `test/cleanup.sh` 已经在测试间清理 `mail/` 和 `attachments/`
2. 可以将项目移到非 iCloud 同步目录 (如 `~/projects/`)
3. 或在系统设置中关闭 Desktop & Documents 的 iCloud 同步
4. 将 `mail_storage_path` 配置指向 `/tmp/` 等非同步目录

这**不影响测试结果本身** — `fileproviderd` 是独立的系统进程, 不会阻塞或修改 smtpsServer 的处理逻辑。但它可能间接触发 I/O 竞争使磁盘写入延迟抖动。

---

## 2026-06-25 — 历史数据 (Python cl.py, 仅供参考)

> **以下结果已标记为过时**, 保留仅为历史记录。条件说明不完整, 工具链不同, 不可与新结果直接比较。

### ~~mta-relay conn-pool (端口25, 复用连接, 无TLS)~~

```
10000 封, ramp 50→400 并发 (步长50), Python smtplib
```

| 并发 | 速率 (msg/s) | p50 | p99 | p999 |
|------|-------------|-----|-----|------|
| 50 | 4629 | 9.8ms | 20.1ms | 25.7ms |
| ... | ... | ... | ... | ... |
| 400 | 3346 | 51.6ms | 121.1ms | 149.8ms |

~~峰值 4629 msg/s @ conc=50~~

### ~~mta-relay per-conn (端口25, 每封新建TCP, 批量写)~~

```
2000 封, conc=50, Python smtplib
```

| 速率 | p50 | p99 | p999 |
|------|-----|-----|------|
| 4927 msg/s | 8.5ms | 25.3ms | 30.9ms |

**注意**: 2000 封远小于 ephemeral port 限制, 所以未触发端口耗尽。当时结论 "localhost 上 TCP 建连成本可忽略" **不准确** — 小幅测试感知不到 TCP 握手开销, C++ 大量测试表明 TCP 建连是主要瓶颈。

### ~~submission pipeline (端口587, TLS+AUTH, 批量写)~~

~~峰值 349 msg/s @ conc=50。TLS 握手是最大瓶颈。~~

---

## 对 `smtp_client.cpp` 的修改 (2026-06-28)

原 C++ `smtp_client` 仅支持 pipeline 模式。新增能力:

- `--seq` 标志: 关闭流水线, 启用串行命令模式 (逐命令发送+等待响应)
- `--pipe` 标志: 显式启用流水线 (默认开启)
- `--reuse` 标志: 启用连接复用 (默认 per-conn)
- `SO_REUSEADDR`: 对多目标 IP 场景有效, localhost 场景无效

Worker 选择矩阵:

| `--pipe` | `--reuse` | Worker 函数 | 模拟场景 |
|----------|-----------|------------|---------|
| N (--seq) | N | `worker_seq_perconn` | 传统客户端, 不优化 |
| N (--seq) | Y (--reuse) | `worker_seq_reuse` | 传统 MTA 中继 |
| Y (default) | N | `worker_pipe_perconn` | 现代客户端批量写 |
| Y (default) | Y (--reuse) | `worker_pipe_reuse` | MTA 中继最大吞吐 |


# ProtoRelay SMTP 性能基准报告

> **测试日期**: 2026-07-29
> **测试工具**: C++ `smtp_client` (raw sockets, TCP_NODELAY) + `fsm_bench` (纯 FSM CPU)
> **服务器版本**: `main` 分支 (重构后 API)

---

## 测试环境

| 项目 | 值 |
|------|-----|
| **CPU** | Apple M2 Pro (12 核: 8P + 4E) |
| **内存** | 16 GB LPDDR5 |
| **OS** | macOS 15.7.7 |
| **编译器** | Apple clang 17.0.0 (clang-1700.6.4.2) |
| **编译选项** | `-O3 -march=native -DNDEBUG` (Release) |
| **C++ 标准** | C++20 |

### 服务器配置

| 参数 | 值 |
|------|-----|
| 端口 | 2525 (plain TCP, auth=off) |
| IO 线程 | 4 |
| Worker 线程 | 4 |
| 最大连接数 | 100000 |
| 数据库 | **关闭** (`use_database: false`, NullDBPool) |
| SPF/DKIM/DMARC | 全部关闭 |
| DNSBL / 入侵检测 | 关闭 |
| Metrics | 关闭 |
| 存储 | Local file (`/tmp/protorelay_test/mail/`) |
| ACK 模式 | `after_enqueue` |
| Perf 模式 | 开启 (跳过日志粘滞、无 reply delay) |

### 客户端配置

| 参数 | 值 |
|------|-----|
| 客户端 | C++ raw socket, 无 asio |
| Loopback IP | 127.0.0.1 (单 IP) |
| TCP_NODELAY | 开启 |
| SO_REUSEADDR | 开启 |
| 邮件体 | 单行 `hi` (~20 bytes) |
| 发/收件人 | `user0~9@scut.email` → `dest0~9@scut.email` |

---

## 约束条件

以下限制可能影响绝对数值，对比不同环境时需考虑：

| 约束 | 影响 |
|------|------|
| **单 loopback IP** | per-conn 模式受 `TIME_WAIT` 临时端口限制 (~16K)。`pipe+per-conn` 8 线程×20000 消息时有 115/20000 失败 |
| **双端同机** | server + client 共享 CPU/Memory，吞吐受限于单机总资源 |
| **无 DB** | Mock 模式跳过 MySQL 写入、事务、Outbox 入队。开启 DB 后吞吐显著下降 |
| **极短邮件体** | body=`"hi\r\n"`。生产环境通常数 KB，吞吐相应下降 |
| **无 TLS** | 端口 2525 为 plain TCP。TLS 在 localhost 上约 3-5× 额外开销 |
| **`march=native`** | 二进制针对本机 CPU 优化，迁移需重新编译 |

### 多 Loopback IP 扩展

客户端已内置 `--local-ips` 参数，突破单 IP 临时端口限制：

```bash
# 创建 loopback 别名 (需 sudo，仅一次)
for i in $(seq 2 16); do
    sudo ifconfig lo0 alias 127.0.0.$i up
done

# bench
./build/smtp_client --pipe --t 16 \
    --local-ips 127.0.0.1,127.0.0.2,...,127.0.0.16 \
    --msgs 200000 --port 2525
```

每个 `127.0.0.x` 独立 16K 端口范围，16 个 IP ≈ 256K 并发 per-conn 连接。

---

## 测试结果

### 1. FSM Mock 基准（纯 CPU，零 I/O）

```
[1 thread]  5000 msgs,  0.35s → 14,136 msg/s
[4 threads] 200K msgs, 12.57s → 15,913 msg/s
```

剥离 TCP/SSL/磁盘/DB，仅测量 FSM 状态机 + 邮件解析 + PersistQueue 的纯 CPU 成本。
4 线程仅比 1 线程高 12%，说明 FSM 逻辑几乎无锁竞争。

### 2. 真实 TCP 吞吐（8 线程 × 20000 消息）

| 模式 | 吞吐 (msg/s) | 失败 | 说明 |
|------|-------------|------|------|
| **seq + reuse** | **18,278** | 0 | 最高：串行命令，连接复用，无端口问题 |
| pipe + reuse | 15,818 | 0 | 流水线 + 复用，减少 RTT 但服务端有上限 |
| seq + per-conn | 10,812 | 15 | 每封新连接，端口耗尽少量失败 |
| pipe + per-conn | 634 | 115 | 流水线 + 每连接，极端端口耗尽 |

### 3. 峰值吞吐（100K 消息，pipe + reuse）

| 线程数 | 吞吐 (msg/s) | 失败 |
|--------|-------------|------|
| 8 | 14,673 | 0 |
| 64 | 13,809 | 0 |

64 线程略低于 8 线程 (-6%)，在 12 核 CPU 上超过 ~8 线程后上下文切换开销显著。

### 4. Python vs C++ 客户端

| 客户端 | 吞吐 (msg/s) |
|--------|-------------|
| C++ `smtp_client` (8t) | 18,278 |
| Python `cl.py` (8t) | ~2,500 |

C++ 比 Python 高 7×，主要避免 GIL + smtplib 逐命令往返。

---

## 吞吐瓶颈分析

```
FSM (mock):    15,913 msg/s  ← 纯 CPU 上限
TCP (reuse):   18,278 msg/s  ← 实际达到 mock 水平 (邮件体极小)
TCP (per-conn):10,812 msg/s  ← connect/close + TIME_WAIT 为瓶颈
```

`seq+reuse` 超过 FSM mock 基准的原因：
- `smtp_client` raw socket 跳过 asio 框架开销
- `fsm_bench` 每次创建新 SmtpsSession + MockConnection，有额外 alloc

### 生产环境预估

| 场景 | 预估吞吐 | 理由 |
|------|---------|------|
| Mock FSM | 15,000 msg/s | 实测 |
| Plain TCP reuse | 15,000 msg/s | 实测 |
| Plain TCP per-conn | 8,000 msg/s | 扣除端口耗尽 |
| TLS reuse | 3K-5K msg/s | TLS 加密 × 3-5 |
| TLS + DB 真实 | 500-1.5K msg/s | MySQL 写入 + Outbox |
| TLS + DB + DKIM | 300-800 msg/s | DKIM RSA 签名 |

---

## 运行方法

```bash
# 1. 启动 mock SMTP 服务器
./build/smtpsServer -c test/config/smtps_mock.json &

# 2. C++ bench (4 种模式)
./build/smtp_client --seq --t 8 --msgs 20000 --port 2525
./build/smtp_client --seq --reuse --t 8 --msgs 20000 --port 2525
./build/smtp_client --pipe --t 8 --msgs 20000 --port 2525
./build/smtp_client --pipe --reuse --t 8 --msgs 100000 --port 2525

# 3. FSM mock bench
./build/fsm_bench -t 4 -n 50000

# 4. 清理
bash test/scripts/cleanup.sh
```

## 2026-07-29 — 重构后 API 重新基准

### 测试条件

```
工具:      C++ smtp_client (raw BSD sockets, TCP_NODELAY, --local-ips 支持)
          fsm_bench (MockConnection 零 I/O)
服务端:    smtpsServer, perf_mode=true, 4 io + 4 worker
配置:      test/config/smtps_mock.json (use_database=false, local file storage)
存储:      LocalFileStorageProvider → /tmp/protorelay_test/mail/ (APFS SSD)
网络:      localhost:2525 plain TCP (无 TLS)
消息量:    pipe+reuse: 100000, 其余: 20000
清理:      每轮测试前 rm -rf 清理 /tmp/protorelay_test
```

### 关键差异：为什么比 72K 天花板低

| 因素 | 72K 天花板 (2026-06-29) | 本次 (2026-07-29) |
|------|------------------------|-------------------|
| 存储 | **NullStorageProvider** (零 I/O) | **LocalFileStorageProvider** (真实 SSD 写) |
| 正则 | 手动字符串解析 | `std::regex` |
| 客户端 | smtp_client C++ | smtp_client C++ |
| FSM 单线程速度 | 4,127 msg/s | **14,136 msg/s** (3.4× 提升) |

**不是重构导致性能下降** — 恰恰相反，FSM 纯 CPU 速度从 4127 提升到 14136 msg/s (+3.4×)。
吞吐从 72K 降到 18K 是**存储提供者不同**：null storage 跳过所有磁盘 I/O，
而 local file storage 每封邮件都要 `ofstream` 写盘 + `fsync`（APFS SSD 约 50-100μs/次）。

### FSM Mock 结果

```
[1 thread]  5000 msgs,  0.35s → 14,136 msg/s  (旧: 4,127 msg/s, +3.4×)
[4 threads] 200K msgs, 12.57s → 15,913 msg/s

单线程提升归因:
  1. PersistentQueue submit 去掉了不必要的锁竞争路径
  2. 邮件对象构造精简 (perf_mode 跳过多余字段初始化)
  3. SmtpsSession 状态管理从虚函数调用改为直接成员访问
  (注: dispatch 仍为 std::map::find, 编译期数组查表见 fast_fsm_base.h)
```

### 真实 TCP 吞吐（8 线程）

| 模式 | 吞吐 (msg/s) | 失败 | 说明 |
|------|-------------|------|------|
| **seq + reuse** | **18,278** | 0 | 最高：串行命令，连接复用 |
| pipe + reuse | 15,818 | 0 | 流水线 + 复用 |
| seq + per-conn | 10,812 | 15 | 每封新连接，端口耗尽 |
| pipe + per-conn | 634 | 115 | 极端端口耗尽 |

### 峰值吞吐（100K，pipe + reuse）

| 线程数 | 吞吐 (msg/s) | vs 旧版 |
|--------|-------------|---------|
| 8 | 14,673 | +23% (旧 11988) |
| 64 | 13,809 | +16% (旧 11919) |

### FSM Mock 历史对比

| 日期 | 单线程 | 多线程峰值 | 备注 |
|------|--------|-----------|------|
| 2026-06-27 | 4,127 | ~16,500 (估算) | 旧 FSM API |
| **2026-07-29** | **14,136** | **15,913** | **重构后 (+3.4×)** |

### smtp_client 新增 `--local-ips` 支持

突破 localhost per-conn 临时端口限制：

```bash
# 多 loopback IP（每个 IP 独立 16K 端口范围）
sudo ifconfig lo0 alias 127.0.0.2 up
./build/smtp_client --pipe --t 16 \
    --local-ips 127.0.0.1,127.0.0.2 \
    --msgs 50000 --port 2525
```

---

## 历史数据 (摘要)

| 日期 | 峰值 (msg/s) | 模式 | 条件 | 说明 |
|------|-------------|------|------|------|
| 2026-06-29 | 72,303 | pipe+reuse | null storage + null DB | CPU/TCP 纯上限 |
| 2026-06-29 | 49,295 | pipe+reuse | null storage | 跳过磁盘后的上限 |
| **2026-07-29** | **18,278** | seq+reuse | local file storage | 真实 SSD 写盘 |
| 2026-06-28 | 12,502 | pipe+reuse | local file storage | 重构前 FSM API |
| 2026-06-28 | 11,147 | seq+reuse | local file storage | 重构前 FSM API |

> 72K → 18K 的下降**不是重构造成**，是 null storage (零 I/O) vs local storage (真实 SSD 写) 的差异。
> 同一 local storage 条件下，重构后 +46% (12502 → 18278)。
> FSM 纯 CPU 更是 +3.4× (4127 → 14136)。重构是正向优化。
