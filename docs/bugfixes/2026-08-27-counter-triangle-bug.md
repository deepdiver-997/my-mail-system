# 2026-08-27 Counter 三角形 bug 修复

## 背景

`/metrics` 端点暴露的三个核心 counter 长期**数值错误**，但没人发现 —— 因为：

1. 项目此前**根本没有任何代码主动读 `/metrics`**，只在 e2e `test_dual_server.py` 启动时 `curl` 一下确认 200 OK，不验内容
2. 原 unit test `server_base_test.cpp:316` 单测 `push_metric_counter("mails_accepted_total", {}, 42)` **只推一次**，抓不到多次累加 bug
3. counter 实际渲染成 `N*(N+1)/2`（三角形数），数字本身**增长趋势是对的**（递增），人眼看不出来

2026-08-27 增 metrics 时起 e2e `test_metrics_exposure.py`，对比 baseline + N 条 EHLO/QUIT 前后的 counter diff，**diff == 6 而不是 3** 才暴露。

## 根因

`MetricsServer::inc_counter(name, labels, delta)` 语义是 **additive delta**：

```cpp
// include/framework/metrics_server.h (示意)
void inc_counter(const std::string& name, const LabelMap& labels, uint64_t delta) {
    std::unique_lock lock(mutex_);
    auto& c = counters_[make_key(name, labels)];
    c.value += delta;          // ← 累加 delta
    if (c.help.empty()) c.help = build_help(name);
}
```

`ServerBase` 三个 `increment_*` helper 把 `fetch_add` 的**累计值**误当 delta 传入：

```cpp
// src/framework/server_base.cpp:283-298（bug 版）
void ServerBase::increment_connections_total() {
    auto v = connections_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    push_metric_counter("protorelay_connections_total", {}, v);  // ← v 是 1, 2, 3, ...
}
void ServerBase::increment_connections_rejected() {
    auto v = connections_rejected_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    push_metric_counter("protorelay_connections_rejected_total", {}, v);
}
void ServerBase::increment_mails_accepted() {
    auto v = mails_accepted_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    push_metric_counter("protorelay_mails_accepted_total", {}, v);
}
```

调用模式 = `1+2+3+...+N = N*(N+1)/2`（三角形数）。`/metrics` 渲染出错的数字。

| 实际 N | 错误渲染 | 正确应为 |
|---|---|---|
| 1 | 1 | 1 |
| 3 | 6 | 3 |
| 10 | 55 | 10 |
| 100 | 5050 | 100 |
| 1000 | 500500 | 1000 |

## 修复

3 行改：`push_metric_counter` 改传 `1`（delta），atomic 累加仍用 `fetch_add` 但**返回值丢弃**。

```cpp
// src/framework/server_base.cpp:283-298（修复版）
void ServerBase::increment_connections_total() {
    connections_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_connections_total", {}, 1);  // delta=1
}
void ServerBase::increment_connections_rejected() {
    connections_rejected_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_connections_rejected_total", {}, 1);
}
void ServerBase::increment_mails_accepted() {
    mails_accepted_total_.fetch_add(1, std::memory_order_relaxed);
    push_metric_counter("protorelay_mails_accepted_total", {}, 1);
}
```

`MetricsServer::inc_counter` 接口**不变**（保持 additive 语义单一），所有调用点都按 `delta=1`（或 `delta=N` 一次性增 N）写，零学习成本。

## 不变量回归测试

`test/unit/metrics_core_test.cpp` 三个 case 把核心 API 的不变量钉死：

```cpp
// 1. counter 增量：N 次 inc delta=1 ⇒ 终值 = N（不是三角形数）
for (int i = 0; i < 100; ++i)
    m.inc_counter("mails_accepted_total", {}, 1);
assert(find_value(m.scrape_text(), "mails_accepted_total") == 100);  // 不是 5050

// 1b. counter 多 label：按 label 维度独立累加
m.inc_counter("outbound_attempts_total", {{"domain", "a.local"}}, 1);
m.inc_counter("outbound_attempts_total", {{"domain", "a.local"}}, 1);
m.inc_counter("outbound_attempts_total", {{"domain", "b.local"}}, 1);
// a.local == 2, b.local == 1

// 2. gauge：后写覆盖前写
m.set_gauge("active_connections", {}, 1);
m.set_gauge("active_connections", {}, 5);  // 1 被覆盖
// == 5

// 3. histogram：sum/count 累加
m.observe("request_latency_ms", {}, 2.0);
m.observe("request_latency_ms", {}, 3.0);
// _sum == 5, _count == 2
```

不依赖 gtest / DB / filesystem，3/3 PASS。

**e2e 兜底**（`test/e2e/test_metrics_exposure.py`）：起真 server → 3 条 EHLO/QUIT → 拉 `/metrics` → 断言 `protorelay_connections_total` diff == 3（不是三角形 6）。即使单元层漏了，e2e 会抓到。7/7 PASS。

## 为什么 unit test 抓不到

`test/unit/server_base_test.cpp:316` 原单测只验 1 次 push 的语义：

```cpp
// 旧单测：只推 1 次 → counter == 42 这种"绝对值"语义混淆
m_server->push_metric_counter("mails_accepted_total", {}, 42);
EXPECT_EQ(server->get_mails_accepted_total_for_test(), 1);  // ← atomic 仍是 1
```

**单测从来没验过「多次 push 后 counter 终值 == 累加的 delta 总和」**。bug 只在 `inc_counter` 被调 ≥2 次时显现，单测一次调当然看不出。3 个 helper 调的频率（每次 SMTP 连接）又恰好 ≥2，所以生产**一直在错**，只是数字递增，监控/告警/人眼都没察觉。

教训：**单测的「1 次调用」覆盖 ≠ 「N 次累加」正确**。counter 类型的测试必须显式跑多次累加 + 断言 `value == N * delta` 形式的不变量，否则 bug 跟单测无关。

## 类似陷阱清单（其它不能传累计值的 API）

| API | 语义 | 错误用法 |
|---|---|---|
| `MetricsServer::inc_counter` | add delta | 传累计值 → 三角形 |
| `std::atomic::fetch_add(x).load()` | **fetch 后值（累加后）** | 当 pre 值用 → off by 1 |
| `std::map::operator[] = v` | set | 在循环里 `m[k] += v` 写错语义 |
| Prometheus text `name k` | set absolute | 误把 counter 当 gauge 写 |
| spdlog `flush_on(warn)` | flush ≥ warn | 误以为 info 也 flush → 监控日志读不到 |

## 相关提交

- `8d73f81 fix(metrics): counter increment is additive, not cumulative` — 3 行 fix + unit test
- `5c68387 feat(metrics): add IntrusionDetector observability + e2e test_metrics_exposure` — e2e 兜底

## 后续

- `MetricsServer` 接口保持 additive 单一语义；如需 set absolute 加 `set_counter`（与 `set_gauge` 平行）
- 考虑在 `inc_counter` 接口注释里强调"delta"语义 + 写一段使用示例（避免下一个新人重蹈）
- 类似「1 次调用正确 ≠ N 次累加正确」的测试盲区，扩到 `PersistentQueue::enqueue` 计数、`IntrusionDetector::record_session` 累加等场景
