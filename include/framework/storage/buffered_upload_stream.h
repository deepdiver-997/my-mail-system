#ifndef PR_FRAMEWORK_STORAGE_BUFFERED_UPLOAD_STREAM_H
#define PR_FRAMEWORK_STORAGE_BUFFERED_UPLOAD_STREAM_H

#include "framework/storage/i_write_stream.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace pr {


// 整对象内存缓冲写入句柄：write_at 只攒缓冲，commit 时把整个对象一次性交给
// 上传函数。给「按块追加 = 每块一次网络往返」的远程后端（S3/WebHDFS）用：
//
//   - S3 走 append_binary 的旧路径每块要 GET 全量 + PUT 全量（O(n²) 字节），
//     改成本流后整封邮件只做一次 PUT；PUT 本身就是整对象原子替换。
//   - WebHDFS 每块 append 是 MKDIRS + 307 + POST 多次往返，改成本流后只在
//     commit 时做一次 CREATE(overwrite=true)。
//
// 内存约束：对象完整地驻留在堆上直到 commit，因此必须带硬上限（构造参数
// max_bytes）。SMTP 侧本就有 10MB 正文上限，但原始字节在超限后仍会继续流向
// 写入流（到 DATA_END 才拒收），上限是防止恶意/异常客户端把内存当磁盘用。
// 超限后 write_at 失败 → MailBodyWriter 进入 failed → DATA_END 回 451。
//
// abort 语义：commit 之前没有任何字节离开本机，直接丢弃缓冲即可；只有
// commit 尝试过（可能已在远端留下半成品）才调用 cleanup 清除远端对象。
class BufferedUploadStream : public IWriteStream {
public:
    // 把 (data, size) 作为完整对象上传。size 可能为 0（空对象也要在远端存在，
    // 与 LocalFileWriteStream 打开即创建空文件对齐）。失败的 kind 由后端判定。
    using UploadFn = std::function<bool(const char* data, std::size_t size,
                                        IoError& error)>;
    // 清除远端半成品对象（commit 失败后的 abort 路径）。不得抛异常。
    using CleanupFn = std::function<void()>;

    BufferedUploadStream(UploadFn upload, CleanupFn cleanup, std::size_t max_bytes)
        : upload_(std::move(upload)), cleanup_(std::move(cleanup)), max_bytes_(max_bytes) {}

    bool write_at(std::uint64_t offset,
                  const char* data,
                  std::size_t size,
                  IoError& error) override {
        if (committed_ || failed_) {
            error = IoError::permanent(committed_ ? "buffered upload already committed"
                                                  : "buffered upload already failed");
            return false;
        }
        if (!upload_) {
            error = IoError::permanent("buffered upload has no upload function");
            return false;
        }
        if (offset != buffer_.size()) {
            error = IoError::permanent(
                "out-of-order write: expected offset " + std::to_string(buffer_.size()) +
                ", got " + std::to_string(offset));
            return false;
        }
        if (buffer_.size() + size > max_bytes_) {
            failed_ = true;
            // 超的是本后端的缓冲上限，重投同一封邮件结果不变 → permanent。
            error = IoError::permanent(
                "object exceeds buffered upload limit " + std::to_string(max_bytes_) +
                " bytes (already buffered " + std::to_string(buffer_.size()) + ")");
            return false;
        }
        buffer_.append(data, size);
        return true;
    }

    bool commit(IoError& error) override {
        if (committed_) {
            return true;
        }
        if (failed_) {
            error = IoError::permanent("buffered upload already failed");
            return false;
        }
        if (!upload_) {
            error = IoError::permanent("buffered upload has no upload function");
            return false;
        }
        upload_attempted_ = true;
        if (!upload_(buffer_.data(), buffer_.size(), error)) {
            failed_ = true;
            return false;
        }
        committed_ = true;
        std::string().swap(buffer_);   // 尽早归还内存，别等析构
        return true;
    }

    void abort() noexcept override {
        if (committed_) {
            return;
        }
        if (upload_attempted_ && cleanup_) {
            cleanup_();   // commit 试过且失败，远端可能有半成品
        }
        upload_attempted_ = false;
        failed_ = true;
        std::string().swap(buffer_);
    }

    ~BufferedUploadStream() override { abort(); }

    std::size_t buffered_bytes() const { return buffer_.size(); }

private:
    UploadFn upload_;
    CleanupFn cleanup_;
    std::size_t max_bytes_;
    std::string buffer_;
    bool committed_ = false;
    bool failed_ = false;
    bool upload_attempted_ = false;
};


} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_BUFFERED_UPLOAD_STREAM_H
