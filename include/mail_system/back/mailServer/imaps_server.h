#ifndef IMAPS_SERVER_H
#define IMAPS_SERVER_H

#include "mail_system/back/mailServer/tcp_server_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imaps_fsm.hpp"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/session/imaps_session.h"
#include "mail_system/back/entities/mail.h"

namespace mail_system {

class ImapsServer : public TcpServerBase<ImapsSession<TcpConnection>,
                                          ImapsSession<SslConnection>> {
public:
    ImapsServer(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
    virtual ~ImapsServer() override;

    std::shared_ptr<TraditionalImapsFsm<TcpConnection>> get_tcp_fsm() const { return m_tcp_fsm; }
    std::shared_ptr<TraditionalImapsFsm<SslConnection>> get_ssl_fsm() const { return m_ssl_fsm; }

protected:
    bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const override;

    std::shared_ptr<ImapsSession<TcpConnection>> make_tcp_session(
        std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc) override;
    std::shared_ptr<ImapsSession<SslConnection>> make_ssl_session(
        std::unique_ptr<SslConnection> conn, const ListenerConfig& lc) override;

private:
    std::shared_ptr<TraditionalImapsFsm<TcpConnection>> m_tcp_fsm;
    std::shared_ptr<TraditionalImapsFsm<SslConnection>> m_ssl_fsm;
};

} // namespace mail_system

#endif // IMAPS_SERVER_H
