#ifndef MAIL_SYSTEM_POP3S_SESSION_H
#define MAIL_SYSTEM_POP3S_SESSION_H

#include <memory>
#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/mailServer/fsm/pop3s/pop3s_fsm.h"
#include "mail_system/back/db/db_pool.h"

namespace mail_system {

class Pop3sSession : public SessionBase<Pop3sState, Pop3sEvent, Pop3sContext> {
public:
    Pop3sSession(
        tcp::socket socket,
        std::shared_ptr<boost::asio::ssl::context> ssl_context,
        const ServerConfig& config,
        std::shared_ptr<DBPool> db_pool,
        std::shared_ptr<ThreadPoolBase> m_workerThreadPool,
        std::shared_ptr<ThreadPoolBase> m_ioThreadPool,
        std::unique_ptr<Pop3sFsm> fsm
    );
    ~Pop3sSession() override = default;

protected:
    // 发送问候消息
    void sendGreeting() override;
    // 处理接收到的数据
    void processData(const std::string& data) override;

private:
    // 处理POP3命令
    void processCommand(const std::string& command);
    // 发送POP3响应
    void sendPop3Response(const std::string& response);
    
    // 是否处于多行响应模式
    bool multilineResponse_{false};
};

} // namespace mail_system

#endif // MAIL_SYSTEM_POP3S_SESSION_H