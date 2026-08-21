#ifndef MAIL_SYSTEM_STORAGE_LOCAL_FILE_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_LOCAL_FILE_STORAGE_PROVIDER_H

#include "mail_system/back/storage/i_storage_provider.h"

#include <atomic>
#include <memory>
#include <string>

namespace mail_system {
namespace storage {

class LocalFileStorageProvider : public IStorageProvider {
public:
    LocalFileStorageProvider(std::string mail_root, std::string attachment_root);

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

    // 覆写默认的 append_binary 适配：常开 fd + pwrite，
    // 每块数据省掉一次父目录 stat + open + close。
    std::unique_ptr<IWriteStream> open_write(const std::string& storage_key,
                                            IoError& error) override;

    bool read_all(const std::string& storage_key,
                  std::string& out,
                  IoError& error) override;

    // 覆写为 mmap：零拷贝，省掉 read() 那次 page cache → 用户缓冲区的拷贝
    std::unique_ptr<IReadStream> open_read(const std::string& storage_key,
                                           IoError& error) override;

    // 覆写为一次 stat，不必把对象读下来
    bool object_size(const std::string& storage_key,
                     std::uint64_t& size,
                     IoError& error) override;

private:
    static std::string ensure_trailing_slash(const std::string& path);
    static std::string sanitize_filename(const std::string& name);

    std::string mail_root_;
    std::string attachment_root_;
    std::atomic<std::uint64_t> attachment_seq_{0};
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_LOCAL_FILE_STORAGE_PROVIDER_H
