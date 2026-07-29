#ifndef MAIL_SYSTEM_SERVER_BASE_H
#define MAIL_SYSTEM_SERVER_BASE_H

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "server_config.h"
#include "intrusion_detector.h"

#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/thread_pool/io_thread_pool.h"
#include "mail_system/back/thread_pool/boost_thread_pool.h"

#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/lru_cache.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/router/i_shard_router.h"
#include "mail_system/back/mailServer/metrics_server.h"

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

// ====================================================================
// 服务器基类 — 生命周期管理
//
// 典型用法:
//   auto server = std::make_shared<SmtpsServer>(config);
//   server->start();   // 非阻塞，启动 listener/outbound/metrics
//   server->run();     // 阻塞当前线程直到收到停止信号
//   // shared_ptr 析构 → ~ServerBase() → stop() → RAII 清理资源
//
// 信号处理:
//   signal_handler → server->request_stop() (async-signal-safe)
//   → run() 检测到 Pausing 状态 → 主线程退出
//   → shared_ptr 析构 → stop() 完成资源释放
//
// 状态转换:
//   Stopped → start() → Running → request_stop() → Pausing
//   → stop() → 子系统和线程停止 → state 保持 Pausing
//   ~ServerBase() 后不再访问 state
// ====================================================================
class ServerBase {
class SessionBase;
public:
    ServerBase(const ServerConfig& config,
         std::shared_ptr<mail_system::ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<mail_system::ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
    virtual ~ServerBase();

    ServerBase(const ServerBase&) = delete;
    ServerBase& operator=(const ServerBase&) = delete;
    ServerBase(ServerBase&&) = delete;
    ServerBase& operator=(ServerBase&&) = delete;

    // 非阻塞启动：启动 listener 线程、metrics server、outbound client。
    // 调用后应立即调用 run() 阻塞主线程，或自行循环检查 get_state()。
    void start();

    // 阻塞当前线程直到服务器进入 Pausing 状态（由 request_stop() 触发）。
    // 返回后由外部 shared_ptr 析构完成资源清理。
    void run();

    // 线程安全，可在任意线程调用。
    ServerState get_state() const;

    // async-signal-safe: 信号处理器中唯一可调用的方法。
    // 仅设置原子状态为 Pausing，不执行任何资源操作。
    // run() 检测到此状态后退出，destructor 中的 stop() 执行清理。
    void request_stop() { m_state.store(ServerState::Pausing, std::memory_order_release); }

    // STARTTLS 升级：将 TCP socket 包装为 SSL stream，分派给 handle_accept。
    void pass_stream(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
                     const ListenerConfig& lc) {
        this->handle_accept(std::move(ssl_socket), boost::system::error_code(), lc);
    }

    // STARTTLS: 从已建立的 TCP 连接升级到 TLS。
    // 派生类可覆写以直接创建 SSL session（跳过 greeting/重启协议）。
    virtual void handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket);

    const ListenerConfig* get_listener_config(uint16_t port) const;

protected:
    // 停止服务器：释放 work guard、关闭 acceptor、停止 io_context、
    // 等待 listener 线程退出、停止 outbound/queue/thread pools。
    // 仅从 ~ServerBase() 调用，外部应使用 request_stop() + 析构。
    virtual void stop(ServerState state = ServerState::Pausing);

    // 启动所有 listener 的异步 accept 循环。
    virtual void start_all_tcp_acceptors();
    virtual void start_all_ssl_acceptors();

    // 单个 acceptor 的 accept 循环（被 start_all_* 调用）。
    void do_tcp_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc);
    void do_ssl_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc);

    // 处理 TLS acceptor 来的新连接
    virtual void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
                               const boost::system::error_code& error, ListenerConfig lc) = 0;
    // 处理 TCP acceptor 来的新连接
    virtual void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
                                   const boost::system::error_code& error, ListenerConfig lc) = 0;

public:
    std::shared_ptr<boost::asio::io_context> get_io_context();
    boost::asio::ssl::context& get_ssl_context();

