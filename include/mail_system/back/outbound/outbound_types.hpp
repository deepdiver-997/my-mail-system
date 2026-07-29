#ifndef OUTBOUND_SMTP_FSM_HPP
#define OUTBOUND_SMTP_FSM_HPP

#include "mail_system/back/entities/mail.h"
#include <cstdint>
#include <string>
#include <memory>

namespace mail_system {
namespace outbound {

// ================================================================
// 投递任务
// ================================================================
struct MailDeliveryTask {
    uint64_t mail_id;
    uint64_t record_id;        // mail_outbox.id
    std::string sender;        // 信封 MAIL FROM（已由 router 解析到本 MX）
    std::string recipient;     // 信封 RCPT TO（单个收件人，已去重分派）
    std::shared_ptr<struct mail> mail_ptr;  // 邮件对象（多个 session 共享，只读）
    int attempt_count;
    int max_attempts;
};

// ================================================================
// Outbound SMTP 状态
// ================================================================
enum class OutboundSmtpState {
    INIT,              // 初始，尚未 connect
    CONNECTING,        // connect() 已发起，等待连接
    CONNECTED,         // 等待 220 问候
    EHLO,              // 已发 EHLO，等待 250
    STARTTLS,          // 已发 STARTTLS，等待 220（可选）
    MAIL_FROM,         // 已发 MAIL FROM，等待 250
    RCPT_TO,           // 已发 RCPT TO，等待 250
    DATA,              // 已发 DATA，等待 354
    DATA_BODY,         // 发送邮件正文中
    WAIT_ACCEPT,       // 正文发送完毕，等待 250 OK
    QUIT,              // 已发 QUIT，等待 221
    CLOSED,            // 连接已关闭
};

// ================================================================
// Outbound SMTP 事件（多数是接收到的响应码）
// ================================================================
enum class OutboundSmtpEvent {
    CONNECT,           // 发起连接
    CONNECTED,         // 连接成功
    GREETING_220,      // 收到 220
    EHLO_250,          // EHLO 成功
    STARTTLS_220,      // STARTTLS 成功
    MAIL_250,          // MAIL FROM 成功
    RCPT_250,          // RCPT TO 成功
    DATA_354,          // DATA 接受
    ACCEPT_250,        // 邮件被接受
    QUIT_221,          // QUIT 成功
    ERROR_4XX,         // 临时错误
    ERROR_5XX,         // 永久错误
    CONNECTION_LOST,   // 连接丢失
};

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_SMTP_FSM_HPP
