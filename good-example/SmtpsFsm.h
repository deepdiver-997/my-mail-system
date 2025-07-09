#ifndef MAIL_SYSTEM_SMTPS_FSM_H
#define MAIL_SYSTEM_SMTPS_FSM_H

#include <string>
#include <memory>
#include <vector>
#include "../../../back/db_pool.h"

namespace mail_system {
namespace SMTPS {

// SMTP会话状态枚举
enum class SmtpsState {
    INIT,           // 初始状态
    EHLO,           // 收到EHLO
    MAIL_FROM,      // 收到MAIL FROM
    RCPT_TO,        // 收到RCPT TO
    DATA,           // 收到DATA
    DATA_CONTENT,   // 接收数据内容
    AUTH,           // 身份验证
    AUTH_USERNAME,  // 等待用户名
    AUTH_PASSWORD,  // 等待密码
    TLS_START,      // 开始TLS
    QUIT            // 退出
};

// SMTP事件枚举
enum class SmtpsEvent {
    CONNECT,        // 客户端连接
    EHLO,           // EHLO命令
    HELO,           // HELO命令
    MAIL,           // MAIL FROM命令
    RCPT,           // RCPT TO命令
    DATA,           // DATA命令
    DATA_LINE,      // 数据行
    DATA_END,       // 数据结束（.）
    AUTH,           // AUTH命令
    AUTH_DATA,      // 认证数据
    STARTTLS,       // STARTTLS命令
    RSET,           // RSET命令
    NOOP,           // NOOP命令
    VRFY,           // VRFY命令
    QUIT,           // QUIT命令
    UNKNOWN         // 未知命令
};

// 会话上下文结构体
struct SmtpsContext {
    std::string clientName;
    std::string sender;
    std::vector<std::string> recipients;
    std::string messageData;
    bool authenticated;
    std::string username;
    std::string authType;
    SmtpsState state;
    
    SmtpsContext() : authenticated(false), state(SmtpsState::INIT) {}
};

// 状态机抽象基类
class SmtpsFsm {
public:
    virtual ~SmtpsFsm() = default;
    
    // 初始化状态机
    virtual void initialize(std::shared_ptr<DBPool> dbPool) = 0;
    
    // 处理事件
    virtual std::pair<int, std::string> processEvent(
        SmtpsEvent event, 
        const std::string& args, 
        SmtpsContext& context) = 0;
    
    // 获取当前状态
    virtual SmtpsState getCurrentState(const SmtpsContext& context) const = 0;
    
    // 重置状态机
    virtual void reset(SmtpsContext& context) = 0;
    
    // 验证用户身份
    virtual bool authenticateUser(const std::string& username, const std::string& password) = 0;
    
    // 保存邮件到数据库
    virtual bool saveMailToDB(const SmtpsContext& context) = 0;
    
    // 验证邮箱地址格式
    virtual bool isValidEmailAddress(const std::string& email) = 0;
};

} // namespace SMTPS
} // namespace mail_system

#endif // MAIL_SYSTEM_SMTPS_FSM_H