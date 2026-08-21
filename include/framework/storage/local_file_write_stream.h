#ifndef PR_FRAMEWORK_STORAGE_LOCAL_FILE_WRITE_STREAM_H
#define PR_FRAMEWORK_STORAGE_LOCAL_FILE_WRITE_STREAM_H

#include "framework/storage/i_write_stream.h"

#include <memory>

namespace pr {


// 本地文件写入句柄：整个对象生命周期只 open 一次 fd，落盘走 pwrite。
//
// 相比按 key 反复 append 的老路径，每块数据省掉一次父目录 stat + open + close；
// 而且写入位置由 offset 显式给出，不依赖内核维护的文件游标，
// 因此即便调用方将来把写入挪到别的线程，也不会再出现字节错位。
class LocalFileWriteStream : public IWriteStream {
public:
    // 打开（必要时创建父目录并截断已有文件）。失败返回 nullptr 并填充 error。
    static std::unique_ptr<LocalFileWriteStream> open(const std::string& path,
                                                      std::string& error);

    ~LocalFileWriteStream() override;

    LocalFileWriteStream(const LocalFileWriteStream&) = delete;
    LocalFileWriteStream& operator=(const LocalFileWriteStream&) = delete;

    bool write_at(std::uint64_t offset,
                  const char* data,
                  std::size_t size,
                  std::string& error) override;

    // flush + fsync，返回 true 表示数据已真正落到稳定存储。
    bool commit(std::string& error) override;

    // 关闭 fd 并删除半成品文件。
    void abort() noexcept override;

private:
    LocalFileWriteStream(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

    void close_fd() noexcept;

    int fd_;
    std::string path_;
    bool committed_ = false;
};


} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_LOCAL_FILE_WRITE_STREAM_H
