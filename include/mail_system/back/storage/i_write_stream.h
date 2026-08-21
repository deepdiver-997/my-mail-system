#ifndef MAIL_SYSTEM_STORAGE_I_WRITE_STREAM_H
#define MAIL_SYSTEM_STORAGE_I_WRITE_STREAM_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mail_system {
namespace storage {

// 单个存储对象的写入句柄。
//
// 语义刻意照着 POSIX 的 pwrite 定，但只抄「显式」不抄「有状态」：
//   - 位置由调用方给出（write_at 的 offset），流内部不维护游标；
//     不存在 seek，也不存在 O_APPEND 那种「位置由被调用方决定」的语义。
//   - 调用方保证 offset 在 issue 时刻连续递增（单写者）。本地实现据此可用
//     pwrite，完成顺序无关紧要；仅支持追加的后端（HDFS/S3）也能按序落盘。
//   - commit() 是 POSIX 的 close() 所没有的概念：返回 true 才代表数据已持久化，
//     SMTP 侧只有在此之后才可以回 250，否则就是对发送方 MTA 撒谎。
class IWriteStream {
public:
    virtual ~IWriteStream() = default;

    // 在 offset 处写入 size 字节。offset 必须等于此前已写入的总字节数。
    virtual bool write_at(std::uint64_t offset,
                          const char* data,
                          std::size_t size,
                          std::string& error) = 0;

    // 冲刷并持久化。返回 true 后数据方可视为已落盘。
    virtual bool commit(std::string& error) = 0;

    // 放弃写入并清除半成品对象。析构路径会调用，不得抛异常。
    virtual void abort() noexcept = 0;
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_I_WRITE_STREAM_H
