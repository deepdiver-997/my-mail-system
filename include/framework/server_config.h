#ifndef MAIL_SYSTEM_SERVER_CONFIG_H
#define MAIL_SYSTEM_SERVER_CONFIG_H

#include "framework/server_config_base.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/storage/storage_config.h"
#include "mail_system/back/outbound/outbound_config.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <vector>

namespace mail_system {

// 框架类型向后兼容
using pr::ListenerType;
using pr::ListenerConfig;
using pr::listener_type_to_string;
using pr::listener_type_from_string;
using pr::InboundAuthPolicy;

// 邮件特有枚举 (不属于框架通用配置)
enum class InboundAckMode : int { AFTER_PERSIST = 0, AFTER_ENQUEUE = 1 };

inline const char* inbound_ack_mode_to_string(InboundAckMode mode) {
    switch (mode) {
    case InboundAckMode::AFTER_ENQUEUE: return "after_enqueue";
    case InboundAckMode::AFTER_PERSIST:
    default: return "after_persist";
    }
}

inline InboundAckMode inbound_ack_mode_from_string(const std::string& s) {
    if (s == "after_enqueue") return InboundAckMode::AFTER_ENQUEUE;
    return InboundAckMode::AFTER_PERSIST;
}

// InboundAuthPolicy 转换函数
inline const char* inbound_auth_policy_to_string(InboundAuthPolicy p) {
    switch (p) {
    case InboundAuthPolicy::AUTO: return "auto";
    case InboundAuthPolicy::ON:   return "on";
    case InboundAuthPolicy::OFF:
    default: return "off";
    }
}

inline InboundAuthPolicy inbound_auth_policy_from_string(const std::string& s) {
    if (s == "auto") return InboundAuthPolicy::AUTO;
    if (s == "on")   return InboundAuthPolicy::ON;
    return InboundAuthPolicy::OFF;
}

// 扩展的 ListenerConfig —— 在框架基类基础上增加邮件特有字段
struct MailListenerConfig : public pr::ListenerConfig {
    std::string spf_mode   = "off";
    std::string dkim_mode  = "off";
    std::string dmarc_mode = "off";

    void show() const {
        LOG_SERVER_INFO("  [{}:{}] auth={} spf={} dkim={} dmarc={}",
                        pr::listener_type_to_string(type), port,
                        inbound_auth_policy_to_string(auth_policy),
                        spf_mode, dkim_mode, dmarc_mode);
    }

    static MailListenerConfig from_json(const nlohmann::json& item) {
        MailListenerConfig lc;
        lc.type   = pr::listener_type_from_string(item.value("type", "tcp"));
        lc.port   = static_cast<uint16_t>(item.value("port", 0));
        lc.auth_policy = inbound_auth_policy_from_string(item.value("auth_policy", "off"));
        lc.spf_mode   = item.value("spf_mode", "off");
        lc.dkim_mode  = item.value("dkim_mode", "off");
        lc.dmarc_mode = item.value("dmarc_mode", "off");
        return lc;
    }
};

// ============================================================
// 分片路由配置
// ============================================================
struct ShardRouterConfig {
    std::string type = "hash";           // "hash" / "table" / "static"
    std::string table_name = "user_shards";
    std::string email_column = "email";
    std::string shard_column = "shard_id";
    size_t shard_count = 1;
    size_t cache_capacity = 100000;

    std::vector<std::pair<std::string, int>> static_mappings;
    int default_shard = 0;

    struct ShardEntry {
        std::string db_config_file;
        std::string storage_root;
    };
    std::vector<ShardEntry> shards;

    bool loadFromJson(const std::string& filename) {
        std::ifstream f(filename);
        if (!f.is_open()) return false;
        nlohmann::json j;
        f >> j;

        type            = j.value("type", type);
        table_name      = j.value("table_name", table_name);
        email_column    = j.value("email_column", email_column);
        shard_column    = j.value("shard_column", shard_column);
        shard_count     = j.value("shard_count", shard_count);
        cache_capacity  = j.value("cache_capacity", cache_capacity);
        default_shard   = j.value("default_shard", default_shard);

        if (j.contains("mappings") && j["mappings"].is_array()) {
            static_mappings.clear();
            for (auto& m : j["mappings"]) {
                static_mappings.emplace_back(m.value("domain", ""), m.value("shard", 0));
            }
        }

        if (j.contains("shards") && j["shards"].is_array()) {
            shards.clear();
            for (auto& s : j["shards"]) {
                ShardEntry e;
                e.db_config_file = s.value("db_config_file", "");
                e.storage_root   = s.value("storage_root", "");
                shards.push_back(e);
            }
        }

        return true;
    }
};

// ============================================================
// ServerConfig — 邮件服务器配置 (继承框架 ServerConfig)
// ============================================================
struct ServerConfig : public pr::ServerConfig {
    // 扩展的监听器 (取代基类的 ListenerConfig)
    std::vector<MailListenerConfig> mail_listeners;

