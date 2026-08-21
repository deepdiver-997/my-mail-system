#ifndef MAIL_SYSTEM_STORAGE_DISTRIBUTED_FILE_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_DISTRIBUTED_FILE_STORAGE_PROVIDER_H

#include "mail_system/back/storage/i_storage_provider.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace mail_system {
namespace storage {

class DistributedFileStorageProvider : public IStorageProvider {
public:
    DistributedFileStorageProvider(std::vector<std::string> storage_roots,
                                   std::size_t replica_count = 1);

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

    // 依次尝试各副本，任一可读即成功
    bool read_all(const std::string& storage_key,
                  std::string& out,
                  IoError& error) override;

private:
    static std::string ensure_trailing_slash(const std::string& path);
    static std::string sanitize_filename(const std::string& name);

    std::string strip_root_prefix(const std::string& storage_key) const;
    std::size_t get_primary_index(const std::string& relative_key) const;
    std::vector<std::size_t> pick_replica_indices(const std::string& relative_key) const;

    std::vector<std::string> roots_;
    std::size_t replica_count_;
    std::atomic<std::uint64_t> attachment_seq_{0};
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_DISTRIBUTED_FILE_STORAGE_PROVIDER_H
