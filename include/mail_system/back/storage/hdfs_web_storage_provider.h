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
                           long timeout_ms = 5000);

    bool ensure_ready(std::string& error) override;

    std::string build_mail_body_key(std::uint64_t mail_id) override;

    std::string build_attachment_key(std::uint64_t mail_id,
                                     const std::string& original_filename) override;

    bool append_binary(const std::string& storage_key,
                       const char* data,
                       std::size_t size,
                       std::string& error) override;

    bool remove_object(const std::string& storage_key,
                       std::string& error) override;

    // WebHDFS OPEN（307 重定向到 datanode 后再取正文）
    bool read_all(const std::string& storage_key,
                  std::string& out,
                  std::string& error) override;

private:
    static std::string normalize_endpoint(const std::string& endpoint);
    static std::string normalize_base_path(const std::string& base_path);
    static std::string sanitize_filename(const std::string& name);
    static std::string url_encode(const std::string& value);

    bool ensure_remote_directory(const std::string& relative_dir, std::string& error);
    bool webhdfs_mkdirs(const std::string& relative_dir, std::string& error);
    bool webhdfs_append(const std::string& relative_path,
                        const char* data,
                        std::size_t size,
                        std::string& error);
    bool webhdfs_create_with_payload(const std::string& relative_path,
                                     const char* data,
                                     std::size_t size,
                                     std::string& error);
    bool webhdfs_delete(const std::string& relative_path, std::string& error);

    std::string endpoint_;
    std::string base_path_;
    std::string user_;
    std::size_t replica_count_;
    long timeout_ms_;

    std::atomic<std::uint64_t> attachment_seq_{0};
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_HDFS_WEB_STORAGE_PROVIDER_H
