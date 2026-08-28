# IMAP CPU 100% 空转 Bug 分析报告

## 现象

IMAP 服务进程（imapsServer）的两个 IO 线程各占用约 100% CPU，累计 CPU 时间远超正常水平（运行 5 小时累积 3 天 CPU 时间）。SMTP 进程未受影响。

## 背景：IMAP vs SMTP 的流水线消费差异

两个协议共享 `SessionBase::do_async_read()` 中的同一个流水线消费循环：

```
session_base.h:
  async_read 回调
    └→ command_read_buffer_.append(data)
    └→ while (has_buffered_input())
         ├→ handle_read(extract_one_line())   // 从缓冲区取一行（不移走全部）
         └→ process_read()                   // 处理一行/一个命令
```

重构后 `extract_one_line()` 每次只从缓冲区移除一行，剩余行原地保留，避免了旧代码 `take_buffered_input()` 全量移出再全量拷回的 O(n²) 拷贝。

**handle_read** 的实现差异仍是 IMAP 空转的根本原因：

| | SMTP | IMAP |
|---|---|---|
| `handle_read(data)` | 直接调用 `parse_smtp_command(data)`，内部解析命令 | 仅 `command_read_buffer_.append(data)` — 放回缓冲 |
| `process_read()` | 调用 `fsm->auto_process_event()` | 直接操作 `command_read_buffer_` 逐行解析 |
| 缓冲区消费时机 | `handle_read` 阶段就解析完了 | `process_read` 阶段才逐行消费 |
| 行分隔符 | 重载 `extract_one_line()` 和 `has_buffered_input()` 用 `\n` | 基类默认 `\r\n`（符合 RFC 3501） |
| 特殊状态 | 无持久等待状态 | **IDLE 模式**：服务器无限期等待客户端发 `DONE` |

SMTP 之所以不受影响：`handle_read` 自己解析命令不依赖后续 `process_read` 消费缓冲区。
         ├→ handle_read(take_buffered_input())   // 取出全部缓冲
         └→ process_read()                       // 处理一行/一个命令
```

但 **handle_read** 的实现截然不同，这是 SMTP 不受影响的根本原因：

| | SMTP | IMAP |
|---|---|---|
| `handle_read(data)` | 直接调用 `parse_smtp_command(data)`，内部消费一行并从 `command_read_buffer_` 中移除 | 仅 `command_read_buffer_.append(data)` — 全部放回 |
| `process_read()` | 调用 `fsm->auto_process_event()` — 4 行代码 | 直接操作 `command_read_buffer_` 逐行解析，200+ 行 |
| 缓冲区消费时机 | `handle_read` 阶段就消费了 | `process_read` 阶段才消费 |
| 特殊状态 | 无持久等待状态 | **IDLE 模式**：服务器无限期等待客户端发 `DONE` |

关键差异：SMTP 的 `handle_read` 就完成了数据消费，`process_read` 只是触发 FSM。FSM 处理后调用 `do_async_read()` 时，缓冲区已空，所以 `has_buffered_input()` 返回 false → 真正启动异步读。

IMAP 的 `handle_read` 只是把数据放回缓冲区，`process_read` 才逐行消费。消费一行后若还有数据，调用 `do_async_read()` 时 `has_buffered_input()` 仍为 true → 立即返回 → 流水线循环再次 `take_buffered_input`（全量拷贝）→ 再次 `process_read`。

---

## 根因 1：IDLE 模式流水线循环 O(n²) 空转

### 位置

`include/mail_system/back/mailServer/session/imaps_session.tpp` — `process_read()` IDLE 分支

### 触发条件

客户端发送 `IDLE` 命令进入 IDLE 状态后，如果缓冲区中有多个非 `DONE` 的完整行（如管道化的后续命令或垃圾数据），就会触发。

### 调用链

```
async_read 回调
  └→ 数据追加到 command_read_buffer_
  └→ while (has_buffered_input())              ← 流水线消费循环
       ├→ handle_read(take_buffered_input())    ← 全量取出再全量放回（拷贝整个缓冲区）
       └→ process_read()
            └→ IDLE 分支：处理 1 行，从 buf 中删除
                 ├→ 非 DONE → do_async_read()
                 │            └→ has_buffered_input() → true → 立即返回
                 └→ return
       └→ has_buffered_input() → true → 继续循环  ← 再次 copy + process
