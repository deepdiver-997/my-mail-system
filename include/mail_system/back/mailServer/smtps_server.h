#ifndef SMTPS_SERVER_H
#define SMTPS_SERVER_H

#include "server_base.h"
#include "fsm/smtps/smtps_fsm.h"
#include "mail_system/back/mailServer/session/smtps_session.h"

namespace mail_system {

    class SmtpsServer : public ServerBase {
    public:
        SmtpsServer(const ServerConfig& config);
        ~SmtpsServer() override;

    protected:
        // 处理新连接
        void handle_accept(std::unique_ptr<SmtpsSession> session, const boost::system::error_code& error);

        std::shared_ptr<SmtpsFsm> m_fsm;
    };

} // namespace mail_system

#endif // SMTPS_SERVER_H