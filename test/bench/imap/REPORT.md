# IMAP 读路径基线（2026-08-29）

> **目的**：DB 真异步（Phase 2，database-async-design.md）的 **before** 基线。验证"io 线程
> 同步死等 MySQL/storage"这一热点假设，并为异步改造提供对照值。
>
> **工具**：`test/bench/imap/imap_client.cpp`（C++ raw socket 多连接并发；LOGIN→SELECT→FETCH 循环）。
> `test/bench/imap/seed_imap_data.py`（灌 200 封测试邮件）。
> **环境**：macOS ARM64, localhost, 4 io + 4 worker, `achieve=mysql`（当前同步 DB 路径）,
> local storage, mailbox 200 封（每封 2KB body）。
> **每轮** = SELECT INBOX + FETCH 1:200 (FLAGS RFC822.SIZE)。读场景不产生新数据，无需清理。

## 结论：热点确认 —— io 线程同步阻塞是吞吐天花板

| 并发连接 | 吞吐 (rounds/s) | P50 延迟 (ms) | P95 (ms) | P99 (ms) |
|---------|----------------|--------------|---------|---------|
| 1 | ~300–540 | 1.8–3.3 | 2.4–3.6 | 3.1–5.3 |
| 4 | ~740–1390 | 2.8–5.1 | 4.0–7.8 | 4.6–8.4 |
| 16 | ~770–1470 | 10.8–20.8 | 13.1–24.9 | 14.1–27.6 |
| 64 | ~760–1460 | 43.8–84.0 | 47.1–92.5 | 49.2–98.5 |

（两次采样因本机负载波动区间不同，但形态一致。）

**形态**：并发 > 4（= io 线程数）后吞吐**封顶**，延迟随排队线性上涨。原因：

- 每轮 FETCH 的 `get_mailbox_mails`（1 次 DB 查询）+ 200 次 `object_size`（storage 读）都在
  io 线程上**同步内联**执行——查询期间该 io 线程无法服务任何其他连接。
- 并发在途查询上限 = io 线程数（4）；超出就排队（延迟涨、吞吐平）。
- 压测中 4 个 io 线程各 ~40% CPU，但大部分时间**阻塞在 DB/storage 系统调用上**（非计算忙），
  证实是 I/O 等待而非 CPU 瓶颈。

## 对照：SELECT-only（stats 缓存命中，近零 DB）

| 并发 | 吞吐 (rounds/s) | P50 (ms) |
|-----|----------------|---------|
| 4 | 44,663 | 0.067 |
| 16 | 91,072 | 0.127 |

SELECT 的 stats 查询走缓存后接近零成本 → **瓶颈不在 SELECT，在 FETCH 的 DB 列表查询 + storage 读**。

## 预期：Phase 2（DB 真异步）应把吞吐天花板从 ~4×单线程速率抬到连接池上限（128），
且 io 线程不再被查询期间卡死。用本表做 before/after 对照。

## Phase 2 后（2026-08-29，`achieve=mariadb` 非阻塞 async）

> 同一 200-mail 邮箱，同日同期复测。对照 `achieve=mysql`（sync）与 mariadb（async）。

| 并发 | mysql sync (rounds/s) | mariadb async (rounds/s) | P50 (ms, async) |
|------|----------------------|--------------------------|-----------------|
| 1 | 609 | 586 | 1.56 |
| 4 | 1816 | 1925 | 2.02 |
| 8 | 1910 | 1891 | 4.19 |
| 16 | 1975 | 1968 | 7.99 |

**结论**：两引擎吞吐持平、都封顶 ~1950 rps。瓶颈是每轮**非 DB 的 io 线程工作**
（200 次 storage 读 + 200 行响应组装/写回），不是 DB 阻塞；本地 DB socket 立即可读，
非阻塞查询也走内联完成（rc=0 无 wait），io 线程 CPU 并不因 async 降低。**async 的收益
在远程/慢 DB 上才显现**——socket 不就绪时 io 线程在 `async_wait` 期间真正让出。
SELECT-only（缓存命中近零 DB）两引擎均 ~40k rps。

**顺带修掉的真 bug：async op 连接泄漏**（Phase 2 实施时发现）：`done` 捕获 op 自身构成
shared_ptr 循环 → 每个 async 查询泄漏一条连接 → 池耗尽 → io 线程卡 5s。压测复现
89 acquire / 1 release。修后 acquire=release。回归单测 `mariadb_async_test`
（50 查询后池 available 恢复基线）。

## ⚠ 顺带发现的生产 bug：FETCH 续作链栈溢出

FETCH 大邮箱（>~200 封）会 **SIGSEGV**：`fetch_complete_mail_with_body` 栈溢出。
本地 storage 的 `async_object_size/read` 回调**内联**触发 → `fetch_drive` 被逐封重入（真递归），
500 封即爆栈（lldb 确认 EXC_BAD_ACCESS code=2，栈指针落在 guard 页，无法 unwind）。
`server_base.cpp:137` 的设计注释说"本地内联 µs 级"，`fetch_drive` 注释称"万封不爆栈"——两者
在内联回调下矛盾。**已修复（2026-08-29，`fetch_drive` 续作链迭代化）**：每封共享一个 `std::atomic<bool> alive`，
size/body 两个异步读共用；完成本封的最后一步走 `fetch_continue`——inline 回调（外层
`fetch_drive` 帧仍在）→ 外层循环 continue；deferred 回调 → 驱动下一封。本地 storage
保持内联（不加线程投递开销）。回归单测 `fetch_many_mails_no_stack_overflow`（400 封）+
实测 FETCH 1:2000 正常。




## Profile 热点（2026-08-29 03:03，Darwin，采样 10s，负载: --t 16 --conns 4 --rounds 2000）

> 由 `test/bench/imap/profile.sh` 生成（release 构建，mysql 引擎）。`__psynch_cvwait`/`kevent`
> 是 asio reactor 的空闲等待（正常）；`stat`=storage 读、`__recvfrom`=网络读、
> `MySQLResult::get_value`=DB 结果解析是真实应用热点。

  wall=48.4677s  rounds=128000  throughput=2640.93 rounds/s

| 采样计数 | 热点函数 |
|---------|---------|
| 33596 | `__psynch_cvwait` |
| 20189 | `__recvfrom` |
| 8445 | `kevent` |
| 8396 | `__sigwait` |
| 8396 | `__semwait_signal` |
| 8119 | `stat` |
| 550 | `_platform_memmove` |
| 441 | `__sendto` |
| 291 | `mail_system::MySQLResult::get_value(unsigned long, std::basic_string<char> const&) const` |
| 258 | `_nanov2_free` |
| 203 | `_platform_memcmp` |
| 190 | `__psynch_mutexwait` |
