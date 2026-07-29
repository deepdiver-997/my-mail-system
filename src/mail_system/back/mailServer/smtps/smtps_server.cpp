#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/outbound/cares_dns_resolver.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>
#include <netdb.h>        // getaddrinfo / freeaddrinfo (DNSBL 查询)
#include <arpa/inet.h>    // inet_ntop

namespace mail_system {

SmtpsServer::SmtpsServer(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : ServerBase(config, ioThreadPool, wokerThreadPool, dbPool) {
    auto cfg = std::atomic_load(&m_config);

    // SMTP 专用：创建持久化队列和出站投递客户端
    // 这些是 SMTP 私有的，IMAP 不需要
    if (m_shardRouter && cfg->use_database) {
        if (!m_persistentQueue) {
            m_persistentQueue = std::make_shared<persist_storage::PersistentQueue>(
                m_shardRouter,
                m_workerThreadPool);
            m_persistentQueue->set_local_domain(m_domain);
            persist_storage::PersistentQueuePressureConfig pressure_config;
            pressure_config.max_inflight_mails = cfg->persist_max_inflight_mails;
            pressure_config.min_available_memory_mb = cfg->persist_min_available_memory_mb;
            pressure_config.min_db_available_connections = cfg->persist_min_db_available_connections;
            m_persistentQueue->set_pressure_config(pressure_config);
            m_persistentQueue->inject_metrics(get_metrics());
            LOG_SERVER_INFO("PersistentQueue created for SMTP server");
        }

        if (!m_outboundInterruptFlag) {
            m_outboundInterruptFlag = std::make_shared<std::atomic<bool>>(true);
        }

        if (!m_outboundClient) {
            outbound::OutboundConfig oc;
            oc.helo_domain           = cfg->outbound_helo_domain;
            oc.mail_from_domain      = cfg->outbound_mail_from_domain;
            oc.rewrite_header_from   = cfg->outbound_rewrite_header_from;
            oc.dkim_enabled          = cfg->outbound_dkim_enabled;
            oc.dkim_selector         = cfg->outbound_dkim_selector;
            oc.dkim_domain           = cfg->outbound_dkim_domain;
            oc.dkim_private_key_file = cfg->outbound_dkim_private_key_file;
            oc.ports                 = cfg->outbound_ports;
            oc.max_attempts          = cfg->outbound_max_attempts;
            oc.busy_sleep_ms         = static_cast<int>(cfg->outbound_poll_busy_sleep_ms);
            oc.backoff_base_ms       = static_cast<int>(cfg->outbound_poll_backoff_base_ms);
            oc.backoff_max_ms        = static_cast<int>(cfg->outbound_poll_backoff_max_ms);
            oc.backoff_shift_cap     = cfg->outbound_poll_backoff_shift_cap;

            m_outboundClient = std::make_shared<outbound::SmtpOutboundClient>(
                m_shardRouter,
                m_ioThreadPool,
                m_workerThreadPool,
                std::make_shared<outbound::CaresDnsResolver>(),
                m_outboundInterruptFlag,
                std::move(oc),
                m_domain
            );
            m_persistentQueue->set_outbound_client(m_outboundClient);
            m_outboundClient->inject_metrics(get_metrics());
            m_outboundClient->start();
            LOG_SERVER_INFO("Outbound client created and started for SMTP server");
        }
    } else {
        LOG_SERVER_WARN("No database pool — SMTP outbound delivery disabled");
    }

    m_tcp_fsm = std::make_shared<TraditionalSmtpsFsm<TcpConnection>>(m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalSmtpsFsm<SslConnection>>(m_ioThreadPool, m_workerThreadPool, m_persistentQueue, m_shardRouter);
}

SmtpsServer::~SmtpsServer() {
    stop();
}

void SmtpsServer::stop(ServerState state) {
    // 先停 SMTP 专有组件（必须在停线程池之前）
    if (m_outboundClient) { m_outboundClient->stop(); LOG_SERVER_INFO("Outbound client stopped"); }
    if (m_persistentQueue) {
        m_persistentQueue->shutdown();
        m_persistentQueue.reset();
        LOG_SERVER_INFO("PersistentQueue shutdown");
    }
    // 再调用基类的停止逻辑（线程池等）
    ServerBase::stop(state);
}

void SmtpsServer::handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
                                 const boost::system::error_code& error, ListenerConfig lc) {
    using SslSession = SmtpsSession<mail_system::SslConnection>;
    auto ssl_connection = std::make_unique<SslConnection>(std::move(ssl_socket));
    auto session = std::make_shared<SslSession>(this, std::move(ssl_connection), m_ssl_fsm);
    if (!error) {
        try {
            session->set_listener_config(lc);
            LOG_NETWORK_INFO("New SMTPS connection from {}", session->get_client_ip());
            increment_connection_count();
            SslSession::start(session);
        } catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting SMTPS session: {}", e.what());
        }
    } else {
        LOG_NETWORK_ERROR("Accept error: {}", error.message());
    }
}

void SmtpsServer::handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
                                    const boost::system::error_code& error, ListenerConfig lc) {
    using TcpSession = SmtpsSession<mail_system::TcpConnection>;
    auto tcp_connection = std::make_unique<TcpConnection>(std::move(socket));
    auto session = std::make_shared<TcpSession>(this, std::move(tcp_connection), m_tcp_fsm);
    if (!error) {
        try {
            session->set_listener_config(lc);
            LOG_NETWORK_INFO("New SMTP connection from {}", session->get_client_ip());
            increment_connection_count();
            TcpSession::start(session);
        } catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting SMTP session: {}", e.what());
        }
    } else {
        LOG_NETWORK_ERROR("Accept error: {}", error.message());
    }
}

