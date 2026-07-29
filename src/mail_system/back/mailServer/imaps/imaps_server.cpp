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
        : TcpServerBase(config, ioThreadPool, workerThreadPool, dbPool) {
    auto cfg = std::atomic_load(&m_config);

    m_tcp_fsm = std::make_shared<TraditionalImapsFsm<TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalImapsFsm<SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);

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

std::shared_ptr<ImapsSession<TcpConnection>> ImapsServer::make_tcp_session(
    std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<ImapsSession<TcpConnection>>(
        this, std::move(conn), m_tcp_fsm);
}

std::shared_ptr<ImapsSession<SslConnection>> ImapsServer::make_ssl_session(
    std::unique_ptr<SslConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<ImapsSession<SslConnection>>(
        this, std::move(conn), m_ssl_fsm);
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
