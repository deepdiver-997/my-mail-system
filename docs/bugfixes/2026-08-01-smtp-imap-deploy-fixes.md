# 2026-08-01 部署与协议修复记录

## 背景

将 ProtoRelay 从本地交叉编译部署至 `root@120.24.169.213` (Ubuntu 22.04, 1.6GB RAM)，替换旧版 SMTP/IMAP 服务。部署后发现网易邮件大师无法拉取邮件。

---

## 问题清单

### 1. 交叉编译 `-march=native` 不兼容

**症状：** `cc1plus: error: bad value 'native' for '-march=' switch`

**根因：** CMakeLists.txt 中 Release 模式硬编码 `-march=native`。macOS 上的 x86_64 交叉编译器不支持此标志（host 是 aarch64）。

**修复：**
- [CMakeLists.txt](../../CMakeLists.txt#L74)：`CMAKE_CXX_FLAGS_RELEASE` 改用 `if(NOT DEFINED)` 保护，允许命令行覆盖
- [build.sh](../../build.sh)：cross-x64 模式追加 `-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3"`（对应服务器 Intel Xeon Platinum 的 AVX512 能力）

### 2. `drain_buffered_commands()` 不恢复异步读

**症状：** SMTP 465 端口 AUTH 成功后 MAIL FROM 无响应，连接超时断开。IMAP 也有潜在风险。

**根因：** [session_base.tpp](../../include/framework/session_base.tpp#L191) 的 `drain_buffered_commands()` 在 while 循环中处理完缓冲命令后，如果缓冲区为空，直接 return 了，没有调用 `do_async_read()` 恢复网络读取。

SMTP AUTH 成功回调：
```cpp
session->do_async_write("235 Authentication successful\r\n",
    [session](auto s, auto& ec) {
        if (!ec) s->set_current_state(WAIT_MAIL_FROM);
        s->drain_buffered_commands();  // ← 没缓充命令，完了就不读了！
    });
```

其他 handler（RCPT TO, MAIL FROM 等）都在回调里显式调了 `do_async_read()`，唯独 `drain_buffered_commands()` 这条路径漏了。

**修复：** 在 SMTP AUTH 的成功/失败回调中，`drain_buffered_commands()` 之后显式补充 `do_async_read()`：

```cpp
// 6 处 SMTP AUTH 回调
s->drain_buffered_commands();
if (!s->has_buffered_input() && !s->is_closed()) s->do_async_read();
```

**为什么不在 `drain_buffered_commands()` 内部兜底：** 在 pipeline 场景下（客户端一次性发 AUTH + MAIL FROM + RCPT TO + DATA），`drain_buffered_commands()` 的 while 循环会依次消费所有命令。其中 DATA 回调是同步触发的（`do_async_write` 检测到有缓冲命令时同步调用回调），回调里的 `do_async_read()` 会真正发起异步读。如果 `drain_buffered_commands()` 末尾再加一次 `do_async_read()`，同一 socket 上就有两个并发 `async_read`——Asio 的 SSL stream 不支持这种行为，属于未定义行为。

因此采用显式方案：`drain_buffered_commands()` 保持纯消费者语义（只消费缓冲、不启动新读），由调用方按需恢复异步读。这与其他 handler（RCPT TO/MAIL FROM）的模式一致。

### 3. IMAP FETCH `BODY.PEEK[HEADER.FIELDS]` 不识别

**症状：** 网易邮件大师发送 `UID FETCH ... BODY.PEEK[HEADER.FIELDS (to from subject date message-id)]` 只取特定邮件头，但服务端只认得 `BODY.PEEK[HEADER]`，无法识别 `.FIELDS` 后缀。导致邮件列表的主题/发件人信息无法显示。

**根因：** [traditional_imaps_fsm.tpp](../../include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp) 中 header 匹配检查只做简单子串匹配：
```cpp
bool want_body_header = attrs.find("BODY.PEEK[HEADER]") != std::string::npos;
```
没有解析 `HEADER.FIELDS (field1 field2 ...)` 语法，更不支持 `HEADER.FIELDS.NOT`。

**修复：**
- 检测 `HEADER.FIELDS` 子串，提取括号内字段名列表
- 遍历邮件原始 headers，只保留匹配的字段
- 响应标签同步为 `BODY[HEADER.FIELDS (field1 field2)]`
- 同时支持 `HEADER.FIELDS.NOT`（返回不匹配的字段）

### 4. IMAP FETCH `BODY[1]` 响应标签错误

**症状：** 客户端请求 `BODY.PEEK[1]`，服务端返回标签 `BODY[]`。RFC 3501 规定单段邮件的 `BODY[1]` 等价于 `BODY[]`，但部分严格客户端不认可。

**修复：** 根据 `body_part_num` 动态生成响应标签 `BODY[1]` 或 `BODY[]`。

### 5. SSL 证书路径

**症状：** 部署后 SMTP/IMAP 都无法启动，报 `Certificate file not found: /opt/smtpServer/crt/server.crt`。

**根因：** 证书保存在 `/opt/smtpServer/config/crt/`，但 `WorkingDirectory=/opt/smtpServer`，配置文件中的相对路径 `crt/server.crt` 解析为 `/opt/smtpServer/crt/`。

**修复：** `ln -sfn /opt/smtpServer/config/crt /opt/smtpServer/crt`

### 6. 部署脚本并行度低

**症状：** 交叉编译只用 4 线程，CPU 利用率低。

**修复：** [deploy.sh](../../deploy.sh) 从 `JOBS="4"` 改为 `JOBS="$(sysctl -n hw.ncpu)"`，自动检测 CPU 核心数。

---

## 验证

- IMAP FSM 单元测试：38/38 通过
- SMTP 465 手动发信：MAIL FROM → RCPT TO → DATA → QUIT 全流程通过
- IMAP 993 手动测试：LOGIN → SELECT → UID FETCH (HEADER.FIELDS + BODY[1]) 返回正确
- 网易邮件大师实际测试：test3@scut.email 可正常登录拉取邮件

---

## 影响范围

| 修改文件 | 影响 |
|---------|------|
| `CMakeLists.txt` | 仅影响交叉编译时的 Release flags |
| `build.sh` | 仅 cross-x64 模式追加 `-march=x86-64-v3` |
| `include/framework/session_base.tpp` | SMTP + IMAP 共用，修复 AUTH 后不恢复读取 |
| `include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp` | IMAP FETCH 响应格式 |
| `include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h` | 新增 `#include <set>` |
| `deploy.sh` | 并行度优化 |
