#ifndef MAIL_SYSTEM_STORAGE_MAIL_BODY_WRITER_H
#define MAIL_SYSTEM_STORAGE_MAIL_BODY_WRITER_H

#include "mail_system/back/storage/i_write_stream.h"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace mail_system {
namespace storage {

// 邮件正文的缓冲写入器：固定容量缓冲 + 单一偏移量 + RAII 兜底。
//
// 取代了此前散落在 SmtpsSession 里的 expand_buffer / flush_buffer_to_disk /
// async_flush_buffer_to_disk / wait_for_async_writes / flush_body_and_wait 五件套。
// 三条关键设计：
//   1. offset_ 是文件位置的唯一真相，且只在 emit() 一处推进 —— 写错位在结构上无法表达。
//   2. 不做异步。给 page cache 写 64KB 是十几微秒量级，而为此拷一份缓冲区再派发到
//      线程池，开销比它想省掉的那次写还大，顺带把顺序也丢了。
//   3. 未 commit 就析构 = abort，半成品文件不会留在盘上被当成正常邮件解析。
//
// 非线程安全：刻意如此。单个会话的正文只允许一个写者。
class MailBodyWriter {
public:
    static constexpr std::size_t DEFAULT_BUFFER_SIZE = 64 * 1024;

    explicit MailBodyWriter(std::unique_ptr<IWriteStream> stream,
                            std::size_t buffer_capacity = DEFAULT_BUFFER_SIZE)
        : stream_(std::move(stream)),
          buffer_(buffer_capacity == 0 ? DEFAULT_BUFFER_SIZE : buffer_capacity) {}

    ~MailBodyWriter() { abort(); }

    MailBodyWriter(const MailBodyWriter&) = delete;
    MailBodyWriter& operator=(const MailBodyWriter&) = delete;

    bool valid() const { return static_cast<bool>(stream_); }
    bool committed() const { return committed_; }
    // 底层写入是否已失败。失败后 write/commit 恒返回 false，
    // 调用方（如 SmtpsSession::append_body_data）据此对后续数据只丢弃不再重试。
    bool failed() const { return failed_; }

    // 已接收的正文总字节数（含尚未刷盘的缓冲部分）。
    std::uint64_t bytes_total() const { return offset_ + buffer_used_; }

    bool write(const char* data, std::size_t size, std::string& error) {
        if (failed_) {
            error = "mail body writer already failed";
            return false;
        }
        if (!stream_) {
            error = "mail body writer has no write stream";
            return false;
        }
        if (!data || size == 0) {
            return true;
        }

        if (buffer_used_ + size > buffer_.size() && !flush(error)) {
            return false;
        }
        // 刷过之后仍放不下，说明单块就超过缓冲容量：直穿，省掉一次无谓的拷贝。
        // 走的仍是 emit()，同样推进 offset_，不是特例分支。
        if (size > buffer_.size()) {
            return emit(data, size, error);
        }

        std::memcpy(buffer_.data() + buffer_used_, data, size);
        buffer_used_ += size;
        return true;
    }

    // 冲刷剩余缓冲并持久化。返回 true 后才可以回 250。
    bool commit(std::string& error) {
        if (committed_) {
            return true;
        }
        if (failed_) {
            error = "mail body writer already failed";
            return false;
        }
        if (!stream_) {
            error = "mail body writer has no write stream";
            return false;
        }
        if (!flush(error)) {
            return false;
        }
        if (!stream_->commit(error)) {
            failed_ = true;
            return false;
        }
        committed_ = true;
        return true;
    }

    // 幂等：重复调用、以及 commit 之后再调用都安全。
    void abort() noexcept {
        if (!stream_) {
            return;
        }
        if (!committed_) {
            stream_->abort();
        }
        stream_.reset();
        buffer_used_ = 0;
    }

private:
    bool flush(std::string& error) {
        if (buffer_used_ == 0) {
            return true;
        }
        if (!emit(buffer_.data(), buffer_used_, error)) {
            return false;
        }
        buffer_used_ = 0;
        return true;
    }

    bool emit(const char* data, std::size_t size, std::string& error) {
        if (!stream_->write_at(offset_, data, size, error)) {
            failed_ = true;
            return false;
        }
        offset_ += size;
        return true;
    }

    std::unique_ptr<IWriteStream> stream_;
    std::vector<char> buffer_;
    std::size_t buffer_used_ = 0;
    std::uint64_t offset_ = 0;   // 已落盘字节数 —— 文件位置的唯一真相
    bool committed_ = false;
    bool failed_ = false;
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_MAIL_BODY_WRITER_H
