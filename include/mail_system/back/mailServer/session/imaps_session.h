#ifndef MAIL_SYSTEM_IMAPS_SESSION_H
#define MAIL_SYSTEM_IMAPS_SESSION_H

#include <memory>
#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imaps_fsm.h"
#include "mail_system/back/db/db_pool.h"

namespace mail_system {

class ImapsSession : public SessionBase<ImapsState, ImapsEvent, ImapsContext> {
public:
    ImapsSession(
        tcp::socket socket,
        std::shared_ptr<boost::asio::ssl::context> ssl_context,
        const ServerConfig& config,
        std::shared_ptr<DBPool> db_pool,
        std::shared_ptr<ThreadPoolBase> m_workerThreadPool,
        std::shared_ptr<ThreadPoolBase> m_ioThreadPool,
        std::unique_ptr<ImapsFsm> fsm
    );
    ~ImapsSession() override = default;

protected:
    // 发送问候消息
    void sendGreeting() override;
    // 处理接收到的数据
    void processData(const std::string& data) override;

private:
    // 处理IMAP命令
    void processCommand(const std::string& command);
    // 发送IMAP响应
    void sendImapResponse(const std::string& response);
};

} // namespace mail_system

#endif // MAIL_SYSTEM_IMAPS_SESSION_H