    bool use_database;
    DBPoolConfig db_pool_config;

    bool dnsbl_enabled;
    bool perf_mode;

    std::string router_type;
    std::string router_config_file;
    ShardRouterConfig router_config;

    storage::StorageConfig storage;
    std::string storage_provider;
    std::string mail_storage_path;
    std::string attachment_storage_path;
    std::vector<std::string> distributed_storage_roots;
    size_t distributed_storage_replica_count;
    std::string hdfs_endpoint;
    std::string hdfs_base_path;
    std::string hdfs_user;
    uint32_t hdfs_timeout_ms;
    size_t hdfs_replication;

    std::string s3_endpoint;
    std::string s3_bucket;
    std::string s3_access_key;
    std::string s3_secret_key;
    std::string s3_region;
    uint32_t s3_timeout_ms;
    bool s3_use_path_style;

    std::string system_name;
    std::string system_domain;
    std::string helo_hostname;  // 入站 EHLO 响应首行主机名（RFC 5321：必须为服务器自身域名，非回显客户端）
    std::vector<uint16_t> outbound_ports;
    size_t outbound_max_attempts;
    uint32_t outbound_poll_busy_sleep_ms;
    uint32_t outbound_poll_backoff_base_ms;
    uint32_t outbound_poll_backoff_max_ms;
    uint32_t outbound_poll_backoff_shift_cap;

    InboundAckMode inbound_ack_mode;
    uint32_t inbound_persist_wait_timeout_ms;
    size_t inbound_mime_parse_limit_bytes;  // MIME eager 解析阈值：超过则跳过，交给 IMAP lazy 解析
    size_t persist_max_inflight_mails;
    size_t persist_min_available_memory_mb;
    size_t persist_min_db_available_connections;

    std::string outbound_helo_domain;
    std::string outbound_mail_from_domain;
    bool outbound_rewrite_header_from;
    bool outbound_dkim_enabled;
    std::string outbound_dkim_selector;
    std::string outbound_dkim_domain;
    std::string outbound_dkim_private_key_file;

    bool metrics_enabled;
    uint16_t metrics_port;
    std::string metrics_bind_address;

    InboundAuthPolicy inbound_auth_policy;
    std::string inbound_spf_mode;
    std::string inbound_dkim_mode;
    std::string inbound_dmarc_mode;
    uint32_t inbound_auth_timeout_ms;

    bool intrusion_detection_enabled;
    int  intrusion_persist_interval_sec;
    int  intrusion_persist_dirty_threshold;
    int  intrusion_max_records;
    int  intrusion_ban_threshold;

    ServerConfig()
        : use_database(false)
        , dnsbl_enabled(true)
        , perf_mode(false)
        , router_type("hash")
        , storage_provider("local")
        , distributed_storage_replica_count(1)
        , hdfs_endpoint("http://127.0.0.1:9870")
        , hdfs_base_path("/mail-system")
        , hdfs_user("hdfs")
        , hdfs_timeout_ms(5000)
        , hdfs_replication(1)
        , s3_endpoint("http://127.0.0.1:9000")
        , s3_bucket("protorelay")
        , s3_access_key("minioadmin")
        , s3_secret_key("minioadmin")
        , s3_region("us-east-1")
        , s3_timeout_ms(5000)
        , s3_use_path_style(true)
        , system_name("mail-system")
        , system_domain("example.com")
        , helo_hostname("")
        , outbound_ports({25, 587, 465})
        , outbound_max_attempts(8)
        , outbound_poll_busy_sleep_ms(20)
        , outbound_poll_backoff_base_ms(50)
        , outbound_poll_backoff_max_ms(1200)
        , outbound_poll_backoff_shift_cap(6)
        , inbound_ack_mode(InboundAckMode::AFTER_PERSIST)
        , inbound_persist_wait_timeout_ms(5000)
        , inbound_mime_parse_limit_bytes(1024 * 1024)
        , persist_max_inflight_mails(2048)
        , persist_min_available_memory_mb(256)
        , persist_min_db_available_connections(1)
        , outbound_helo_domain("outbound.local")
        , outbound_rewrite_header_from(true)
        , outbound_dkim_enabled(false)
        , outbound_dkim_selector("default")
        , metrics_enabled(false)
        , metrics_port(9090)
        , metrics_bind_address("127.0.0.1")
        , inbound_auth_policy(InboundAuthPolicy::OFF)
        , inbound_spf_mode("off")
        , inbound_dkim_mode("off")
        , inbound_dmarc_mode("off")
        , inbound_auth_timeout_ms(30000)
        , intrusion_detection_enabled(false)
        , intrusion_persist_interval_sec(60)
        , intrusion_persist_dirty_threshold(256)
        , intrusion_max_records(10000)
        , intrusion_ban_threshold(0)
    {}

