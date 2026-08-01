# ProtoRelay 测试文档

## 测试架构

```
test/
├── unit/                        # C++ 单元测试（零 I/O，MockConnection）
│   ├── smtps_fsm_test.cpp       # SMTP FSM 状态机测试 (26 tests)
│   ├── imaps_fsm_test.cpp       # IMAP FSM 状态机测试 (30 tests)
│   ├── test_inbound_verifier.cpp# InboundVerifier 组件测试 (55 tests)
│   ├── sql_queries_test.cpp     # SQL 查询生成器测试 (30 tests)
│   ├── outbound_smoke.cpp       # 出站类型/FSM 烟雾测试
│   ├── mock_connection.h        # 零 I/O Mock 连接
│   ├── mock_dns_resolver.h      # Mock DNS 解析器
│   └── mock_outbound_stream.h   # 出站流 Mock
├── bench/                       # 性能基准测试
│   ├── fsm_bench.cpp            # FSM 吞吐基准
│   ├── smtp_client.cpp          # 高性能 SMTP 客户端
│   ├── bench.sh / run_bench_all.sh
│   └── bench-report.md
├── e2e/                         # Python 端到端测试
│   ├── test_smtp_flow.py        # SMTP 全流程测试 (端口 25/465/587)
│   ├── test_dual_server.py      # 双服务器互通测试 (带 static route)
│   ├── test_outbound.py         # 出站投递
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
```

### 仅运行单元测试
```bash
./build.sh Release
./build/smtps_fsm_test
./build/imaps_fsm_test
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
