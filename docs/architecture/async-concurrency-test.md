# SMTP 入站异步并发测试方案

> 下个会话可直接开始的任务。目标：为入站校验/FSM 的异步路径建立**真实多线程并发测试**，暴露 session 状态竞争与裸指针悬垂，配合 TSan 检测数据竞争。

## 背景

入站校验已异步化：MAIL FROM 的 SPF、RCPT TO 的用户存在性（`user_exists_async`/`auth_user_async`）、DATA_END 的完整校验（`verify_all_from_file_async`）都用底层原生异步接口 + 回调，靠 `set_paused` 串行化（session 状态单线程访问）。

**隐患**：这些异步回调的真实线程模型（c-ares 线程、DB worker 线程）与 session 所在 IO 线程的并发，当前测试**完全没覆盖**。

## 现状与局限

| 组件 | 现状 | 问题 |
|---|---|---|
| `test/unit/mock_dns_resolver.h` | `async_resolve_*` **同步**调用回调 | 无延迟/无线程，测不出跨线程竞态 |
| `test/unit/mock_connection.h` | 零 I/O，async_* **同步**回调 | 同上 |
| `test/unit/smtps_fsm_test.cpp` | 已建 `IOThreadPool(1)` + `BoostThreadPool(2)`，但 session 操作同步完成 | 线程池存在但未用于异步时序 |
| 单测执行 | 串行单线程 | 无法触发"session 已析构后回调"等边界 |

**已有修复需回归保护**：`auth_user_async`/`user_exists_async` 已改 `shared_ptr` 保活 session；若将来切真异步 DB，需测试防止裸指针悬垂回归。

## 方案：增强 mock 支持异步延迟回调（不是重写）

保留 MockConnection/MockDnsResolver 的零 I/O 特性，改造核心：

### 1. MockDnsResolver：异步延迟回调
`async_resolve_txt/mx/host/ptr` 不再同步调用回调，而是：
- **保存回调**（按 domain 存入 pending map），由测试**手动触发**（deterministic，避免随机睡眠 flaky）；或
- 投递到独立线程 + **固定小延迟**（如 1-5ms）后回调

两种模式（手动触发 vs 自动延迟）用开关切换，测试按需选择。

### 2. MockConnection：异步延迟回调
`async_read/async_write/async_query` 同样支持延迟/手动触发回调，模拟 DB worker 线程延迟。

### 3. 测试驱动：session 跑真实 IO 线程
```
主线程(协调) ──创建 session(shared_ptr)──► IOThreadPool 的 io_context 线程
   │ 轮询 atomic 完成标志                    │ run: 处理命令/状态机
   │                                        │ 发 async_query/resolve(存回调)
   │ 手动触发 mock 回调 ◄────────────────────┘ (模拟 c-ares/DB 线程)
   └── session 完成 → 销毁 → 验证不悬垂/无竞态
```

### 4. TSan 集成
编译测试目标加 `-fsanitize=thread -g -O1`，运行检测数据竞争（比随机睡眠并发测试可靠）。

## 关键文件

- `test/unit/mock_dns_resolver.h` — 增强异步延迟/手动触发
- `test/unit/mock_connection.h` — 增强 async_read/write/query 延迟回调
- `test/unit/smtps_fsm_test.cpp` — 新增并发测试用例
- `include/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp` — 被测（MAIL FROM SPF / RCPT user_exists / DATA_END verify）
- `src/mail_system/back/inbound/inbound_verifier.cpp` — 被测（CPS 回调链）

## 实施步骤

1. **增强 MockDnsResolver**：`async_resolve_*` 支持"手动触发回调"模式（保存回调 + `fire(domain)`），或"自动延迟"模式（线程 + sleep）
2. **增强 MockConnection**：`async_read/write/query` 支持延迟回调（DB 场景）
3. **写并发用例**（smtps_fsm_test 或新文件）：
   - MAIL FROM SPF 异步：发起 → 手动触发 DNS 回调 → 断言 250/550
   - RCPT user_exists：发起 → 手动触发 DB 回调（延迟）→ 断言
   - DATA_END verify：发起 → 手动触发 DNS 链 → 断言入队 + 250
   - **关键边界**：回调触发时 session 已"逻辑完成"（模拟 io_context 即将关闭）→ 验证 shared_ptr 保活不悬垂
4. **TSan 构建**：给测试目标加 `-fsanitize=thread`
5. **验证**：TSan 无 data race 报告；逻辑断言通过

## 验收标准

- [ ] 并发测试在 TSan 下无 `data race` 报告
- [ ] 覆盖三条异步路径（SPF / user_exists / verify_all_from_file）
- [ ] "回调晚于 session 结束"边界用例通过（shared_ptr 保活有效）
- [ ] 现有串行单测仍通过（86+）

## 参考

- 现有串行测试：`test/unit/smtps_fsm_test.cpp`（夹具已建 IOThreadPool/BoostThreadPool）
- 已修复的保活模式：`auth_user_async`/`user_exists_async`（shared_ptr + set_paused）
- TSan：`-fsanitize=thread`（GCC/Clang），检测跨线程数据竞争