    void show() const {
        LOG_SERVER_INFO("address={} io_threads={} worker_threads={} maxConn={} use_db={} domain={}",
                        address, io_thread_count, worker_thread_count, maxConnections,
                        use_database, system_domain);
        for (auto& l : mail_listeners) l.show();
        LOG_SERVER_INFO("router: type={} shards={} cert={} key={}",
                        router_type, router_config.shard_count,
                        certFile.empty() ? "(none)" : certFile,
                        keyFile.empty() ? "(none)" : keyFile);
        LOG_SERVER_INFO("inbound: auth={} spf={} dkim={} dmarc={} intrusion={} metrics={}",
                        inbound_auth_policy_to_string(inbound_auth_policy),
                        inbound_spf_mode, inbound_dkim_mode, inbound_dmarc_mode,
                        intrusion_detection_enabled, metrics_enabled);
        LOG_SERVER_INFO("outbound: helo_domain={} dkim={}",
                        outbound_helo_domain, outbound_dkim_enabled);
    }

    bool validate() const override {
        if (!pr::ServerConfig::validate()) return false;
        if (mail_listeners.empty()) { LOG_SERVER_ERROR("Error: at least one listener required"); return false; }
        if (inbound_persist_wait_timeout_ms == 0) {
            LOG_SERVER_ERROR("Error: inbound_persist_wait_timeout_ms 0"); return false;
        }
        if (storage_provider == "distributed" && distributed_storage_roots.empty()) {
            LOG_SERVER_ERROR("Error: distributed needs roots"); return false;
        }
        return true;
    }

