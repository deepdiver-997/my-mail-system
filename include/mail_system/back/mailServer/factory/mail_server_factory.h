#ifndef MAIL_SYSTEM_MAIL_SERVER_FACTORY_H
#define MAIL_SYSTEM_MAIL_SERVER_FACTORY_H

#include <memory>
#include <string>
#include <stdexcept>
#include "mail_server_base.h"
#include "server_config.h"

namespace mail_system {

// 邮件服务器类型枚举
enum class MailServerType {
    SMTP,       // 简单邮件传输协议
    SMTPS,      // 安全的SMTP
    POP3,       // 邮局协议版本3
    POP3S,      // 安全的POP3
    IMAP,       // 互联网消息访问协议
    IMAPS       // 安全的IMAP
};

// 邮件服务器工厂类
class MailServerFactory {
public:
    // 创建邮件服务器
    template<typename SessionType, typename FsmFactoryType>
    static std::unique_ptr<MailServerBase<SessionType, FsmFactoryType>> 
    createServer(MailServerType type, const ServerConfig& config, std::unique_ptr<FsmFactoryType> fsm_factory) {
        // 根据服务器类型设置默认端口
        ServerConfig server_config = config;
        if (server_config.port == 0) {
            server_config.port = getDefaultPort(type);
        }
        
        // 根据服务器类型设置SSL
        if (type == MailServerType::SMTPS || type == MailServerType::POP3S || type == MailServerType::IMAPS) {
            server_config.use_ssl = true;
        }
        
        // 验证配置
        if (!server_config.validate()) {
            throw std::invalid_argument("Invalid server configuration");
        }
        
        // 创建服务器
        return std::make_unique<MailServerBase<SessionType, FsmFactoryType>>(
            server_config, 
            std::move(fsm_factory)
        );
    }
    
    // 获取默认端口
    static uint16_t getDefaultPort(MailServerType type) {
        switch (type) {
            case MailServerType::SMTP:
                return 25;
            case MailServerType::SMTPS:
                return 465;
            case MailServerType::POP3:
                return 110;
            case MailServerType::POP3S:
                return 995;
            case MailServerType::IMAP:
                return 143;
            case MailServerType::IMAPS:
                return 993;
            default:
                throw std::invalid_argument("Unknown mail server type");
        }
    }
    
    // 获取服务器类型名称
    static std::string getServerTypeName(MailServerType type) {
        switch (type) {
            case MailServerType::SMTP:
                return "SMTP";
            case MailServerType::SMTPS:
                return "SMTPS";
            case MailServerType::POP3:
                return "POP3";
            case MailServerType::POP3S:
                return "POP3S";
            case MailServerType::IMAP:
                return "IMAP";
            case MailServerType::IMAPS:
                return "IMAPS";
            default:
                return "Unknown";
        }
    }
};

} // namespace mail_system

#endif // MAIL_SYSTEM_MAIL_SERVER_FACTORY_H