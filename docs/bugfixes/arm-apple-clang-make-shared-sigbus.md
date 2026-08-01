# ARM macOS + Apple Clang: `make_shared` 触发 SIGBUS

## 症状

在 ARM macOS (Apple Silicon) 上构建 Debug (`-O0`) 或部分 Release 配置时，SMTP FSM 单元测试
启动即崩溃：`Bus error`（SIGBUS，exit code 138）。

```
$ ./build/smtps_fsm_test
[1]    12345 bus error  ./build/smtps_fsm_test
```

IMAP FSM 单元测试同样场景也可能触发。

## 根因

**不是我们主动违反对齐**，问题出在 `std::make_shared<T>()` 的**单次堆分配**机制：

1. `make_shared` 一次 `operator new` 同时分配 control block + 对象 `T`
2. Apple clang 在 `-O0` 下进行重模板展开（`TraditionalSmtpsFsm<MockConnection>`）时，对这个复合分配内部的子对象字段可能生成**非对齐 load/store 指令**
3. ARM 硬件强制对齐访问——非对齐的 load 直接触发 MMU 异常 → SIGBUS

关键点：**即使我们在结构体里写 `alignas(16)` 或 `char padding[16]` 也修不了**，因为：

- 问题发生在 `make_shared` 返回的那块内存内部——编译器决定 control block 和对象 `T` 的相对偏移
- `-O0` 时编译器不对这些偏移做对齐保证，生成的 load 指令可能访问非对齐地址
- 这是一个 **编译器代码生成 bug**（或至少是 `-O0` 时的已知限制），不是应用层代码可以干预的

## 为什么 x86 没事

x86 硬件**容忍非对齐访问**（有性能惩罚但不崩溃）。ARM 硬件**直接拒绝**。

## 绕过方法

1. **Release 构建** (`-O2`/`-O3`)：编译器主动做对齐优化，问题消失
2. **换用 `new` + `shared_ptr` 构造**（分离 allocation）：`std::shared_ptr<T>(new T(...))` 替代 `std::make_shared<T>(...)`——control block 和对象分开分配，编译器能正确对齐各自的内存。代价是两次 `new` 而非一次

## 影响范围

- 仅影响 ARM macOS 上的本地单元测试（`smtps_fsm_test`、`imaps_fsm_test`）
- 服务器部署的是 Linux x86_64 交叉编译产物，不受影响
- 本地 Release 构建正常

---

## 附：Session 生命周期说明

一个常被误解的点：**`TcpServerBase` 不持有任何 session 容器**。

```cpp
// tcp_server_base.h 的 accept 回调中：
auto session = make_tcp_session(std::move(conn), lc);
if (session) {
    increment_connection_count();   // ← 仅 metrics 计数，不持引用
    TcpSession::start(session);
}
// session 局部变量出作用域 → server 侧不再持有
```

session 的生存**完全靠异步 I/O 回调链**：

```
start(session)
  └→ session->do_async_read()
       └→ conn->async_read(buf, [self = shared_from_this()](...) {
               // self 持有一个 shared_ptr 引用 → session 存活
               self->handle_read(data);
               self->process_read();
               // FSM handler 可能调 do_async_write，回调中再调 do_async_read
               // 形成读→写→读→写... 的链条
           })
```

**只要回调链不断，session 就活着。** 一旦某个 handler 忘记调 `do_async_read()`（或 `drain_buffered_commands()` 后忘记恢复读取），最后一个 async 操作完成后所有 `shared_ptr` 释放 → session **立即析构** → `~SessionBase()` 调 `close()` 关闭连接 → `decrement_connection_count()`。

没有"悬挂 session"的状态——要么在回调链上活着，要么析构。
