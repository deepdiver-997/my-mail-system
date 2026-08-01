#ifndef MAIL_SYSTEM_SERVER_BASE_H
#define MAIL_SYSTEM_SERVER_BASE_H

#include <atomic>
#include <memory>
#include <string>
#include <map>
#include "server_config.h"
#include "intrusion_detector.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "framework/net/dns_resolver.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/lru_cache.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/router/i_shard_router.h"
#include "framework/metrics_server.h"

namespace mail_system {

enum class ServerState { Stopped, Running, Pausing, Paused };

inline const char* server_state_to_string(ServerState s) {
    switch (s) {
    case ServerState::Running: return "Running";
    case ServerState::Pausing: return "Pausing";
    case ServerState::Paused:  return "Paused";
    default: return "Stopped";
    }
}

// ================================================================
// ServerBase — 传输层无关的服务器基类
//
//   只负责生命周期、配置、metrics、共享组件。
//   不包含任何 TCP/SSL/accept 逻辑（由 TcpServerBase 等子类提供）。
// ================================================================
class ServerBase {
public:
    ServerBase(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
    virtual ~ServerBase();

    ServerBase(const ServerBase&) = delete;
    ServerBase& operator=(const ServerBase&) = delete;
    ServerBase(ServerBase&&) = delete;
    ServerBase& operator=(ServerBase&&) = delete;

    // 非阻塞启动（子类实现具体的监听/连接逻辑）
    virtual void start() = 0;

    // 阻塞当前线程直到收到停止信号
    void run();

    ServerState get_state() const;
    void request_stop() { m_state.store(ServerState::Pausing, std::memory_order_release); }

    // reload 运行时配置
    bool reload_config(const std::string& json_file);

    // ── 共享成员（所有协议共用） ──────────────────────────────
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<pr::IDnsResolver> m_dnsResolver;

    void set_mailbox_cache(std::shared_ptr<IMailboxCache> cache) { m_mailboxCache = cache; }
    std::shared_ptr<IMailboxCache> get_mailbox_cache() const { return m_mailboxCache; }
    std::shared_ptr<router::IShardRouter> get_shard_router() const { return m_shardRouter; }
    std::shared_ptr<pr::IDnsResolver> get_dns_resolver() const { return m_dnsResolver; }

    std::shared_ptr<IMailboxCache> m_mailboxCache;
    std::string m_domain;
    std::string m_configFilePath;
    std::shared_ptr<ServerConfig> m_config;

    // ── 连接计数 ──────────────────────────────────────────────
    std::atomic<size_t> active_connections_{0};
    std::atomic<size_t> connections_total_{0};
    std::atomic<size_t> connections_rejected_total_{0};
    std::atomic<size_t> mails_accepted_total_{0};
    void increment_connection_count();
    void decrement_connection_count();
    void increment_connections_total();
    void increment_connections_rejected();
    void increment_mails_accepted();

    // ── 入侵检测 ──────────────────────────────────────────────
    IntrusionDetector m_intrusionDetector;
    void record_session_end(const std::string& ip, bool authenticated) {
        m_intrusionDetector.record_session(ip, authenticated);
    }
    bool is_ip_banned(const std::string& ip) const {
        auto cfg = std::atomic_load(&m_config);
        if (!cfg->intrusion_detection_enabled || cfg->intrusion_ban_threshold <= 0) return false;
        return m_intrusionDetector.is_banned(ip);
    }

    // ── Metrics ───────────────────────────────────────────────
    std::weak_ptr<MetricsServer> get_metrics() const { return m_metricsServer; }
    void refresh_metrics();
    void push_metric_gauge(const std::string& name, const MetricsServer::LabelMap& labels, double v);
    void push_metric_counter(const std::string& name, const MetricsServer::LabelMap& labels, uint64_t v);
    void push_metric_observe(const std::string& name, const MetricsServer::LabelMap& labels, double v);

    [[deprecated]] std::string build_metrics_response() const;
    [[deprecated]] std::string build_status_response() const;

protected:
    virtual void stop(ServerState state = ServerState::Pausing);
    virtual bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const = 0;

    void start_metrics_server();
    void stop_metrics_server();
    std::shared_ptr<MetricsServer> m_metricsServer;

    std::atomic<ServerState> m_state{ServerState::Stopped};
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SERVER_BASE_H