    bool loadFromFile(const std::string& filename) {
        std::ifstream config_file(filename);
        if (!config_file.is_open()) {
            LOG_SERVER_ERROR("Failed to open config: {}", filename);
            return false;
        }
        nlohmann::json j;
        config_file >> j;
        std::string base_dir = std::filesystem::path(filename).parent_path().string();

        // 1. 加载 framework 级字段
        pr::ServerConfig::loadFromJson(j, base_dir);

        // 2. 加载邮件特有 listeners (覆盖基类的 listeners)
        if (j.contains("listeners") && j["listeners"].is_array()) {
            mail_listeners.clear();
            listeners.clear();
            for (auto& item : j["listeners"]) {
                auto lc = MailListenerConfig::from_json(item);
                if (lc.port != 0) {
                    mail_listeners.push_back(lc);
                    listeners.push_back(lc);  // MailListenerConfig → pr::ListenerConfig slice
                }
            }
        }

        // 3. 加载邮件特有字段
        use_database = j.value("use_database", use_database);
        if (use_database) {
            std::string db_file = resolve_path(filename, j.value("db_config_file", ""));
            db_pool_config.loadFromJson(db_file);
        }
        dnsbl_enabled     = j.value("dnsbl_enabled", dnsbl_enabled);
        perf_mode         = j.value("perf_mode", perf_mode);
        router_type       = j.value("router_type", router_type);
        router_config_file = resolve_path(filename, j.value("router_config_file", ""));
        if (!router_config_file.empty() && std::filesystem::exists(router_config_file)) {
            router_config.loadFromJson(router_config_file);
        } else {
            router_config.type = router_type;
            router_config.shard_count = 1;
        }

        if (j.contains("storage") && j["storage"].is_object()) {
            storage = storage::StorageConfig::from_json(j["storage"], base_dir);
        }

        system_name           = j.value("system_name", system_name);
        system_domain         = j.value("system_domain", system_domain);
        helo_hostname         = j.value("helo_hostname", helo_hostname);
        outbound_helo_domain  = j.value("outbound_helo_domain", outbound_helo_domain);
        outbound_mail_from_domain = j.value("outbound_mail_from_domain", outbound_mail_from_domain);
        outbound_rewrite_header_from = j.value("outbound_rewrite_header_from", outbound_rewrite_header_from);
        outbound_dkim_enabled = j.value("outbound_dkim_enabled", outbound_dkim_enabled);
        outbound_dkim_selector  = j.value("outbound_dkim_selector", outbound_dkim_selector);
        outbound_dkim_domain    = j.value("outbound_dkim_domain", outbound_dkim_domain);
        outbound_dkim_private_key_file = resolve_path(filename,
            j.value("outbound_dkim_private_key_file", outbound_dkim_private_key_file));
        outbound_max_attempts  = j.value("outbound_max_attempts", outbound_max_attempts);
        outbound_poll_busy_sleep_ms  = j.value("outbound_poll_busy_sleep_ms", outbound_poll_busy_sleep_ms);
        outbound_poll_backoff_base_ms= j.value("outbound_poll_backoff_base_ms", outbound_poll_backoff_base_ms);
        outbound_poll_backoff_max_ms = j.value("outbound_poll_backoff_max_ms", outbound_poll_backoff_max_ms);
        outbound_poll_backoff_shift_cap= j.value("outbound_poll_backoff_shift_cap", outbound_poll_backoff_shift_cap);

        inbound_ack_mode = inbound_ack_mode_from_string(
            j.value("inbound_ack_mode", std::string(inbound_ack_mode_to_string(inbound_ack_mode))));
        inbound_persist_wait_timeout_ms = j.value("inbound_persist_wait_timeout_ms", inbound_persist_wait_timeout_ms);
        inbound_mime_parse_limit_bytes = j.value("inbound_mime_parse_limit_bytes", inbound_mime_parse_limit_bytes);
        persist_max_inflight_mails      = j.value("persist_max_inflight_mails", persist_max_inflight_mails);
        persist_min_available_memory_mb = j.value("persist_min_available_memory_mb", persist_min_available_memory_mb);
        persist_min_db_available_connections = j.value("persist_min_db_available_connections", persist_min_db_available_connections);

        inbound_auth_policy = inbound_auth_policy_from_string(
            j.value("inbound_auth_policy", std::string(inbound_auth_policy_to_string(inbound_auth_policy))));
        inbound_spf_mode   = j.value("inbound_spf_mode", inbound_spf_mode);
        inbound_dkim_mode  = j.value("inbound_dkim_mode", inbound_dkim_mode);
        inbound_dmarc_mode = j.value("inbound_dmarc_mode", inbound_dmarc_mode);
        inbound_auth_timeout_ms = j.value("inbound_auth_timeout_ms", inbound_auth_timeout_ms);

        metrics_enabled    = j.value("metrics_enabled", metrics_enabled);
        metrics_port       = j.value("metrics_port", metrics_port);
        metrics_bind_address = j.value("metrics_bind_address", metrics_bind_address);

        intrusion_detection_enabled      = j.value("intrusion_detection_enabled", false);
        intrusion_persist_interval_sec   = j.value("intrusion_persist_interval_sec", 60);
        intrusion_persist_dirty_threshold= j.value("intrusion_persist_dirty_threshold", 256);
        intrusion_max_records            = j.value("intrusion_max_records", 10000);
        intrusion_ban_threshold          = j.value("intrusion_ban_threshold", 0);

        if (j.contains("outbound_ports") && j["outbound_ports"].is_array()) {
            outbound_ports.clear();
            for (auto& p : j["outbound_ports"])
                if (p.is_number_unsigned() && p.get<uint32_t>() <= 65535)
                    outbound_ports.push_back(static_cast<uint16_t>(p.get<uint32_t>()));
        }

        if (j.contains("distributed_storage_roots") && j["distributed_storage_roots"].is_array()) {
            distributed_storage_roots.clear();
            for (auto& item : j["distributed_storage_roots"]) {
                if (!item.is_string()) continue;
                auto p = resolve_path(filename, item.get<std::string>());
                if (!p.empty()) distributed_storage_roots.push_back(p);
            }
        }
        distributed_storage_replica_count = j.value("distributed_storage_replica_count", distributed_storage_replica_count);
        hdfs_endpoint   = j.value("hdfs_endpoint", hdfs_endpoint);
        hdfs_base_path  = j.value("hdfs_base_path", hdfs_base_path);
        hdfs_user       = j.value("hdfs_user", hdfs_user);
        hdfs_timeout_ms = j.value("hdfs_timeout_ms", hdfs_timeout_ms);
        hdfs_replication= j.value("hdfs_replication", hdfs_replication);

        s3_endpoint       = j.value("s3_endpoint", s3_endpoint);
        s3_bucket         = j.value("s3_bucket", s3_bucket);
        s3_access_key     = j.value("s3_access_key", s3_access_key);
        s3_secret_key     = j.value("s3_secret_key", s3_secret_key);
        s3_region         = j.value("s3_region", s3_region);
        s3_timeout_ms     = j.value("s3_timeout_ms", s3_timeout_ms);
        s3_use_path_style = j.value("s3_use_path_style", s3_use_path_style);

        if (perf_mode) apply_perf_mode();
        return true;
    }

    void apply_perf_mode() {
        maxConnections               = 100000;
        persist_max_inflight_mails   = 1000000;
        persist_min_available_memory_mb = 0;
        persist_min_db_available_connections = 0;
        dnsbl_enabled                = false;
        intrusion_detection_enabled  = false;
    }

    bool saveToFile(const std::string&) const { return true; }
};

} // namespace mail_system
#endif
