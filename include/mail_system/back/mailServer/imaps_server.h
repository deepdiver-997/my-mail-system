#ifndef IMAPS_SERVER_H
#define IMAPS_SERVER_H

#include "mail_system/back/mailServer/server_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imaps_fsm.hpp"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/session/imaps_session.h"
#include "mail_system/back/entities/mail.h"

namespace mail_system {

class ImapsServer : public ServerBase {
public:
    ImapsServer(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
    virtual ~ImapsServer() override;

    std::shared_ptr<TraditionalImapsFsm<TcpConnection>> get_tcp_fsm() const { return m_tcp_fsm; }
    std::shared_ptr<TraditionalImapsFsm<SslConnection>> get_ssl_fsm() const { return m_ssl_fsm; }

protected:
    // 处理新连接
    void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&& ssl_socket,
                       const boost::system::error_code& error, ListenerConfig lc) override;
    void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
                           const boost::system::error_code& error, ListenerConfig lc) override;

    // 连接负载门控
    bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const override;

    // STARTTLS: 从 TCP 升级到 TLS，不重启协议
    void handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket) override;

    std::shared_ptr<TraditionalImapsFsm<TcpConnection>> m_tcp_fsm;
    std::shared_ptr<TraditionalImapsFsm<SslConnection>> m_ssl_fsm;
};

} // namespace mail_system

#endif // IMAPS_SERVER_H
