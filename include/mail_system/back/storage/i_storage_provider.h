#ifndef MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H

#include "framework/storage/i_read_stream.h"
#include "framework/storage/i_write_stream.h"
#include "framework/storage/buffered_upload_stream.h"
#include "framework/storage/local_file_read_stream.h"
#include "framework/storage/local_file_write_stream.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mail_system {
namespace storage {

// 向后兼容：通用流原语已上移到 framework/storage（namespace pr，与
// framework/db 同层）。IStorageProvider 与邮件命名规则（build_mail_body_key
// 等）留在应用层。
using pr::IReadStream;
using pr::IWriteStream;
using pr::BufferedReadStream;
using pr::BufferedUploadStream;
using pr::LocalFileWriteStream;
using pr::MappedReadStream;
using CommitCallback = pr::IWriteStream::CommitCallback;
using pr::IoError;

class IStorageProvider {
public:
    virtual ~IStorageProvider() = default;

    virtual bool ensure_ready(IoError& error) = 0;

    virtual std::string build_mail_body_key(std::uint64_t mail_id) = 0;

    virtual std::string build_attachment_key(std::uint64_t mail_id,
                                             const std::string& original_filename) = 0;

    // 一次性写入整个对象（IMAP APPEND 等已持有完整内容的场景）。
    // 流式接收请改用 open_write()：append_binary 的位置由后端当前末尾决定，
    // 多次调用之间没有任何顺序保证。
    virtual bool append_binary(const std::string& storage_key,
                               const char* data,
                               std::size_t size,
                               IoError& error) = 0;

    virtual bool remove_object(const std::string& storage_key,
                               IoError& error) = 0;

    // 打开一个写入句柄，用于流式写入单个对象（如 SMTP DATA 阶段的邮件正文）。
    // 默认实现基于 append_binary 适配，对仅支持追加的后端已经够用；
    // 本地文件后端覆写为常开 fd + pwrite，省掉每块 stat/open/close 的开销。
    // 失败返回 nullptr 并填充 error。
    virtual std::unique_ptr<IWriteStream> open_write(const std::string& storage_key,
                                                     IoError& error);

    // ── 读侧 ────────────────────────────────────────────────────────────
    // 读侧此前完全没有抽象：FSM 直接对 body_path 做 std::ifstream。
    // 而 body_path 对 S3/HDFS 来说是 build_mail_body_key() 返回的远程 key，
    // 不是本地路径 —— 远程后端的读路径因此一直是坏的。这三个方法补上这个缺口。

    // 读进调用方自己的缓冲区，内容可随意修改。
    // 纯虚：每个后端都必须明确表态，不留静默走本地文件系统的缺口。
    virtual bool read_all(const std::string& storage_key,
                          std::string& out,
                          IoError& error) = 0;

    // 只读打开。调用方承诺不修改 view() 的内容，后端据此可以零拷贝优化
    // （本地后端覆写为 mmap）。默认实现退化成 read_all + 堆缓冲，
    // 对远程后端而言那次下载和拷贝本来就无法避免。
    virtual std::unique_ptr<IReadStream> open_read(const std::string& storage_key,
                                                   IoError& error);

    // 对象字节数。默认实现会真的把对象读下来（正确但对远程后端偏贵），
    // 本地后端覆写为一次 stat。
    virtual bool object_size(const std::string& storage_key,
                             std::uint64_t& size,
                             IoError& error);

    // ── 读侧异步形状 ────────────────────────────────────────────────────
    // 与 IWriteStream::commit_async 同一模式：默认实现在发起线程内联执行
    // （本地后端 stat/mmap 是 µs 级，零开销、行为与同步版一致），远程后端
    // 将来覆写为真异步（provider 线程下载，完成后触发回调）时调用点不改。
    //
    // 回调线程契约：真异步实现允许在 provider 线程触发回调。调用方必须已
    // 通过流水线 set_paused(true) 取得 session 独占（SPF/DNS/commit 回调
    // 同一约定），或自行把续作 post 回 io 线程。
    //
    // 失败语义与同步版一致：ok=false / stream==nullptr 时 error 必有内容。

    using OpenReadCallback = std::function<void(std::unique_ptr<IReadStream> stream,
                                                const IoError& error)>;
    using ReadAllCallback = std::function<void(bool ok,
                                               std::string data,
                                               const IoError& error)>;
    using ObjectSizeCallback = std::function<void(bool ok,
                                                  std::uint64_t size,
                                                  const IoError& error)>;

    virtual void async_open_read(const std::string& storage_key, OpenReadCallback cb) {
        IoError error;
        auto stream = open_read(storage_key, error);
        if (cb) cb(std::move(stream), std::move(error));
    }

    virtual void async_read_all(const std::string& storage_key, ReadAllCallback cb) {
        IoError error;
        std::string data;
        const bool ok = read_all(storage_key, data, error);
        if (cb) cb(ok, std::move(data), std::move(error));
    }

    virtual void async_object_size(const std::string& storage_key, ObjectSizeCallback cb) {
        IoError error;
        std::uint64_t size = 0;
        const bool ok = object_size(storage_key, size, error);
        if (cb) cb(ok, size, std::move(error));
    }
};

// 把 append_binary 适配成 IWriteStream。
// 位置仍由后端决定，因此这里显式校验 offset 是否与已写入字节数一致：
// 一旦调用方乱序（正是历史上把邮件写错位的那类 bug），此处直接失败而不是静默写坏。
class AppendWriteStream : public IWriteStream {
public:
    AppendWriteStream(IStorageProvider* provider, std::string storage_key)
        : provider_(provider), storage_key_(std::move(storage_key)) {}

    bool write_at(std::uint64_t offset,
                  const char* data,
                  std::size_t size,
                  IoError& error) override {
        if (!provider_) {
            error = IoError::permanent("no storage provider");
            return false;
        }
        if (offset != written_) {
            error = IoError::permanent(
                "out-of-order write: expected offset " + std::to_string(written_) +
                ", got " + std::to_string(offset));
            return false;
        }
        if (!provider_->append_binary(storage_key_, data, size, error)) {
            return false;
        }
        written_ += size;
        return true;
    }

    // 追加型后端在 append_binary 返回时即已落盘，无需额外动作。
    bool commit(IoError&) override { return true; }

    void abort() noexcept override {
        if (!provider_) {
            return;
        }
        try {
            IoError ignored;
            provider_->remove_object(storage_key_, ignored);
        } catch (...) {
            // 析构路径，吞掉
        }
    }

private:
    IStorageProvider* provider_;
    std::string storage_key_;
    std::uint64_t written_ = 0;
};

inline std::unique_ptr<IWriteStream> IStorageProvider::open_write(
    const std::string& storage_key, IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("storage key is empty");
        return nullptr;
    }
    return std::make_unique<AppendWriteStream>(this, storage_key);
}

inline std::unique_ptr<IReadStream> IStorageProvider::open_read(
    const std::string& storage_key, IoError& error) {
    std::string data;
    if (!read_all(storage_key, data, error)) {
        return nullptr;
    }
    return std::make_unique<BufferedReadStream>(std::move(data));
}

inline bool IStorageProvider::object_size(const std::string& storage_key,
                                          std::uint64_t& size,
                                          IoError& error) {
    std::string data;
    if (!read_all(storage_key, data, error)) {
        return false;
    }
    size = data.size();
    return true;
}

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H
