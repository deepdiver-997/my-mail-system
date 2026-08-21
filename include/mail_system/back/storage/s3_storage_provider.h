#ifndef MAIL_SYSTEM_STORAGE_S3_STORAGE_PROVIDER_H
#define MAIL_SYSTEM_STORAGE_S3_STORAGE_PROVIDER_H

#include "mail_system/back/storage/i_storage_provider.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace mail_system {
namespace storage {

// S3/MinIO 对象存储驱动 — 通过 HTTP PUT/GET/DELETE + AWS Signature V4
// 流式写入走 open_write：整对象内存缓冲，commit 时一次 PUT（PUT 即整对象原子替换）。
// append_binary 保留给一次性写完整内容的场景（如 IMAP APPEND）。
class S3StorageProvider : public IStorageProvider {
public:
    S3StorageProvider(std::string endpoint,
                      std::string bucket,
                      std::string access_key,
                      std::string secret_key,
                      std::string region = "us-east-1",
                      long timeout_ms = 5000,
                      bool use_path_style = true,
                      std::size_t max_write_buffer_bytes = kDefaultWriteBufferBytes);

    // SMTP 正文上限 10MB，64MB 给整对象缓冲留足余量（附件随正文一起落盘）。
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

    // 复用已有的签名 GET（s3_get）
    bool read_all(const std::string& storage_key,
                  std::string& out,
                  IoError& error) override;

    // 整对象缓冲 + commit 时单次 s3_put，替代逐块 GET+PUT 的 O(n²) 老路径。
    std::unique_ptr<IWriteStream> open_write(const std::string& storage_key,
                                             IoError& error) override;

private:
    // --- HTTP 底层 ---
    bool s3_get(const std::string& key, std::string& body, IoError& error);
    bool s3_put(const std::string& key, const char* data, std::size_t size, IoError& error);
    bool s3_delete(const std::string& key, IoError& error);

    // --- AWS Signature V4 ---
    std::string sign_request(const std::string& method,
                             const std::string& key,
                             const std::string& payload_hash,
                             const std::map<std::string, std::string>& extra_headers) const;
    static std::string sha256_hex(const std::string& data);
    static std::string sha256_hex(const char* data, std::size_t size);
    static std::string hmac_sha256(const std::string& key, const std::string& msg);
    static std::string hmac_sha256_hex(const std::string& key, const std::string& msg);
    static std::string hex_encode(const unsigned char* data, std::size_t len);
    std::string iso8601_basic() const;
    std::string iso8601_date() const;

    // --- 配置 ---
    std::string endpoint_;
    std::string bucket_;
    std::string access_key_;
    std::string secret_key_;
    std::string region_;
    long timeout_ms_;
    bool use_path_style_;
    std::size_t max_write_buffer_bytes_;

    std::atomic<std::uint64_t> attachment_seq_{0};
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_S3_STORAGE_PROVIDER_H