public:
    std::shared_ptr<mail_system::ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<mail_system::ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;

    void set_mailbox_cache(std::shared_ptr<IMailboxCache> cache) { m_mailboxCache = cache; }
    std::shared_ptr<IMailboxCache> get_mailbox_cache() const { return m_mailboxCache; }
    std::shared_ptr<router::IShardRouter> get_shard_router() const { return m_shardRouter; }

    bool ssl_in_worker;
    std::shared_ptr<IMailboxCache> m_mailboxCache;
    std::string m_domain;
    std::string m_configFilePath;
    std::shared_ptr<ServerConfig> m_config;

    bool reload_config(const std::string& json_file);

    IntrusionDetector m_intrusionDetector;
    void record_session_end(const std::string& ip, bool authenticated) {
        m_intrusionDetector.record_session(ip, authenticated);
    }

    // IP 封禁检查（在 accept 层调用）
    bool is_ip_banned(const std::string& ip) const {
        auto cfg = std::atomic_load(&m_config);
        if (!cfg->intrusion_detection_enabled || cfg->intrusion_ban_threshold <= 0) return false;
        return m_intrusionDetector.is_banned(ip);
    }

    std::atomic<size_t> active_connections_{0};
    void increment_connection_count();
    void decrement_connection_count();

    std::atomic<size_t> connections_total_{0};
    std::atomic<size_t> connections_rejected_total_{0};
    std::atomic<size_t> mails_accepted_total_{0};
    void increment_connections_total();
    void increment_connections_rejected();
    void increment_mails_accepted();

    // Metrics push helpers + inject
    std::weak_ptr<MetricsServer> get_metrics() const { return m_metricsServer; }
    void refresh_metrics();
    void push_metric_gauge(const std::string& name, const MetricsServer::LabelMap& labels, double v);
    void push_metric_counter(const std::string& name, const MetricsServer::LabelMap& labels, uint64_t v);
    void push_metric_observe(const std::string& name, const MetricsServer::LabelMap& labels, double v);

    // (build_metrics_response / build_status_response replaced by MetricsServer storage + refresh_metrics)
    [[deprecated]] std::string build_metrics_response() const {
        std::string out;
        out.reserve(2048);
        auto add_gauge = [&](const char* name, const char* labels, const char* help, size_t val) {
            out.append("# HELP ").append(name).append(" ").append(help).append("\n");
            out.append("# TYPE ").append(name).append(" gauge\n");
            out.append(name);
            if (labels) out.append(labels);
            out.append(" ").append(std::to_string(val)).append("\n");
        };
        auto add_counter = [&](const char* name, const char* labels, const char* help, size_t val) {
            out.append("# HELP ").append(name).append(" ").append(help).append("\n");
            out.append("# TYPE ").append(name).append(" counter\n");
            out.append(name);
            if (labels) out.append(labels);
            out.append(" ").append(std::to_string(val)).append("\n");
        };

        // ---- 连接/邮件计数器 ----
        add_gauge("protorelay_active_connections", nullptr,
                  "Current active connections",
                  active_connections_.load(std::memory_order_relaxed));
        add_counter("protorelay_connections_total", nullptr,
                    "Total connections accepted",
                    connections_total_.load(std::memory_order_relaxed));
        add_counter("protorelay_connections_rejected_total", nullptr,
                    "Total connections rejected",
                    connections_rejected_total_.load(std::memory_order_relaxed));
        add_counter("protorelay_mails_accepted_total", nullptr,
                    "Total mails accepted",
                    mails_accepted_total_.load(std::memory_order_relaxed));

        // ---- 监听器 ----
        for (auto& [port, lc] : m_listener_configs) {
            std::string labels = "{port=\"" + std::to_string(port)
                               + "\",type=\"" + listener_type_to_string(lc.type) + "\"}";
            add_gauge("protorelay_listener_info", labels.c_str(),
                      "Listener active", 1);
        }

        // ---- Shard Router ----
        if (m_shardRouter) {
            size_t n = m_shardRouter->shard_count();
            std::string info_labels = "{router=\"" + std::string(m_shardRouter->name())
                                    + "\",shard_count=\"" + std::to_string(n) + "\"}";
            add_gauge("protorelay_router_info", info_labels.c_str(),
                      "Router metadata", 1);

            for (size_t i = 0; i < n; ++i) {
                char buf[32];
                snprintf(buf, sizeof(buf), "{shard=\"%zu\"}", i);
                auto db = m_shardRouter->get_db_pool(i);
                if (db) {
                    add_gauge("protorelay_db_pool_size", buf,
                              "DB pool current size", db->get_pool_size());
                    add_gauge("protorelay_db_available", buf,
                              "DB idle connections", db->get_available_connections());
                    add_gauge("protorelay_db_active", buf,
                              "DB active connections", db->get_active_connections());
                    add_gauge("protorelay_db_pool_max", buf,
                              "DB pool max size", db->get_max_pool_size());
                }
                auto st = m_shardRouter->get_storage(i);
                if (st) {
                    add_gauge("protorelay_storage_ready", buf,
                              "Storage provider ready", 1);
                }
            }
        }
        return out;
    }

    // JSON 格式状态摘要，供控制面板 /status 端点使用
    std::string build_status_response() const {
        std::string out;
        out.reserve(1024);
        out.append("{");

        // 基础信息
        out.append("\"state\":\"").append(server_state_to_string(m_state.load())).append("\",");
        auto cfg = std::atomic_load(&m_config);
        out.append("\"version\":\"").append(cfg->system_name).append("\",");
        out.append("\"domain\":\"").append(cfg->system_domain).append("\",");

        // 连接统计
        out.append("\"connections\":{");
        out.append("\"active\":").append(std::to_string(active_connections_.load()));
        out.append(",\"total\":").append(std::to_string(connections_total_.load()));
        out.append(",\"rejected\":").append(std::to_string(connections_rejected_total_.load()));
        out.append("},");

        out.append("\"mails_accepted\":").append(std::to_string(mails_accepted_total_.load())).append(",");

        // Router
        if (m_shardRouter) {
            out.append("\"router\":{");
            out.append("\"type\":\"").append(m_shardRouter->name()).append("\",");
            out.append("\"shard_count\":").append(std::to_string(m_shardRouter->shard_count()));
            out.append("},");

            // 每个 shard 详情
            out.append("\"shards\":[");
            size_t n = m_shardRouter->shard_count();
            for (size_t i = 0; i < n; ++i) {
                if (i > 0) out.append(",");
                out.append("{");
                out.append("\"index\":").append(std::to_string(i)).append(",");

                auto db = m_shardRouter->get_db_pool(i);
                out.append("\"db\":");
                if (db) {
                    out.append("{\"pool_size\":").append(std::to_string(db->get_pool_size()));
                    out.append(",\"available\":").append(std::to_string(db->get_available_connections()));
                    out.append(",\"active\":").append(std::to_string(db->get_active_connections()));
                    out.append(",\"max\":").append(std::to_string(db->get_max_pool_size()));
                    out.append("}");
                } else {
                    out.append("null");
                }
                out.append(",");

                auto st = m_shardRouter->get_storage(i);
                out.append("\"storage_ready\":").append(st ? "true" : "false");

                out.append("}");
            }
            out.append("],");
        }

        // 监听器
        out.append("\"listeners\":[");
        bool first = true;
        for (auto& [port, lc] : m_listener_configs) {
            if (!first) out.append(",");
            first = false;
            out.append("{\"port\":").append(std::to_string(port));
            out.append(",\"type\":\"").append(listener_type_to_string(lc.type)).append("\"");
            out.append(",\"auth\":\"").append(inbound_auth_policy_to_string(lc.auth_policy)).append("\"");
            out.append("}");
        }
        out.append("],");

        out.append("}");
        return out;
    }

protected:
    void load_certificates(const std::string& cert_file, const std::string& key_file, const std::string& dh_file = "");

    std::shared_ptr<boost::asio::io_context> m_ioContext;
    boost::asio::ssl::context m_sslContext;

    // 多监听器
    std::vector<std::shared_ptr<boost::asio::ip::tcp::acceptor>> m_tcp_acceptors;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::acceptor>> m_ssl_acceptors;
    std::unordered_map<uint16_t, ListenerConfig> m_listener_configs;

    std::shared_ptr<boost::asio::ip::tcp::resolver> m_resolver;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_workGuard;

    virtual bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const = 0;

    void start_metrics_server();
    void stop_metrics_server();
    std::shared_ptr<MetricsServer> m_metricsServer;

    std::atomic<bool> has_listener_thread;
    std::thread m_listenerThread;
    std::atomic<ServerState> m_state;
    std::map<std::string, std::string> m_known_domains;
};

} // namespace mail_system
#endif
