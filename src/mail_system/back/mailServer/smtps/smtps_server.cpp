#include "mail_system/back/mailServer/smtps_server.h"
#include "framework/connection/ssl_connection.h"
#include "framework/connection/tcp_connection.h"
#include "mail_system/back/mailServer/outbound/cares_dns_resolver.h"
#include "mail_system/back/mailServer/outbound/outbound_config.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>
#include <netdb.h>
#include <arpa/inet.h>

namespace mail_system {

SmtpsServer::SmtpsServer(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : TcpServerBase(config, ioThreadPool, wokerThreadPool, dbPool) {
    auto cfg = std::atomic_load(&m_config);

    // DNS resolver (共享基础资源，入站验证+出站投递+DNSBL共用)
    m_dnsResolver = std::make_shared<outbound::CaresDnsResolver>();

    // SMTP 专用：持久化队列和出站投递
    if (m_shardRouter && cfg->use_database) {
        if (!m_persistentQueue) {
            m_persistentQueue = std::make_shared<persist_storage::PersistentQueue>(
                m_shardRouter, m_workerThreadPool);
            m_persistentQueue->set_local_domain(m_domain);
            persist_storage::PersistentQueuePressureConfig pressure_config;
            pressure_config.max_inflight_mails = cfg->persist_max_inflight_mails;
            pressure_config.min_available_memory_mb = cfg->persist_min_available_memory_mb;
            pressure_config.min_db_available_connections = cfg->persist_min_db_available_connections;
            m_persistentQueue->set_pressure_config(pressure_config);
            m_persistentQueue->inject_metrics(get_metrics());
            LOG_SERVER_INFO("PersistentQueue created for SMTP server");
        }

        // 新 outbound 引擎：DB 路径下默认启用，PQ 在 has_external_recipient
        // 命中时会调 submit() 触发投递。
        if (!m_outboundServer) {
            m_outboundServer = std::make_shared<outbound::OutboundServer>(this);
            // 把 server_config 平铺字段组装成 OutboundConfig（含 static_routes）
            outbound::OutboundConfig ob_cfg;
            ob_cfg.helo_domain = cfg->outbound_helo_domain;
            ob_cfg.mail_from_domain = cfg->outbound_mail_from_domain;
            ob_cfg.rewrite_header_from = cfg->outbound_rewrite_header_from;
            ob_cfg.dkim_enabled = cfg->outbound_dkim_enabled;
            ob_cfg.dkim_selector = cfg->outbound_dkim_selector;
            ob_cfg.dkim_domain = cfg->outbound_dkim_domain;
            ob_cfg.dkim_private_key_file = cfg->outbound_dkim_private_key_file;
            ob_cfg.ports = cfg->outbound_ports;
            ob_cfg.max_attempts = cfg->outbound_max_attempts;
            ob_cfg.static_routes = std::move(cfg->outbound_static_routes);
            m_outboundServer->set_config(std::move(ob_cfg));
        }
        m_persistentQueue->set_outbound_server(m_outboundServer);
        LOG_SERVER_INFO("OutboundServer (new engine) wired into PersistentQueue");
    } else {
        LOG_SERVER_WARN("No database pool — SMTP outbound delivery disabled");
    }

    m_tcp_fsm = std::make_shared<TraditionalSmtpsFsm<TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalSmtpsFsm<SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_shardRouter);

    // DB 路径下 m_outboundServer 已被 wire 到 PersistentQueue；启动它开始拉 outbox
    if (m_outboundServer) {
        m_outboundServer->start();
        LOG_SERVER_INFO("OutboundServer (new engine) started");
    }
}

SmtpsServer::~SmtpsServer() {
    stop();
}

void SmtpsServer::stop(ServerState state) {
    if (m_outboundServer) { m_outboundServer->stop(); LOG_SERVER_INFO("OutboundServer stopped"); }
    if (m_persistentQueue) {
        m_persistentQueue->shutdown();
        m_persistentQueue.reset();
        LOG_SERVER_INFO("PersistentQueue shutdown");
    }
    TcpServerBase::stop(state);
}

std::shared_ptr<SmtpsSession<TcpConnection>> SmtpsServer::make_tcp_session(
    std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc)
{
    auto session = std::make_shared<SmtpsSession<TcpConnection>>(
        this, std::move(conn), m_tcp_fsm);
    session->set_listener_config(lc);
    return session;
}

std::shared_ptr<SmtpsSession<SslConnection>> SmtpsServer::make_ssl_session(
    std::unique_ptr<SslConnection> conn, const ListenerConfig& lc)
{
    auto session = std::make_shared<SmtpsSession<SslConnection>>(
        this, std::move(conn), m_ssl_fsm);
    session->set_listener_config(lc);
    return session;
}

bool SmtpsServer::should_reject_connection(std::string& reason, const std::string& client_ip) const {
    auto cfg = std::atomic_load(&m_config);

    // DNSBL 反垃圾检查
    if (!cfg->perf_mode && cfg->dnsbl_enabled && !client_ip.empty()) {
        std::string reversed;
        try {
            auto addr = boost::asio::ip::make_address(client_ip);
            if (addr.is_v4()) {
                auto bytes = addr.to_v4().to_bytes();
                reversed = std::to_string(bytes[3]) + "." +
                           std::to_string(bytes[2]) + "." +
                           std::to_string(bytes[1]) + "." +
                           std::to_string(bytes[0]);
            }
        } catch (...) {}

        if (!reversed.empty()) {
            std::string query = reversed + ".zen.spamhaus.org";
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo* result = nullptr;

            if (getaddrinfo(query.c_str(), nullptr, &hints, &result) == 0 && result) {
                char ip_str[INET_ADDRSTRLEN] = {};
                auto* sin = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
                inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                std::string resolved = ip_str;
                freeaddrinfo(result);

                if (resolved.compare(0, 7, "127.0.0") == 0) {
                    reason = "DNSBL listed (" + resolved + "): " + query;
                    return true;
                }
            }
        }
    }

    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }

    if (m_persistentQueue &&
        cfg->persist_max_inflight_mails > 0 &&
        m_persistentQueue->inflight_count() >= cfg->persist_max_inflight_mails) {
        reason = "persist inflight limit reached";
        return true;
    }

    return false;
}

} // namespace mail_system
