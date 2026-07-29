#ifndef SMTPS_SERVER_H
#define SMTPS_SERVER_H

#include "framework/tcp_server_base.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.hpp"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/outbound/smtp_outbound_client.h"

namespace mail_system {

class SmtpsServer : public TcpServerBase<SmtpsSession<TcpConnection>,
                                          SmtpsSession<SslConnection>> {
public:
    SmtpsServer(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
    virtual ~SmtpsServer() override;

    std::shared_ptr<TraditionalSmtpsFsm<TcpConnection>> get_tcp_fsm() const { return m_tcp_fsm; }
    std::shared_ptr<TraditionalSmtpsFsm<SslConnection>> get_ssl_fsm() const { return m_ssl_fsm; }

protected:
    void stop(ServerState state = ServerState::Pausing) override;
    bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const override;

    // TcpServerBase 工厂
    std::shared_ptr<SmtpsSession<TcpConnection>> make_tcp_session(
        std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc) override;
    std::shared_ptr<SmtpsSession<SslConnection>> make_ssl_session(
        std::unique_ptr<SslConnection> conn, const ListenerConfig& lc) override;

private:
    std::shared_ptr<TraditionalSmtpsFsm<TcpConnection>> m_tcp_fsm;
    std::shared_ptr<TraditionalSmtpsFsm<SslConnection>> m_ssl_fsm;

public:
    std::shared_ptr<persist_storage::PersistentQueue> m_persistentQueue;
    std::shared_ptr<outbound::SmtpOutboundClient> m_outboundClient;
    std::shared_ptr<std::atomic<bool>> m_outboundInterruptFlag;
};

} // namespace mail_system

#endif // SMTPS_SERVER_H
