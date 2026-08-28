# 性能测试（bench）索引

按协议组织：每个协议一个子目录，吞吐基准 + 采样热点分析 + 报告放一起。

```
test/bench/
  README.md                  ← 本文件
  run_bench_all.sh           跑全部压测（SMTP 优先，见文件内注释）
  smtp/
    smtp_client.cpp          SMTP 压测客户端（raw socket，线程×连接）
    bench.sh                 SMTP 压测启动器（ramp 并发找峰值）
    REPORT.md                SMTP 吞吐基准报告（历史全量）
  imap/
    imap_client.cpp          IMAP 读路径压测客户端（SELECT+FETCH 循环）
    seed_imap_data.py        灌测试邮件（INSERT IGNORE 幂等）
    profile.sh               高负载采样找热点（macOS sample / Linux perf）
    REPORT.md                IMAP 吞吐基准 + Phase 2 对照 + 热点报告
  fsm/
    fsm_bench.cpp            纯 FSM 基准（MockConnection 零 I/O）
    dispatch_bench.cpp       分发器基准
```

## 快速开始

```bash
# 构建压测客户端（release 用于 profiling 更准）
cmake --build build --target imap_client smtp_client fsm_bench dispatch_bench -j 4
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target imapsServer imap_client -j 4

# IMAP 读路径吞吐基准（SELECT+FETCH）
python3 test/bench/imap/seed_imap_data.py --mails 200          # 首次灌数据
./build/imap_client --t 16 --conns 4 --rounds 500              # 高并发吞吐
./build/imap_client --t 4 --conns 1 --rounds 500 --select-only # 纯 SELECT 对照

# IMAP 高负载采样找热点（append 进 imap/REPORT.md）
# 需先启动一个 imapsServer（配置见 profile.sh 的 --config / env CONFIG）
./test/bench/imap/profile.sh --sample-secs 10 --topn 20

# SMTP 峰值（见 smtp/REPORT.md 的"运行方法"）
./test/bench/smtp/bench.sh
```

## 约定

- **吞吐基准**：`*_client.cpp` 输出 rounds/s + P50/P95/P99，结果记录在各协议 `REPORT.md`。
- **热点分析**：`profile.sh` 采样服务 PID → 折叠 top-N 热点表 → append 进 `imap/REPORT.md`。
  **必须 profile release 构建**（Debug 热点误导），采样窗口要短于压测时长（否则后半段全是空闲帧）。
- **数据准备**：IMAP 读路径不产生新数据（`seed_imap_data.py` 只灌一次，重复跑是重读）；
  压测邮箱大小影响结果（`get_mailbox_mails` 返回全量），对照时保持同一邮箱规模。
- **平台**：macOS 用内置 `sample`（零依赖）；Linux 用 `perf`（`perf record -p` 通常要 sudo）。
