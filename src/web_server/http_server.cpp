#include "web_server/http_server.h"
#include "mail_system/back/common/logger.h"
#include <memory>

namespace web_server {

HttpServer::HttpServer(const mail_system::ServerConfig& config,
                       std::string doc_root,
                       std::shared_ptr<mail_system::ThreadPoolBase> ioThreadPool,
                       std::shared_ptr<mail_system::ThreadPoolBase> workerThreadPool,
                       std::shared_ptr<mail_system::DBPool> dbPool)
    : mail_system::TcpServerBase<TcpHttpSession, SslHttpSession>(
          config, ioThreadPool, workerThreadPool, dbPool)
    , doc_root_(std::move(doc_root))
{
    m_tcp_fsm = std::make_shared<HttpFsm<mail_system::TcpConnection>>();
    m_ssl_fsm = std::make_shared<HttpFsm<mail_system::SslConnection>>();
    LOG_SERVER_INFO("HttpServer ready, doc_root={}", doc_root_);
}

std::shared_ptr<TcpHttpSession> HttpServer::make_tcp_session(
    std::unique_ptr<mail_system::TcpConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    auto session = std::make_shared<TcpHttpSession>(this, std::move(conn), m_tcp_fsm);
    session->set_doc_root(doc_root_);
    return session;
}

std::shared_ptr<SslHttpSession> HttpServer::make_ssl_session(
    std::unique_ptr<mail_system::SslConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    auto session = std::make_shared<SslHttpSession>(this, std::move(conn), m_ssl_fsm);
    session->set_doc_root(doc_root_);
    return session;
}

bool HttpServer::should_reject_connection(std::string& reason,
                                          const std::string& /*client_ip*/) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg && cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace web_server