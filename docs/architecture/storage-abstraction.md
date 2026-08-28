# 存储抽象：显式 offset 写 + 只读视图读

## 设计目标

`IStorageProvider` 屏蔽本地文件、分布式副本、S3、WebHDFS 的差异。早期它只有一个 `append_binary(key, data, size, error)`——无状态、按 key 追加、位置由后端当前末尾决定。这带来两个后果：

1. **写入位置不可表达**：调用方无法说出"这块数据属于第 N 字节"，因此落盘顺序 = 调用到达顺序。一旦写入经过线程池（执行顺序 ≠ 发起顺序），就会把文件写错位（见 [正文错位修复](../mail-system/bugfixes/2026-08-21-mail-body-rotation-fix.md)）。
2. **读侧完全缺失**：FSM 直接对 `body_path` 做 `std::ifstream`。而 `body_path` 对远程后端是 key 不是本地路径，远程后端的读路径因此一直是坏的。

## 写侧：显式 offset，顺序无关

```
IWriteStream
  write_at(offset, data, size, error)   // 位置由调用方在「发起时」给出
  commit(error)                          // fsync 后才算持久化，之后才可回 250
  commit_async(cb)                       // 异步形状的 commit：默认内联执行，
                                         // 远程后端可覆写为 provider 线程执行
  abort() noexcept                       // 丢弃并删除半成品

IStorageProvider
  open_write(key, error) -> IWriteStream
```

关键语义：

- **单写者，offset 连续递增**（在发起时）。本地后端据此用 `pwrite`，**完成顺序无关紧要**；只支持追加的 S3/HDFS 也能按序落盘。
- `AppendWriteStream` 是默认适配：把 `append_binary` 包成 `IWriteStream`，并**在运行时校验 offset 连续**——乱序直接报错，不静默写坏。
- `commit()` 是 POSIX `close()` 没有的概念：返回 true 才代表数据已落稳定存储。SMTP 侧只有 commit 成功才回 250，否则回 451 让对方重投。
- `commit_async(cb)` 与 `IDBConnection::async_query` 同一模式：**默认实现在发起线程内联执行**（等价于同步 commit 后立刻回调），本地后端零开销、行为不变；远程后端将来覆写为真异步（provider 线程做 PUT，完成后触发回调）时，调用点代码不改。**回调线程契约**：真异步实现允许在 provider 线程触发回调，调用方必须已通过流水线 `set_paused(true)` 取得 session 独占（SPF/DNS 回调同一约定——pause 期间 io 线程不再消费该 session 也不发起新的 socket 读），或自行 post 回 executor。DATA_END 收尾已按此改造（`commit_body_async` → 回调里 250/451）。

`MailBodyWriter` 在其上做固定 64KB 缓冲，`offset_` 是文件位置的唯一真相，只在 `emit()` 一处推进。

### 远程后端：整对象缓冲，一次上传

对追加型后端，「每块 `write_at` = 一次网络往返」在流式写入下不可接受，S3 的
`append_binary` 适配甚至是 GET 全量 + PUT 全量（O(n²) 字节）。因此 S3/WebHDFS
覆写 `open_write` 返回 `BufferedUploadStream`：

- `write_at` 只攒内存缓冲（沿用 offset 连续性校验），`commit()` 时把整个对象
  **一次性**交给上传函数——S3 一次 PUT（整对象原子替换），WebHDFS 一次
  CREATE(overwrite=true)。一封 N 块的邮件从 N 次（或 N×2 次）往返变成 1 次。
- 对象完整驻留内存直到 commit，因此带硬上限（`max_write_buffer_bytes`，默认
  64MB，可配置）。SMTP 正文上限 10MB，但超限后原始字节仍会继续流向写入流直到
  DATA_END 才拒收——上限防的就是这种客户端把内存当磁盘用。超限 → `write_at`
  失败 → `MailBodyWriter` 进入 failed → DATA_END 回 451。
- `abort` 语义按「是否尝试过上传」区分：commit 前丢弃缓冲即可（远端零副作用）；
  commit 尝试失败后（PUT 中断可能在远端留半成品）才调用 cleanup 清除远端对象。
- `commit` 空对象也要上传一次（size=0），与本地后端「打开即创建空文件」对齐，
  否则空邮件在远程后端读侧会读不到。
- 超大对象（几十 MB 以上）若成为现实需求，再演进为 multipart/并行分片上传；
  接口不变，只动 `BufferedUploadStream` 的上传函数。

## 读侧：只读视图 + 可改缓冲

```
IReadStream
  view()  -> string_view    // 调用方不得修改；后端据此可零拷贝优化
  size()  -> uint64_t

IStorageProvider
  read_all(key, out, error)            // 纯虚：读进调用方可改的缓冲
  open_read(key, error) -> IReadStream  // 默认 read_all 兜底；本地覆写为 mmap
  object_size(key, size, error)         // 默认读下来算大小；本地覆写为一次 stat
  async_open_read(key, cb)              // 默认内联；真异步覆写在 provider 线程回调
  async_read_all(key, cb)               //   同上 —— 与 commit_async 同一契约
  async_object_size(key, cb)            //   同上
```

