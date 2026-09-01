#ifndef WEB_SERVER_H2_SERVER_H
#define WEB_SERVER_H2_SERVER_H
// ──────────────────────────────────────────────────────────────────
// H2Server — HTTP/2 服务器装配（复用 TcpServerBase 工厂）
// 与 HttpServer 同构，只是 session 换成 H2Session（多流帧式）.
// ──────────────────────────────────────────────────────────────────
#include "framework/tcp_server_base.h"
#include "web_server/h2/h2_session.h"
#include <memory>
#include <string>

namespace web_server {
namespace h2 {

class H2Server : public mail_system::TcpServerBase<TcpH2Session, SslH2Session> {
public:
    H2Server(const mail_system::ServerConfig& config,
             std::shared_ptr<mail_system::ThreadPoolBase> ioThreadPool = nullptr,
             std::shared_ptr<mail_system::ThreadPoolBase> workerThreadPool = nullptr,
             std::shared_ptr<mail_system::DBPool> dbPool = nullptr);
    ~H2Server() override = default;

protected:
    std::shared_ptr<TcpH2Session> make_tcp_session(
        std::unique_ptr<mail_system::TcpConnection> conn,
        const mail_system::ListenerConfig& lc) override;
    std::shared_ptr<SslH2Session> make_ssl_session(
        std::unique_ptr<mail_system::SslConnection> conn,
        const mail_system::ListenerConfig& lc) override;
    bool should_reject_connection(std::string& reason,
        const std::string& client_ip = "") const override;
};

} // namespace h2
} // namespace web_server

#endif // WEB_SERVER_H2_SERVER_H