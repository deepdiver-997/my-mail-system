# Mock 空 executor 导致 FSM 全套测试静默挂红（bad executor）

- 日期：2026-09-04 发现并修复
- 影响：`imaps_fsm_test` / `smtps_fsm_test` / `smtps_fsm_concurrency_test` / `pop3_fsm_test`
  四个套件全部失败（pop3 仅 2/33），**带病时间 ≥ 2026-09-01，跨多个提交无人发现**
- 生产：无影响（真实连接返回的是 socket 的有效 executor）

## 现象

四个 FSM 测试在 session 起来的第一拍抛
`boost::wrapexcept<boost::asio::execution::bad_executor>`，串行套件直接 FATAL 中止，
pop3 套件被 harness 逐用例吞掉后 31 个用例全部标 FAIL。

## 排查

用 worktree 在历史提交上对照编译运行：

1. **d83279d（IO 线程池 B 方案 0f4bb45 的父提交）**：pop3_fsm_test 同样 2 passed / 31 failed
   → 排除"IO 线程池从单 io_context 改多 io_context"和 webServer 引入的可能。
2. 82a0149~1（watchdog 之前）就已报 bad executor → 比 watchdog 更早的框架路径
   （如 `IConnection::async_write_with_delay` 默认实现会用连接 executor 建 timer）同样踩雷。
   watchdog（7c2f26b / 00289f1 / 82a0149，2026-09-01）只是把它扩大成"每个 session 第一拍必炸"。

## 根因

契约变更 + mock 未同步：

- 框架把 `IConnection::get_executor()` 的用法从"偶尔拿来建 timer"升级为
  "**可以无条件 post 的活动 executor**"（watchdog `SessionBase::rearm()` 在 session
  启动和每条入站命令都 post 一次）。
- 生产实现（`TcpConnection`/`SslConnection`）天然满足；**测试 mock 返回的是默认构造的
  空 `any_io_executor{}`**——对空 executor 执行 post 是 asio 定义的抛出点。
- 编译期无任何告警；跑全套的人不存在 → 静默挂红一个多星期。

## 修复（只动 mock，不碰框架）

`test/unit/mock_connection.h`：

1. `get_executor()` 返回 mock 私有 `io_context` 的**真实** executor：watchdog 的 post
   入队但测试不驱动 → watchdog 在单测中惰性挂起，不再炸。
2. `close()` 末尾 `exec_ctx_.poll()`：排空队列，让捕获了 `shared_from_this` 的 rearm
   lambda 进去被 `closed_` 短路释放——否则 session 关闭后仍被队列钉住，
   `weak.expired()` 类的生命周期断言（并发测试边界用例）过不去。

修复后全套 23/23 通过（pop3 33/33）。

## 教训（流程规则）

1. **改共享契约接口时，必须同步审计所有实现方，测试 mock 也是实现方。**
   `get_executor()` 返回"空对象不炸"到"必须可用"的语义变化，唯一不会跟着改的地方就是 mock。
   审计方法：改接口后 grep 全部 override，逐个确认满足新语义。
2. **大改动 / 框架层改动，提交前必须跑全套测试**：
   `bash test/run_all_tests.sh`（23 个单测，增量构建约 1-2 分钟）。
   只跑自己改动的 target 检测不到间接破坏；套件红着合码 = 后续所有回归信号全部失真。