- **view() 只读契约**是后端能用 mmap 的前提。生命周期：view 随流对象销毁而失效，不得跨临时对象取 view。
- 需要就地修改内容时用 `read_all`（如 IMAP FETCH 要对 body 做切片拼接）。
- `read_all` 是纯虚：新增后端时编译器强制它实现读，不留静默走本地文件系统的缺口。

后端实现对照：

| 后端 | 读 | 写 | 说明 |
|---|---|---|---|
| Local | mmap / ifstream / stat | 常开 fd + pwrite + fsync | 零拷贝，每块省 stat+open+close |
| Distributed | 逐副本尝试 | 多副本 append | 本地文件系统多根 |
| S3 | 签名 GET / HEAD(Content-Length) | 缓冲整对象 + 单次 PUT | `object_size` 覆写为一次 HEAD——默认实现会整对象下载只为拿大小，FETCH RFC822.SIZE 逐封调用不可接受 |
| HDFS | WebHDFS OPEN + 307 | 缓冲整对象 + 单次 CREATE(overwrite) | 手动走重定向取正文 |
| Null | 明确失败 | 假装成功 | 读回失败不冒充空内容 |

## 为什么不做异步写（write_at 永远同步）

本地写 64KB 进 page cache 是十几微秒，同步写在 IO 线程完全可行；为它派发线程池要付"拷贝缓冲区 + future 生命周期管理"的代价，还曾经把顺序搞丢。远程后端走整对象缓冲后，`write_at` 也只是 memcpy 进内存缓冲。**异步化与否由后端决定，不该由 FSM 决定**，且异步的边界在 **commit（对象粒度）** 而不在 write_at（块粒度）：

- 本地：`commit_async` 走默认内联实现，全程同步。
- 远程（S3/HDFS）：唯一的网络时刻是 commit 时那一次 PUT/CREATE（毫秒级）。`commit_async` 已备好异步形状，后端将来覆写为 provider 内部线程执行即可，`IWriteStream` 的 `write_at` 签名永远不变——和 mmap 藏进 provider 是同一个思路。

读侧还剩 `read_all` / `object_size` 两个一次性操作在远程后端上是同步网络调用（IMAP FETCH、SMTP DATA 后 MIME 预解析路径），同样的「默认内联 + 真异步覆写」待遇留给它们。

## 错误值：IoError{kind, code, message}

存储接口的错误一律是 `IoError`（`framework/storage/io_error.h`），不再是无结构的
字符串——kind 的判定归属后端（只有它知道失败来自 errno 还是 HTTP 状态码），
调用方据此决定对外行为：

- **Retryable（默认，fail-safe）**：ENOSPC/EDQUOT/EIO/EMFILE、网络类、HTTP
  5xx/408/429、curl 传输错误。SMTP 回 **451**，发送方稍后重投。
- **Permanent**：EACCES/EPERM/EROFS/EFBIG、HTTP 4xx（403/404 拒绝类）、
  流式写超出 `max_write_buffer_bytes`。SMTP 回 **550**——重投结果不会改变，
  再收一次只会再失败一次。

拿不准时选 retryable：451 最多让对方多投几次，550 会把邮件丢掉。

## 分层：framework 原语 vs 应用层 Provider

通用原语已在 `framework/storage/`（namespace `pr`，与 `framework/db` 同层）：
`IReadStream`/`IWriteStream`（含 `commit_async`）/`BufferedUploadStream`/
`LocalFile*Stream`/`MappedFile`/`IoError`。`IStorageProvider` 与邮件命名规则
（`build_mail_body_key` 等）留在 `mail_system/back/storage/`，经伞形头
`i_storage_provider.h` 以 using 兼容引入——和 `db_pool.h` 的向后兼容是同一模式。

## 远程后端的真异步：装配时装饰，而非 provider 内自建线程

`AsyncStorageProvider`（含 `AsyncCommitStream`）在装配时套在 S3/WebHDFS 外面：
同步方法全透传，`commit_async` 与三个 async 读把整次网络操作投递到 executor
（server 的 worker 线程池），底层 provider 保持纯同步实现。本地后端不套——
多一次线程跳转纯属浪费。S3/HDFS 的 PUT/GET 由此不再阻塞 io 线程，
SMTP DATA_END 整条链路（commit → MIME 预解析读）对远程后端完全非阻塞。
executor 惰性取 pool（装配时 worker 池尚未创建），无池配置内联兜底。

## 后续方向

- 超大对象 multipart/并行分片上传（当前邮件上限 10MB 用不上）。

至此读写两侧的异步化全部闭环：IMAP FETCH 已改为续作链
（`fetch_drive` → size/envelope → 正文 → 下一封），逐封走
`async_object_size`/`async_read_all`；本地内联、远程经装饰器上
worker。每封正文只读一次（旧路径 header/body/BODYSTRUCTURE 兜底
最多各读一次，一封三次网络往返）。
