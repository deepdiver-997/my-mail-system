#ifndef MAIL_SYSTEM_STORAGE_HDFS_WEB_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_HDFS_WEB_STORAGE_PROVIDER_H

#include "mail_system/back/storage/i_storage_provider.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mail_system {
namespace storage {

class HdfsWebStorageProvider : public IStorageProvider {
public:
    HdfsWebStorageProvider(std::string endpoint,
                           std::string base_path,
                           std::string user,
                           std::size_t replica_count = 1,
                           long timeout_ms = 5000,
                           std::size_t max_write_buffer_bytes = kDefaultWriteBufferBytes);

    // 与 S3StorageProvider 同一常数：SMTP 正文上限 10MB，64MB 留足余量。
    static constexpr std::size_t kDefaultWriteBufferBytes = 64ull * 1024 * 1024;

    bool ensure_ready(IoError& error) override;

    std::string build_mail_body_key(std::uint64_t mail_id) override;

    std::string build_attachment_key(std::uint64_t mail_id,
                                     const std::string& original_filename) override;

    bool append_binary(const std::string& storage_key,
                       const char* data,
                       std::size_t size,
                       IoError& error) override;

    bool remove_object(const std::string& storage_key,
                       IoError& error) override;

    // WebHDFS OPEN（307 重定向到 datanode 后再取正文）
    bool read_all(const std::string& storage_key,
                  std::string& out,
                  IoError& error) override;

    // 整对象缓冲 + commit 时单次 CREATE(overwrite)，替代逐块 APPEND 的
    // 每块 MKDIRS + 307 + POST 多次往返。
    std::unique_ptr<IWriteStream> open_write(const std::string& storage_key,
                                             IoError& error) override;

private:
    static std::string normalize_endpoint(const std::string& endpoint);
    static std::string normalize_base_path(const std::string& base_path);
    static std::string sanitize_filename(const std::string& name);
    static std::string url_encode(const std::string& value);

    bool ensure_remote_directory(const std::string& relative_dir, IoError& error);
    bool webhdfs_mkdirs(const std::string& relative_dir, IoError& error);
    bool webhdfs_append(const std::string& relative_path,
                        const char* data,
                        std::size_t size,
                        IoError& error);
    bool webhdfs_create_with_payload(const std::string& relative_path,
                                     const char* data,
                                     std::size_t size,
                                     IoError& error);
    bool webhdfs_delete(const std::string& relative_path, IoError& error);

    std::string endpoint_;
    std::string base_path_;
    std::string user_;
    std::size_t replica_count_;
    long timeout_ms_;
    std::size_t max_write_buffer_bytes_;

    std::atomic<std::uint64_t> attachment_seq_{0};
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_HDFS_WEB_STORAGE_PROVIDER_H
