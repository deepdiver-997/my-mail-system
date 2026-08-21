#ifndef MAIL_SYSTEM_STORAGE_ASYNC_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_ASYNC_STORAGE_PROVIDER_H

#include "mail_system/back/storage/i_storage_provider.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace mail_system {
namespace storage {

// 远程后端的真异步装饰器：把「ms 级阻塞的网络操作」从 io 线程挪到 executor
// （server 的 worker 线程池），底层 provider 保持纯同步实现。
//
// 「异步化与否由后端决定」——这个决定就在装配时做：server_base 只给
// S3/WebHDFS 这类远程后端套本装饰器；本地后端（stat/mmap/fsync-进-page-cache
// 都是 µs~ms 级）保持默认内联，零额外开销。
//
// 线程契约与 IWriteStream::commit_async / IStorageProvider::async_* 一致：
// 回调在 executor 线程触发，调用方必须已 set_paused(true) 取得 session
// 独占（SPF/DNS/commit 回调同一约定）。SMTP DATA_END 链路已按此改造，
// 套上本装饰器后 S3/HDFS 的 PUT/GET 即不再阻塞 io 线程。

// 包装内层写入流：write_at/abort 透传（内存操作，µs 级），
// 只有 commit（fsync / PUT，可能毫秒~百毫秒）投递到 executor。
class AsyncCommitStream : public IWriteStream {
public:
    using Executor = std::function<void(std::function<void()>)>;

    AsyncCommitStream(std::shared_ptr<IWriteStream> inner, Executor executor)
        : inner_(std::move(inner)), executor_(std::move(executor)) {}

    bool write_at(std::uint64_t offset, const char* data, std::size_t size,
                  IoError& error) override {
        return inner_->write_at(offset, data, size, error);
    }

    bool commit(IoError& error) override {
        return inner_->commit(error);
    }

    void commit_async(CommitCallback cb) override {
        // 以 shared_ptr 持有内层流：投递与执行之间外层流即使被析构，
        // 内层仍存活；外层本身由 MailBodyWriter/session 的回调链保活。
        auto inner = inner_;
        executor_([inner, cb = std::move(cb)]() mutable {
            IoError error;
            const bool ok = inner->commit(error);
            if (cb) cb(ok, ok ? IoError{} : std::move(error));
        });
    }

    void abort() noexcept override {
        inner_->abort();
    }

private:
    std::shared_ptr<IWriteStream> inner_;
    Executor executor_;
};

class AsyncStorageProvider : public IStorageProvider {
public:
    using Executor = AsyncCommitStream::Executor;

    // executor 为空时退化为内联（等同不包装）。
    AsyncStorageProvider(std::shared_ptr<IStorageProvider> inner, Executor executor)
        : inner_(std::move(inner)), executor_(std::move(executor)) {}

    bool ensure_ready(IoError& error) override { return inner_->ensure_ready(error); }

    std::string build_mail_body_key(std::uint64_t mail_id) override {
        return inner_->build_mail_body_key(mail_id);
    }

    std::string build_attachment_key(std::uint64_t mail_id,
                                     const std::string& original_filename) override {
        return inner_->build_attachment_key(mail_id, original_filename);
    }

    bool append_binary(const std::string& storage_key, const char* data,
                       std::size_t size, IoError& error) override {
        return inner_->append_binary(storage_key, data, size, error);
    }

    bool remove_object(const std::string& storage_key, IoError& error) override {
        return inner_->remove_object(storage_key, error);
    }

    bool read_all(const std::string& storage_key, std::string& out,
                  IoError& error) override {
        return inner_->read_all(storage_key, out, error);
    }

    std::unique_ptr<IReadStream> open_read(const std::string& storage_key,
                                           IoError& error) override {
        return inner_->open_read(storage_key, error);
    }

    bool object_size(const std::string& storage_key, std::uint64_t& size,
                     IoError& error) override {
        return inner_->object_size(storage_key, size, error);
    }

    std::unique_ptr<IWriteStream> open_write(const std::string& storage_key,
                                             IoError& error) override {
        auto stream = inner_->open_write(storage_key, error);
        if (!stream) {
            return nullptr;
        }
        return std::make_unique<AsyncCommitStream>(
            std::shared_ptr<IWriteStream>(std::move(stream)), executor_);
    }

    // ── 三个异步读：整次网络往返投递到 executor ──
    // 装饰器的职责就是提供异步性：直接调内层的同步实现，只是不在 io 线程上调。
    // （若某后端将来自带真异步覆写，就不必再套本装饰器。）

    void async_open_read(const std::string& storage_key, OpenReadCallback cb) override {
        dispatch_read([inner = inner_, storage_key, cb = std::move(cb)]() mutable {
            IoError error;
            auto stream = inner->open_read(storage_key, error);
            if (cb) cb(std::move(stream), std::move(error));
        });
    }

    void async_read_all(const std::string& storage_key, ReadAllCallback cb) override {
        dispatch_read([inner = inner_, storage_key, cb = std::move(cb)]() mutable {
            IoError error;
            std::string data;
            const bool ok = inner->read_all(storage_key, data, error);
            if (cb) cb(ok, std::move(data), std::move(error));
        });
    }

    void async_object_size(const std::string& storage_key, ObjectSizeCallback cb) override {
        dispatch_read([inner = inner_, storage_key, cb = std::move(cb)]() mutable {
            IoError error;
            std::uint64_t size = 0;
            const bool ok = inner->object_size(storage_key, size, error);
            if (cb) cb(ok, size, std::move(error));
        });
    }

private:
    void dispatch_read(std::function<void()> task) {
        if (executor_) {
            executor_(std::move(task));
        } else {
            task();
        }
    }

    std::shared_ptr<IStorageProvider> inner_;
    Executor executor_;
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_ASYNC_STORAGE_PROVIDER_H
