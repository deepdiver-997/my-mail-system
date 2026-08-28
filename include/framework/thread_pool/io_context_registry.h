#ifndef MAIL_SYSTEM_IO_CONTEXT_REGISTRY_H
#define MAIL_SYSTEM_IO_CONTEXT_REGISTRY_H

namespace boost::asio {
class io_context;
}

namespace mail_system {

// IO 线程的 io_context 注册表（thread_local）。
//
// 每个 IOThreadPool 工作线程启动时把自己的 io_context 注册进 thread_local
// （见 io_thread_pool.cpp），DB 非阻塞等待据此判断当前调用线程：
//   - 是 io 线程 → 返回其 io_context → 用 io_context.async_wait 异步续（io 线程不阻塞）
//   - 非 io 线程（worker）→ 返回 nullptr → 走阻塞 poll（worker 本就干阻塞活）
//
// 只读访问（current_io_context）由被查询线程自己触发，无需加锁；写入只在
// IOThreadPool 线程启动/退出时各一次。

boost::asio::io_context* current_io_context();
void set_current_io_context(boost::asio::io_context* ctx);

} // namespace mail_system

#endif // MAIL_SYSTEM_IO_CONTEXT_REGISTRY_H
