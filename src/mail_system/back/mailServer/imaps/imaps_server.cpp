#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>

namespace mail_system {

ImapsServer::ImapsServer(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> workerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : ServerBase(config, ioThreadPool, workerThreadPool, dbPool) {
    auto cfg = std::atomic_load(&m_config);

    // IMAP doesn't need PersistentQueue like SMTP (read-only retrieval)
    // but we still need storage provider for reading mail bodies
    // ServerBase already creates m_shardRouter->get_storage(0)

    // Create FSM instances for TCP and SSL connections
    m_tcp_fsm = std::make_shared<TraditionalImapsFsm<TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalImapsFsm<SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);

    // 创建并注入共享的 LRU 邮箱统计缓存（容量 20000，TTL 8 秒）
    auto stats_cache = std::make_shared<
        TraditionalImapsFsm<TcpConnection>::MailboxStatsCache>(20000, std::chrono::seconds(8));
    m_tcp_fsm->set_mailbox_stats_cache(stats_cache);
    m_ssl_fsm->set_mailbox_stats_cache(stats_cache);



    LOG_IMAP_INFO("IMAP server initialized, SSL fsm={}, TCP fsm={}",
                  m_ssl_fsm ? "ready" : "null",
                  m_tcp_fsm ? "ready" : "null");
}

ImapsServer::~ImapsServer() {
    stop();
}

void ImapsServer::handle_accept(
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
    const boost::system::error_code& error, ListenerConfig lc)
{
    using SslSession = ImapsSession<SslConnection>;
    auto ssl_connection = std::make_unique<SslConnection>(std::move(ssl_socket));
    auto session = std::make_shared<SslSession>(this, std::move(ssl_connection), m_ssl_fsm);
    (void)lc;
    if (!error) {
        try {
            LOG_NETWORK_INFO("New IMAPS connection from {}", session->get_client_ip());
            increment_connection_count();
            SslSession::start(session);
        } catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting IMAPS session: {}", e.what());
        }
    } else {
        LOG_NETWORK_ERROR("IMAPS accept error: {}", error.message());
    }
}

void ImapsServer::handle_tcp_accept(
    std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
    const boost::system::error_code& error, ListenerConfig lc)
{
    using TcpSession = ImapsSession<TcpConnection>;
    auto tcp_connection = std::make_unique<TcpConnection>(std::move(socket));
    auto session = std::make_shared<TcpSession>(this, std::move(tcp_connection), m_tcp_fsm);
    (void)lc;
    if (!error) {
        try {
            LOG_NETWORK_INFO("New IMAP connection from {}", session->get_client_ip());
            increment_connection_count();
            TcpSession::start(session);
        } catch (const std::exception& e) {
            LOG_NETWORK_ERROR("Error starting IMAP session: {}", e.what());
        }
    } else {
        LOG_NETWORK_ERROR("IMAP accept error: {}", error.message());
    }
}

void ImapsServer::handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket) {
    using SslSession = ImapsSession<SslConnection>;
    if (!socket) return;

    try {
        auto ssl_stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
            std::move(*socket), get_ssl_context());
        auto ssl_connection = std::make_unique<SslConnection>(std::move(ssl_stream));
        auto session = std::make_shared<SslSession>(this, std::move(ssl_connection), m_ssl_fsm);

        LOG_NETWORK_INFO("IMAP STARTTLS upgraded, continue on TLS from {}", session->get_client_ip());
        increment_connection_count();
        SslSession::start_after_starttls(session);
    } catch (const std::exception& e) {
        LOG_NETWORK_ERROR("Error handing off IMAP STARTTLS socket: {}", e.what());
    }
}

bool ImapsServer::should_reject_connection(std::string& reason, const std::string&) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace mail_system
