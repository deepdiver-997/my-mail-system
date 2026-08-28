#ifndef POP3_SERVER_H
#define POP3_SERVER_H

#include "framework/tcp_server_base.h"
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.h"
#include "mail_system/back/mailServer/session/pop3_session.h"
#include <boost/asio/steady_timer.hpp>
#include <functional>

namespace mail_system {

class Pop3Server : public TcpServerBase<Pop3Session<TcpConnection>,
                                         Pop3Session<SslConnection>> {
public:
    Pop3Server(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr);
    ~Pop3Server() override;

    void start() override;

protected:
    void stop(ServerState state = ServerState::Pausing) override;

    bool should_reject_connection(std::string& reason, const std::string& client_ip = "") const override;

    std::shared_ptr<Pop3Session<TcpConnection>> make_tcp_session(
        std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc) override;
    std::shared_ptr<Pop3Session<SslConnection>> make_ssl_session(
        std::unique_ptr<SslConnection> conn, const ListenerConfig& lc) override;

private:
    // 锁清扫（v2）：周期回收心跳过期的 pop3_session_lock，防硬崩溃死锁
    void start_lock_sweeper();
    std::shared_ptr<boost::asio::steady_timer> m_sweep_timer;
    std::function<void(const boost::system::error_code&)> m_sweep_tick;

    std::shared_ptr<TraditionalPop3Fsm<TcpConnection>> m_tcp_fsm;
    std::shared_ptr<TraditionalPop3Fsm<SslConnection>> m_ssl_fsm;
};

} // namespace mail_system

#endif // POP3_SERVER_H