```

### 原因

旧代码在 IDLE 模式下每次只处理**一行**，然后调用 `do_async_read()` 期望等待新数据。但由于 IMAP 的 `handle_read` 不消费数据，缓冲区中剩余的完整行导致 `has_buffered_input()` 仍为 true → `do_async_read()` 立即返回。外层流水线循环再次调用 `handle_read(take_buffered_input())` 全量拷贝剩余缓冲 → 再次进入 `process_read`。每轮迭代拷贝整个剩余缓冲区，n 行数据产生 O(n²) 字符拷贝量。

### 修复

将 IDLE 模式下的逐行处理改为内部 `while(true)` 循环，一次性消费缓冲区中所有行：

```cpp
// 修复前：每次只处理 1 行，依赖外层流水线循环
if (session->context_.idle_mode) {
    size_t line_end = buf.find("\r\n");
    if (line_end != std::string::npos) {
        // 处理 1 行
        if (upper == "DONE") { ... return; }
    }
    self->do_async_read();
    return;
}

// 修复后：内部 while(true) 批量消费所有行后再返回
if (session->context_.idle_mode) {
    while (true) {
        size_t line_end = buf.find("\r\n");
        if (line_end == std::string::npos) {
            self->do_async_read();  // 无完整行，真正等待新数据
            return;
        }
        // 提取行、从 buf 删除
        if (upper == "DONE") { ... return; }
        // 非 DONE → 丢弃，继续循环消费下一行
    }
}
```

---

## 根因 2：`has_buffered_input()` 与 `process_read()` 行结束符不一致

### 位置

`include/mail_system/back/mailServer/session/session_base.h` — `has_buffered_input()`

### 触发条件

TCP 流中存在 `\n` 但尚未收到 `\r\n`（如 Unix 风格换行、网络分片导致 `\r` 和 `\n` 在不同 TCP 包中到达）。

### 调用链

```
async_read 回调
  └→ has_buffered_input() → true   ← buffer 中有 \n
  └→ handle_read(...)
  └→ process_read()
       └→ buf.find("\r\n") → npos   ← 但找不到 \r\n！
       └→ do_async_read()
            └→ has_buffered_input() → true  ← 又回到原点
            └→ return
       └→ return
  └→ has_buffered_input() → true   ← 死循环
```

### 原因

`has_buffered_input()` 只检查 `\n`，而 `process_read()` 需要 `\r\n` 才能形成完整命令行。两者判断不一致形成死循环：

- `has_buffered_input()` 认为"有数据可处理" → 触发流水线循环
- `process_read()` 认为"没有完整行" → 调用 `do_async_read()` 等待
- `do_async_read()` 又检查 `has_buffered_input()` → true → 立即返回

### 修复

将 `has_buffered_input()` 的检查从 `\n` 改为 `\r\n`，与 `process_read()` 对齐：

```cpp
// 修复前
virtual bool has_buffered_input() const {
    return command_read_buffer_.find('\n') != std::string::npos;
}

// 修复后
virtual bool has_buffered_input() const {
    return command_read_buffer_.find("\r\n") != std::string::npos;
}
```

SMTP 和 IMAP 协议规范（RFC 5321、RFC 3501）均要求 CRLF 行结束符，此改动不影响正常客户端。

---

## 附：为什么 socket 关闭不会中断死循环

两个根因都发生在 `do_async_read()` 的回调内部——回调函数进入 `while(has_buffered_input())` 后永不返回。IO 线程的 `io_context::run()` 事件循环依赖回调返回来处理下一个事件（包括 socket 的 close/EOF 事件）。回调不返回 → 事件循环被阻塞 → 所有新事件（包括 TCP RST/FIN）永远排不上队 → session 永远不退出 → `close()` 永远不会被调用。

这就是为什么即使客户端已经断开（CLOSE-WAIT），进程仍然 100% CPU 空转。

---

## 影响范围

- **根因 1**：仅影响 `ImapsSession::process_read()` 的 IDLE 分支。SMTP 没有 IDLE 模式，不受影响
- **根因 2**：影响所有使用 `SessionBase` 的会话。修复后 SMTP 通过重载 `has_buffered_input()` 和 `extract_one_line()` 保持 LF 容错
- **流水线重构**：`extract_one_line()` 替代 `take_buffered_input()`，每次只取一行，消除全量拷出-拷回的往返

## 验证

修复后在服务器上执行完整 IMAP 会话（CAPABILITY → LOGIN → SELECT → IDLE/DONE → FETCH → LOGOUT），所有线程 CPU 保持 0%。
