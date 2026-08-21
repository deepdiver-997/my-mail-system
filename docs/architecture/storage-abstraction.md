# 存储抽象：显式 offset 写 + 只读视图读

## 设计目标

`IStorageProvider` 屏蔽本地文件、分布式副本、S3、WebHDFS 的差异。早期它只有一个 `append_binary(key, data, size, error)`——无状态、按 key 追加、位置由后端当前末尾决定。这带来两个后果：

1. **写入位置不可表达**：调用方无法说出"这块数据属于第 N 字节"，因此落盘顺序 = 调用到达顺序。一旦写入经过线程池（执行顺序 ≠ 发起顺序），就会把文件写错位（见 [正文错位修复](../bugfixes/2026-08-21-mail-body-rotation-fix.md)）。
2. **读侧完全缺失**：FSM 直接对 `body_path` 做 `std::ifstream`。而 `body_path` 对远程后端是 key 不是本地路径，远程后端的读路径因此一直是坏的。

## 写侧：显式 offset，顺序无关

```
IWriteStream
  write_at(offset, data, size, error)   // 位置由调用方在「发起时」给出
  commit(error)                          // fsync 后才算持久化，之后才可回 250
  abort() noexcept                       // 丢弃并删除半成品

IStorageProvider
  open_write(key, error) -> IWriteStream
```

关键语义：

- **单写者，offset 连续递增**（在发起时）。本地后端据此用 `pwrite`，**完成顺序无关紧要**；只支持追加的 S3/HDFS 也能按序落盘。
- `AppendWriteStream` 是默认适配：把 `append_binary` 包成 `IWriteStream`，并**在运行时校验 offset 连续**——乱序直接报错，不静默写坏。
- `commit()` 是 POSIX `close()` 没有的概念：返回 true 才代表数据已落稳定存储。SMTP 侧只有 commit 成功才回 250，否则回 451 让对方重投。

`MailBodyWriter` 在其上做固定 64KB 缓冲，`offset_` 是文件位置的唯一真相，只在 `emit()` 一处推进。

## 读侧：只读视图 + 可改缓冲

```
IReadStream
  view()  -> string_view    // 调用方不得修改；后端据此可零拷贝优化
  size()  -> uint64_t

IStorageProvider
  read_all(key, out, error)            // 纯虚：读进调用方可改的缓冲
  open_read(key, error) -> IReadStream  // 默认 read_all 兜底；本地覆写为 mmap
  object_size(key, size, error)         // 默认读下来算大小；本地覆写为一次 stat
```

- **view() 只读契约**是后端能用 mmap 的前提。生命周期：view 随流对象销毁而失效，不得跨临时对象取 view。
- 需要就地修改内容时用 `read_all`（如 IMAP FETCH 要对 body 做切片拼接）。
- `read_all` 是纯虚：新增后端时编译器强制它实现读，不留静默走本地文件系统的缺口。

后端实现对照：

| 后端 | 读 | 写 | 说明 |
|---|---|---|---|
| Local | mmap / ifstream / stat | 常开 fd + pwrite + fsync | 零拷贝，每块省 stat+open+close |
| Distributed | 逐副本尝试 | 多副本 append | 本地文件系统多根 |
| S3 | 签名 GET | append_binary 适配 | 复用已有 `s3_get` |
| HDFS | WebHDFS OPEN + 307 | append_binary 适配 | 手动走重定向取正文 |
| Null | 明确失败 | 假装成功 | 读回失败不冒充空内容 |

## 为什么不做异步写

本地写 64KB 进 page cache 是十几微秒，同步写在 IO 线程完全可行；为它派发线程池要付"拷贝缓冲区 + future 生命周期管理"的代价，还曾经把顺序搞丢。**异步化与否由后端决定，不该由 FSM 决定**：

- 本地：同步即可。
- 远程（S3/HDFS）：单次 PUT 是毫秒级，同步会阻塞 IO 线程。但真正优雅的做法是在**后端内部**做异步（provider 内部队列 + 后端线程，或 libcurl multi），保持 `IWriteStream` 同步签名不变——和 mmap 藏进 provider 是同一个思路。

## 后续方向

- `IoError{code, retryable}`：区分可重试失败（ENOSPC→451）与永久失败（EACCES→550）。
- 通用原语（`IReadStream`/`IWriteStream`/`LocalFile*Stream`）上移到 `framework/storage/`，与 `framework/db/` 的"通用原语"对齐；`IStorageProvider` 里的邮件命名规则（`build_mail_body_key` 等）留在 `mail_system/back/storage/`。
