# POP3 读路径基准报告

> 工具：`test/bench/pop3/pop3_client.cpp`（raw socket，STAT+LIST+RETR 循环）、
> `test/bench/pop3/profile.sh`（高负载采样找热点）。POP3 与 IMAP 共用同一份
> mailbox 数据（`mail_mailbox` 表），seed 复用 `test/bench/imap/seed_imap_data.py`。

## ⚠ POP3 的并发约束：单会话锁

POP3 **每用户同时只允许一个会话**（`pop3_session_lock` 表，异会话抢锁返回
`[IN-USE] Mailbox lock busy`）。因此：

- **并发连接数 ≤ 用户数**，否则后连的会话抢锁失败。压测必须用 `--users` 给每条连接
  配不同用户（`pop3_client` 默认 `--users` 轮询）。
- 被 kill 的会话会留锁约 **5 分钟**（心跳清扫阈值），快速重跑前先清：
  `mysql -e "DELETE FROM pop3_session_lock"`。

## 运行

```bash
# 构建 + 灌数据（POP3 与 IMAP 共用 mailbox；给多用户灌测数据绕过锁）
cmake --build build --target pop3_client -j 4
python3 test/bench/imap/seed_imap_data.py --mails 200 --mailbox 6
python3 test/bench/imap/seed_imap_data.py --user t1@scut.email --mailbox 11 --mails 50

# 起 pop3Server（DB-backed bench 配置，端口 1110 避开 <1024 需 root）
build/pop3Server -c /tmp/pop3_bench_config.json

# 吞吐（多用户并发绕过单会话锁）
USERS="test2@scut.email,t1@scut.email,test@scut.email,alice@a.local,bob@b.local"
./build/pop3_client --port 1110 --users "$USERS" --t 4 --conns 1 --rounds 50 --mails 5

# 高负载采样找热点（append 进本文件）
./test/bench/pop3/profile.sh --config /tmp/pop3_bench_config.json \
    --load-args "--port 1110 --users \"$USERS\" --t 4 --conns 1 --rounds 500 --mails 5"
```

## 吞吐基准（2026-08-29，5 用户，debug 构建）

> 每轮 = STAT + LIST + RETR 1:5。并发 ≤ 用户数时随并发涨；超出即抢锁失败。

| 并发（=用户数） | rounds/s | P50 (ms) | P95 (ms) | P99 (ms) |
|---------------|---------|----------|----------|----------|
| 4 | 260 | 1.27 | 2.16 | 3.09 |

## Profile 热点

_（由 `profile.sh` 自动 append）_
