# 2026-08-21 邮件正文字节错位（rotation）修复 + 存储读侧抽象

## 背景

test3 账号一封验证码邮件在客户端渲染成满屏 `=E3=80=82` 的乱码。排查发现磁盘上的原始报文**不是解析错误，而是字节错位**：文件布局是 `M[k:] + M[:k]`——文件开头是报文尾部，真正的报文头（`X-AliDM-RcptTo:`）躺在文件中间，且与前面的结束 boundary 之间连换行都没有。

83 封邮件里坏了 3 封，rotation 量分别为 **8192 / 4096 / 6909**。

## 根因

入站正文的写路径有**三条互不排序**，且位置语义都由"文件当前末尾"决定：

1. `async_flush_buffer_to_disk()` → 提交到 worker 线程池
2. `flush_buffer_to_disk()` → IO 线程同步写
3. `append_to_buffer()` 超大块直写 → IO 线程同步写

致命点在 `flush_body_and_wait()` 顺序反了：先同步写"报文尾"落盘，才等异步的"报文头"。线程池一旦没及时调度，尾部就先 append 到偏移 0，头部随后追加在后面 → 旋转。

**为什么 append 有这个问题**：`append` 的写入位置在**执行那一刻**由"文件当前末尾"决定。一旦执行顺序 ≠ 发起顺序（线程池乱序），落盘顺序就乱了。rotation 量的不规则（8192/4096/6909）正是"溢出那一刻缓冲区恰好累积了多少字节"，取决于 TCP 分片到达节奏。

## 修复

### 1. 写侧：显式 offset，让乱序在接口层面无法表达（`c52485b`）

- 新增 `IWriteStream`：`write_at(offset, data, size, error)`，位置由**调用方在发起时**给出，谁先执行都无所谓。加上 `commit()`（含 fsync）持久化语义。
- `IStorageProvider::open_write()` 默认基于 `append_binary` 适配，并**校验 offset 连续**——乱序直接失败，而不是静默写坏。
- `LocalFileWriteStream`：整个对象只 open 一次 fd，`pwrite` 落盘，`commit` 走 fsync。每块数据省掉一次父目录 stat + open + close。
- `MailBodyWriter`：固定 64KB 缓冲，`offset_` 是文件位置唯一真相，只在 `emit()` 一处推进；未 commit 就析构自动 abort 删除半成品。
- 删掉 `SmtpsSession` 的 `expand_buffer` / `flush_buffer_to_disk` / `async_flush_buffer_to_disk` / `wait_for_async_writes` / `flush_body_and_wait` 五件套，以及 3 处重复手写的 `ofstream` 兜底。
- 正文未能落盘时改回 `451` 而不是继续走到 `250`（否则就是骗发送方 MTA 把邮件从队列删掉）。

**为什么不做异步**：给 page cache 写 64KB 是十几微秒量级，而为它把缓冲区拷贝一份再派发到线程池，开销比想省掉的那次写还大，顺带把顺序也丢了。

### 2. MIME 解析读回（`6038ed4` / `c16f1bf`）

- `parse_mime_tree` 改收 `std::string_view`。
- 新增 `MappedFile`（mmap 只读）+ `MappedReadStream`，本地后端零拷贝读取。
- **读侧抽象补齐**：`IStorageProvider` 新增 `read_all` / `open_read` / `object_size`，五个后端全部实现。此前读侧完全没有抽象——FSM 直接对 `body_path` 做 `std::ifstream`，而 `body_path` 对 S3/HDFS 是远程 key 不是本地路径，远程后端的读路径一直是坏的。

### 3. IMAP lazy 解析回写 sidecar（`b9c6450`）

- 无 sidecar 的旧邮件/大邮件首次解析后回写 sidecar（`save_mime_tree`），避免每次 FETCH 重复解析。
- sidecar 改为临时文件 + `rename` 原子写（并发读者不会读到半截 JSON），并补 JSON 转义。

## 验证

- 单元测试：`mail_body_writer_test`（102 断言）、`mime_parser_test`（159 断言），ctest 8/8。
- 变异测试：删掉 `offset_ += size` 被"写入错位"断言抓住；删掉 abort 被 RAII 断言抓住。
- 生产端到端：发 226KB 邮件（强制约 4 次刷盘），落盘文件以报文头开头、2000 个行号标记严格单调无缺失、sidecar 正确。multipart 邮件经 mmap 读路径解析出正确的两个子 part。

## 遗留

- `IoError{code, retryable}` 富错误类型（区分 ENOSPC→451 / EACCES→550）。
- 远程后端（S3/HDFS）读的默认实现是"下载进堆缓冲"，尚未做 provider 内部队列 / libcurl multi 的异步化。
- 3 封历史错位邮件未回填（内容均为过期验证码，用户选择暂不动）。