void SmtpsServer::handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket) {
    using SslSession = SmtpsSession<mail_system::SslConnection>;
    if (!socket) return;

    try {
        auto ssl_stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
            std::move(*socket), get_ssl_context());
        auto ssl_connection = std::make_unique<SslConnection>(std::move(ssl_stream));
        auto session = std::make_shared<SslSession>(this, std::move(ssl_connection), m_ssl_fsm);

        LOG_NETWORK_INFO("STARTTLS upgraded, continue SMTP on TLS from {}", session->get_client_ip());
        increment_connection_count();
        SslSession::start_after_starttls(session);
    } catch (const std::exception& e) {
        LOG_NETWORK_ERROR("Error handing off STARTTLS socket: {}", e.what());
    }
}

bool SmtpsServer::should_reject_connection(std::string& reason, const std::string& client_ip) const {
    auto cfg = std::atomic_load(&m_config);

    // --- DNSBL 反垃圾检查 (Spamhaus Zen) ---
    // 将客户端 IP 反转后查询 zen.spamhaus.org，
    // 如果返回 127.0.0.x 说明该 IP 在已知垃圾源列表中。
    // 格式: <reversed_octets>.zen.spamhaus.org
    // 例如: 连接到 1.2.3.4 → 查询 4.3.2.1.zen.spamhaus.org
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
            // IPv6 暂不支持 DNSBL（需要 ip6.arpa 格式）
        } catch (...) {
            // 解析失败，跳过 DNSBL 检查
        }

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

                // Spamhaus 返回码:
                //   127.0.0.2  - SBL (已知垃圾源)
                //   127.0.0.3  - CSS (雪鞋垃圾邮件)
                //   127.0.0.4-7 - XBL (漏洞利用/僵尸网络)
                //   127.0.0.10-14 - PBL (策略阻止列表，通常是动态 IP)
                if (resolved.compare(0, 7, "127.0.0") == 0) {
                    reason = "DNSBL listed (" + resolved + "): " + query;
                    return true;
                }
            }
        }
    }
    // --- DNSBL 检查结束 ---

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
