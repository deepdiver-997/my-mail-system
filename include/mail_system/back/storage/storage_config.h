#ifndef MAIL_SYSTEM_STORAGE_CONFIG_H
#define MAIL_SYSTEM_STORAGE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mail_system {
namespace storage {

// === 各后端的专属配置 ===
struct LocalStorageConfig {
    std::string mail_path;
    std::string attachment_path;
};

struct S3StorageConfig {
    std::string endpoint = "http://127.0.0.1:9000";
    std::string bucket   = "protorelay";
    std::string access_key = "minioadmin";
    std::string secret_key = "minioadmin";
    std::string region     = "us-east-1";
    uint32_t timeout_ms    = 5000;
    bool use_path_style    = true;
    // 流式写入的整对象内存缓冲上限（open_write 攒到 commit 一次上传）。
    uint64_t max_write_buffer_bytes = 64ull * 1024 * 1024;
};

struct HdfsStorageConfig {
    std::string endpoint    = "http://127.0.0.1:9870";
    std::string base_path   = "/mail-system";
    std::string user        = "hdfs";
    uint32_t timeout_ms     = 5000;
    size_t replication      = 1;
    // 同 S3StorageConfig::max_write_buffer_bytes。
    uint64_t max_write_buffer_bytes = 64ull * 1024 * 1024;
};

struct DistributedStorageConfig {
    std::vector<std::string> roots;
    size_t replica_count = 1;
};

// === 统一存储配置 ===
enum class StorageProviderType { Local, Null, S3, HdfsWeb, Distributed };

inline StorageProviderType provider_type_from_string(const std::string& s) {
    if (s == "distributed") return StorageProviderType::Distributed;
    if (s == "hdfs_web")    return StorageProviderType::HdfsWeb;
    if (s == "null")        return StorageProviderType::Null;
    if (s == "s3")          return StorageProviderType::S3;
    return StorageProviderType::Local;
}

struct StorageConfig {
    std::string provider = "local";  // "local" | "null" | "s3" | "hdfs_web" | "distributed"

    LocalStorageConfig local;
    S3StorageConfig s3;
    HdfsStorageConfig hdfs;
    DistributedStorageConfig distributed;

    static StorageConfig from_json(const nlohmann::json& j, const std::string& base_dir = "") {
        StorageConfig cfg;
        cfg.provider = j.value("provider", cfg.provider);

        auto resolve = [&](const std::string& path) {
            if (path.empty() || path[0] == '/' || base_dir.empty()) return path;
            return base_dir + "/" + path;
        };

        if (j.contains("local")) {
            auto& l = j["local"];
            cfg.local.mail_path       = resolve(l.value("mail_path", ""));
            cfg.local.attachment_path = resolve(l.value("attachment_path", ""));
        }

        if (j.contains("s3")) {
            auto& s = j["s3"];
            cfg.s3.endpoint       = s.value("endpoint", cfg.s3.endpoint);
            cfg.s3.bucket         = s.value("bucket", cfg.s3.bucket);
            cfg.s3.access_key     = s.value("access_key", cfg.s3.access_key);
            cfg.s3.secret_key     = s.value("secret_key", cfg.s3.secret_key);
            cfg.s3.region         = s.value("region", cfg.s3.region);
            cfg.s3.timeout_ms     = s.value("timeout_ms", cfg.s3.timeout_ms);
            cfg.s3.use_path_style = s.value("use_path_style", cfg.s3.use_path_style);
            cfg.s3.max_write_buffer_bytes = s.value("max_write_buffer_bytes",
                                                    cfg.s3.max_write_buffer_bytes);
        }

        if (j.contains("hdfs")) {
            auto& h = j["hdfs"];
            cfg.hdfs.endpoint    = h.value("endpoint", cfg.hdfs.endpoint);
            cfg.hdfs.base_path   = h.value("base_path", cfg.hdfs.base_path);
            cfg.hdfs.user        = h.value("user", cfg.hdfs.user);
            cfg.hdfs.timeout_ms  = h.value("timeout_ms", cfg.hdfs.timeout_ms);
            cfg.hdfs.replication = h.value("replication", cfg.hdfs.replication);
            cfg.hdfs.max_write_buffer_bytes = h.value("max_write_buffer_bytes",
                                                      cfg.hdfs.max_write_buffer_bytes);
        }

        if (j.contains("distributed")) {
            auto& d = j["distributed"];
            cfg.distributed.replica_count = d.value("replica_count", cfg.distributed.replica_count);
            if (d.contains("roots") && d["roots"].is_array()) {
                for (auto& r : d["roots"]) {
                    std::string p = resolve(r.get<std::string>());
                    if (!p.empty()) cfg.distributed.roots.push_back(p);
                }
            }
        }

        return cfg;
    }
};

} // namespace storage
} // namespace mail_system
#endif
