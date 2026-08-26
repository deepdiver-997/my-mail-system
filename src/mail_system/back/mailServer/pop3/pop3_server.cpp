#include "mail_system/back/mailServer/pop3_server.h"
#include "framework/connection/ssl_connection.h"
#include "framework/connection/tcp_connection.h"
#include "mail_system/back/common/logger.h"
#include <memory>

namespace mail_system {

Pop3Server::Pop3Server(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> workerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : TcpServerBase(config, ioThreadPool, workerThreadPool, dbPool) {
    m_tcp_fsm = std::make_shared<TraditionalPop3Fsm<TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalPop3Fsm<SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    LOG_SERVER_INFO("POP3 server initialized, SSL fsm={}, TCP fsm={}",
                  m_ssl_fsm ? "ready" : "null",
                  m_tcp_fsm ? "ready" : "null");
}

Pop3Server::~Pop3Server() {
    stop();
}

std::shared_ptr<Pop3Session<TcpConnection>> Pop3Server::make_tcp_session(
    std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<Pop3Session<TcpConnection>>(
        this, std::move(conn), m_tcp_fsm);
}

std::shared_ptr<Pop3Session<SslConnection>> Pop3Server::make_ssl_session(
    std::unique_ptr<SslConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<Pop3Session<SslConnection>>(
        this, std::move(conn), m_ssl_fsm);
}

bool Pop3Server::should_reject_connection(std::string& reason, const std::string&) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace mail_system
