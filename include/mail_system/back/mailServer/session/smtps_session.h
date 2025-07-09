#ifndef SMTPS_SESSION_H
#define SMTPS_SESSION_H

#include "session_base.h"
#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace mail_system {

// SMTP状态枚举
enum class SmtpsState {
    INIT,               // 初始状态
    GREETING,          // 发送问候语
    WAIT_EHLO,        // 等待EHLO命令
    WAIT_AUTH,        // 等待AUTH命令
    WAIT_MAIL_FROM,   // 等待MAIL FROM命令
    WAIT_RCPT_TO,     // 等待RCPT TO命令
    WAIT_DATA,        // 等待DATA命令
    IN_MESSAGE,       // 接收邮件内容
    WAIT_QUIT         // 等待QUIT命令
};

// SMTP事件枚举
enum class SmtpsEvent {
    CONNECT,          // 连接建立
    EHLO,            // 收到EHLO命令
    AUTH,            // 收到AUTH命令
    MAIL_FROM,       // 收到MAIL FROM命令
    RCPT_TO,         // 收到RCPT TO命令
    DATA,            // 收到DATA命令
    DATA_END,        // 收到数据结束标记（.）
    QUIT,            // 收到QUIT命令
    ERROR,           // 发生错误
    TIMEOUT          // 超时
};

// SMTP会话上下文
struct SmtpsContext {
    std::string client_hostname;     // 客户端主机名
    std::string sender_address;      // 发件人地址
    std::vector<std::string> recipient_addresses;  // 收件人地址列表
    std::string message_data;        // 邮件内容
    bool is_authenticated;           // 是否已认证
    
    // 清理上下文数据
    void clear() {
        client_hostname.clear();
        sender_address.clear();
        recipient_addresses.clear();
        message_data.clear();
        is_authenticated = false;
    }
};

// 前向声明
class SmtpsFsm;

class SmtpsSession : public SessionBase {
public:
    SmtpsSession(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> &&socket, std::shared_ptr<SmtpsFsm> fsm);
    ~SmtpsSession() override;

    // 启动会话
    void start() override;

    SmtpsState get_current_state() const {
        return current_state_;
    }

    void set_current_state(SmtpsState state) {
        current_state_ = state;
    }

protected:
    // 处理接收到的数据
    void handle_read(const std::string& data) override;
    
    void process_command(const std::string& command);

private:
    std::shared_ptr<SmtpsFsm> m_fsm;  // 状态机
    SmtpsContext context_;           // 会话上下文
    SmtpsState current_state_;      // 当前状态
    bool m_receivingData;            // 是否在接收数据模式
    std::string m_mailData;          // 邮件数据缓存
};

} // namespace mail_system

#endif // SMTPS_SESSION_H