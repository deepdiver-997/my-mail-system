#include "web_server/h2/h2_server.h"
#include "mail_system/back/common/logger.h"
#include <memory>

namespace web_server {
namespace h2 {

H2Server::H2Server(const mail_system::ServerConfig& config,
                   std::shared_ptr<mail_system::ThreadPoolBase> ioThreadPool,
                   std::shared_ptr<mail_system::ThreadPoolBase> workerThreadPool,
                   std::shared_ptr<mail_system::DBPool> dbPool)
    : mail_system::TcpServerBase<TcpH2Session, SslH2Session>(
          config, ioThreadPool, workerThreadPool, dbPool)
{
    LOG_SERVER_INFO("H2Server (HTTP/2 multi-stream）ready");
}

std::shared_ptr<TcpH2Session> H2Server::make_tcp_session(
    std::unique_ptr<mail_system::TcpConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    return std::make_shared<TcpH2Session>(this, std::move(conn));
}

std::shared_ptr<SslH2Session> H2Server::make_ssl_session(
    std::unique_ptr<mail_system::SslConnection> conn,
    const mail_system::ListenerConfig& /*lc*/)
{
    return std::make_shared<SslH2Session>(this, std::move(conn));
}

bool H2Server::should_reject_connection(std::string& reason,
                                        const std::string& /*client_ip*/) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg && cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace h2
} // namespace web_server