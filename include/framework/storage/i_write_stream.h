#ifndef PR_FRAMEWORK_STORAGE_I_WRITE_STREAM_H
#define PR_FRAMEWORK_STORAGE_I_WRITE_STREAM_H

#include "framework/storage/io_error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace pr {

// 单个存储对象的写入句柄。
//
// 语义刻意照着 POSIX 的 pwrite 定，但只抄「显式」不抄「有状态」：
//   - 位置由调用方给出（write_at 的 offset），流内部不维护游标；
//     不存在 seek，也不存在 O_APPEND 那种「位置由被调用方决定」的语义。
//   - 调用方保证 offset 在 issue 时刻连续递增（单写者）。本地实现据此可用
//     pwrite，完成顺序无关紧要；仅支持追加的后端（HDFS/S3）也能按序落盘。
//   - commit() 是 POSIX 的 close() 所没有的概念：返回 true 才代表数据已持久化，
//     SMTP 侧只有在此之后才可以回 250，否则就是对发送方 MTA 撒谎。
//   - 错误一律携带 IoError：调用方据 retryable() 决定 451（重投）或 550（拒收）。
class IWriteStream {
public:
    virtual ~IWriteStream() = default;

    // 在 offset 处写入 size 字节。offset 必须等于此前已写入的总字节数。
    // 永远保持同步签名：本地是 pwrite 进 page cache（µs 级），远程走
    // BufferedUploadStream 也只是 memcpy 进缓冲，都没有异步化的收益。
    virtual bool write_at(std::uint64_t offset,
                          const char* data,
                          std::size_t size,
                          IoError& error) = 0;

    // 冲刷并持久化。返回 true 后数据方可视为已落盘。
    virtual bool commit(IoError& error) = 0;

    // 异步形状的 commit，回调携带 (成功与否, 错误值)。
    //
    // 默认实现在发起线程内联执行（等价于同步 commit 后立刻回调），本地后端
    // 因此零开销、行为与同步版完全一致 —— 和 IDBConnection::async_query 的
    // 「默认内联」是同一模式。远程后端将来覆写为真异步（provider 线程做
    // PUT，完成后触发回调）时，调用点代码无需改动。
    //
    // 回调线程契约：真异步实现允许在 provider 自己的线程触发回调。调用方
    // 必须已通过流水线 set_paused(true) 取得 session 的独占访问
    // （SPF/DNS 回调的同一约定），或自行把续作 post 回 io 线程。
    using CommitCallback = std::function<void(bool ok, const IoError& error)>;

    virtual void commit_async(CommitCallback cb) {
        IoError error;
        const bool ok = commit(error);
        if (cb) cb(ok, ok ? IoError{} : std::move(error));
    }

    // 放弃写入并清除半成品对象。析构路径会调用，不得抛异常。
    virtual void abort() noexcept = 0;
};

} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_I_WRITE_STREAM_H
