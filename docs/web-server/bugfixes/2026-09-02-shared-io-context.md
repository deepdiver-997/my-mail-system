# 框架所有 io 线程共享一个 io_context → H2 并发 UAF（2026-09-02，已修已提 0f4bb45）

> 本次排查里最隐蔽的一集：起初以为是 H2 的 map 悬垂，最后定位到**框架线程模型**。
> 此 bug 属框架级，但由 H2 暴露——故记在本业务目录，教训通用。

## 现象
- H2 默认多 io 线程压 8MB 必崩/截断；`io_thread_count=1` 即稳（决定性对照实验）。
- ASan：`sendto` 读已释放缓冲，freed-by 在 `drain_stream` 的 `append`。

## 根因：`io_thread_pool.cpp` 构造的一行
```cpp
m_io_contexts(thread_count, std::make_shared<boost::asio::io_context>())
```
`std::vector<T>(count, value)` fill 构造把**同一个 `value`**复制 count 份 —— 对 shared_ptr
就是同一对象 count 个别名，`make_shared` 只发生**一次**。于是所有 io 线程 `run()` 同一个
io_context → 连接轮转绑这个共享 context，读写完成回调由 N 个线程之一调度。

线程 ID 日志实证：同一条连接 `read-complete` 恒一个线程、`flush-cb` 散布全部 N 线程 → **单连接被多线程服务**。

## 为什么 H2 崩、SMTP/IMAP/POP3 没事
共享多线程 io_context 下，H2 连续多帧 pumping 使 read-drain（读线程）与 write-complete（任一
其他 io 线程）**真并发**抢同一 `out_pending_`/`out_flushing_`。其余协议每请求单次 `async_write`、
读写不重叠，没踩中竞争窗。

## 修复（B 方案：每线程独立 context）
```cpp
m_io_contexts.reserve(thread_count);
for (i...) m_io_contexts.push_back(std::make_shared<boost::asio::io_context>());
```
恢复"一连接一 context 一线程"。验证：h2web 默认多线程 6×8MB 逐字节一致且进程存活；
thread_pool 18/18、server_base 47/47；webServer/smtpsServer/imapsServer 编译通过。

## 教训（已记 memory shared-ptr-vector-trap）
任何"想造 N 个独立资源"而写 `vector<shared_ptr>(N, make_shared<...>())` 都是这个坑。
写并发单测遇 UAF，先查 fill 构造。

## 部署注意
改了生产线程拓扑，**邮件协议部署前需回归**。