#include "mail_system/back/mailServer/fsm/BoostMsmSmtpsFsm.h"
#include <regex>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <algorithm>

namespace mail_system {
namespace SMTPS {

namespace {
    // 辅助函数：获取当前时间戳字符串
    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    // 辅助函数：记录日志
    void logMessage(const std::string& message, bool isError = false) {
        std::string timestamp = getCurrentTimestamp();
        if (isError) {
            std::cerr << "[" << timestamp << "] ERROR: " << message << std::endl;
        } else {
            std::cout << "[" << timestamp << "] INFO: " << message << std::endl;
        }
    }

    // 辅助函数：清理和规范化邮件地址
    std::string normalizeEmailAddress(const std::string& email) {
        std::string normalized = email;
        // 转换为小写
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
        // 移除首尾空白字符
        auto start = normalized.find_first_not_of(" \t\r\n");
        auto end = normalized.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        return normalized.substr(start, end - start + 1);
    }
}

// BoostMsmSmtpsFsm实现
BoostMsmSmtpsFsm::BoostMsmSmtpsFsm() {
    m_context.state = SmtpsState::INIT;
    // 创建状态机实例
    m_stateMachine = std::make_unique<SmtpsMsm>(std::ref(m_context));
}

void BoostMsmSmtpsFsm::initialize(std::shared_ptr<DBPool> dbPool) {
    m_dbPool = dbPool;
    // 初始化状态机
    m_stateMachine->start();
    // 初始化上下文
    m_context = SmtpsContext();
}

std::pair<int, std::string> BoostMsmSmtpsFsm::processEvent(
    SmtpsEvent event, const std::string& args, SmtpsContext& context) {
    
    try {
        // 保存上下文到成员变量
        m_context = context;
        
        std::cout << "Processing SMTP event: " << static_cast<int>(event) 
                  << " in state: " << static_cast<int>(context.state) << std::endl;
        
        // 处理全局命令
        switch (event) {
            case SmtpsEvent::QUIT:
                std::cout << "Client requested QUIT" << std::endl;
                context.state = SmtpsState::QUIT;
                return {221, "Bye"};
                
            case SmtpsEvent::RSET:
                std::cout << "Resetting connection state" << std::endl;
                reset(context);
                return {250, "OK"};
                
            case SmtpsEvent::NOOP:
                return {250, "OK"};
        }
        
        // 验证命令序列
        if (!isValidCommandSequence(event, context.state)) {
            std::cerr << "Invalid command sequence: event " << static_cast<int>(event) 
                      << " in state " << static_cast<int>(context.state) << std::endl;
            return {503, "Bad sequence of commands"};
        }
        
        // 根据事件类型处理
        switch (event) {
            case SmtpsEvent::EHLO:
            case SmtpsEvent::HELO: {
                if (args.empty()) {
                    std::cerr << "Empty HELO/EHLO argument" << std::endl;
                    return {501, "Syntax error in parameters or arguments"};
                }
                
                std::cout << "Processing HELO/EHLO from: " << args << std::endl;
                dispatchEvent(event, context);
                context.clientName = args;
                
                std::ostringstream response;
                response << "250-Hello " << args << "\r\n"
                        << "250-SIZE 35882577\r\n"
                        << "250-8BITMIME\r\n"
                        << "250-PIPELINING\r\n"
                        << "250-AUTH LOGIN PLAIN\r\n"
                        << "250-STARTTLS\r\n"
                        << "250 HELP";
                
                context.state = m_context.state;
                return {250, response.str()};
            }
            
            case SmtpsEvent::MAIL: {
                std::regex fromRegex(R"(FROM:\s*<([^>]+)>)", std::regex::icase);
                std::smatch match;
                
                if (!std::regex_search(args, match, fromRegex) || match.size() <= 1) {
                    std::cerr << "Invalid MAIL FROM syntax: " << args << std::endl;
                    return {501, "Syntax error in parameters or arguments"};
                }
                
                std::string sender = match[1].str();
                if (!isValidEmailAddress(sender)) {
                    std::cerr << "Invalid sender address: " << sender << std::endl;
                    return {501, "Invalid sender address"};
                }
                
                std::cout << "Processing MAIL FROM: " << sender << std::endl;
                dispatchEvent(event, context);
                context.sender = sender;
                context.state = m_context.state;
                return {250, "OK"};
            }
            
            case SmtpsEvent::RCPT: {
                if (context.sender.empty()) {
                    std::cerr << "RCPT TO before MAIL FROM" << std::endl;
                    return {503, "Need MAIL command first"};
                }
                
                std::regex toRegex(R"(TO:\s*<([^>]+)>)", std::regex::icase);
                std::smatch match;
                
                if (!std::regex_search(args, match, toRegex) || match.size() <= 1) {
                    std::cerr << "Invalid RCPT TO syntax: " << args << std::endl;
                    return {501, "Syntax error in parameters or arguments"};
                }
                
                std::string recipient = match[1].str();
                if (!isValidEmailAddress(recipient)) {
                    std::cerr << "Invalid recipient address: " << recipient << std::endl;
                    return {501, "Invalid recipient address"};
                }
                
                std::cout << "Processing RCPT TO: " << recipient << std::endl;
                dispatchEvent(event, context);
                context.recipients.push_back(recipient);
                context.state = m_context.state;
                return {250, "OK"};
            }
            
            case SmtpsEvent::DATA: {
                if (context.recipients.empty()) {
                    std::cerr << "DATA command without recipients" << std::endl;
                    return {503, "Need RCPT command first"};
                }
                
                std::cout << "Starting DATA phase" << std::endl;
                dispatchEvent(event, context);
                context.state = m_context.state;
                return {354, "Start mail input; end with <CRLF>.<CRLF>"};
            }
            
            case SmtpsEvent::DATA_LINE: {
                if (args == ".") {
                    std::cout << "End of DATA, saving mail" << std::endl;
                    dispatchEvent(SmtpsEvent::DATA_END, context);
                    if (saveMailToDB(context)) {
                        context.state = m_context.state;
                        return {250, "OK"};
                    }
                    std::cerr << "Failed to save mail to database" << std::endl;
                    return {554, "Transaction failed"};
                }
                
                dispatchEvent(event, context);
                context.messageData += args + "\r\n";
                context.state = m_context.state;
                return {0, ""}; // 继续接收数据
            }
            
            case SmtpsEvent::AUTH: {
                std::istringstream iss(args);
                std::string authType;
                iss >> authType;
                
                if (authType != "LOGIN" && authType != "PLAIN") {
                    std::cerr << "Unsupported authentication type: " << authType << std::endl;
                    return {504, "Unrecognized authentication type"};
                }
                
                std::cout << "Starting authentication with type: " << authType << std::endl;
                dispatchEvent(event, context);
                context.authType = authType;
                context.state = m_context.state;
                return {334, "VXNlcm5hbWU6"}; // Base64 encoded "Username:"
            }
            
            default:
                std::cerr << "Unknown command received: " << static_cast<int>(event) << std::endl;
                return {500, "Unknown command"};
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error processing SMTP event: " << e.what() << std::endl;
        return {451, "Internal server error"};
    }
}

// 辅助方法：验证命令序列是否有效
// bool BoostMsmSmtpsFsm::isValidCommandSequence(SmtpsEvent event, SmtpsState state) {
//     // 某些命令在任何状态下都是有效的
//     if (event == SmtpsEvent::QUIT || 
//         event == SmtpsEvent::NOOP || 
//         event == SmtpsEvent::RSET) {
//         return true;
//     }
    
//     switch (state) {
//         case SmtpsState::INIT:
//             return event == SmtpsEvent::EHLO || event == SmtpsEvent::HELO;
            
//         case SmtpsState::EHLO:
//             return event == SmtpsEvent::MAIL || 
//                    event == SmtpsEvent::AUTH || 
//                    event == SmtpsEvent::STARTTLS;
            
//         case SmtpsState::MAIL_FROM:
//             return event == SmtpsEvent::RCPT;
            
//         case SmtpsState::RCPT_TO:
//             return event == SmtpsEvent::RCPT || event == SmtpsEvent::DATA;
            
//         case SmtpsState::DATA:
//         case SmtpsState::DATA_CONTENT:
//             return event == SmtpsEvent::DATA_LINE;
            
//         case SmtpsState::AUTH:
//         case SmtpsState::AUTH_USERNAME:
//         case SmtpsState::AUTH_PASSWORD:
//             return event == SmtpsEvent::AUTH_DATA;
            
//         default:
//             return false;
//     }
// }

SmtpsState BoostMsmSmtpsFsm::getCurrentState(const SmtpsContext& context) const {
    return context.state;
}

void BoostMsmSmtpsFsm::reset(SmtpsContext& context) {
    // 重置状态机
    m_stateMachine->stop();
    m_stateMachine->start();
    
    // 重置上下文
    context = SmtpsContext();
    m_context = context;
}

bool BoostMsmSmtpsFsm::authenticateUser(const std::string& username, const std::string& password) {
    if (!m_dbPool) {
        std::cerr << "Database pool not initialized" << std::endl;
        return false;
    }

    try {
        auto conn = m_dbPool->getConnection();
        if (!conn) {
            std::cerr << "Failed to get database connection" << std::endl;
            return false;
        }

        // 使用参数化查询防止SQL注入
        // 在实际应用中，密码应该是已经哈希过的
        const std::string query = 
            "SELECT id, password_hash FROM users WHERE username = ? AND active = true LIMIT 1";
        
        auto stmt = conn->prepareStatement(query);
        if (!stmt) {
            std::cerr << "Failed to prepare statement" << std::endl;
            return false;
        }

        stmt->setString(1, username);
        auto result = stmt->executeQuery();
        
        if (!result) {
            std::cerr << "Query execution failed" << std::endl;
            return false;
        }

        if (result->next()) {
            std::string storedHash = result->getString("password_hash");
            // 在实际应用中，这里应该使用安全的密码哈希比较函数
            // 例如：return bcrypt_compare(password, storedHash);
            return storedHash == password; // 临时实现，实际应用中需要改进
        }

        return false; // 用户不存在
        
    } catch (const std::exception& e) {
        std::cerr << "Authentication error: " << e.what() << std::endl;
        return false;
    }
}

bool BoostMsmSmtpsFsm::saveMailToDB(const SmtpsContext& context) {
    if (!m_dbPool) {
        std::cerr << "Database pool not initialized" << std::endl;
        return false;
    }
    
    // 验证邮件数据的完整性
    if (context.sender.empty() || context.recipients.empty()) {
        std::cerr << "Invalid mail data: missing sender or recipients" << std::endl;
        return false;
    }

    try {
        auto conn = m_dbPool->getConnection();
        if (!conn) {
            std::cerr << "Failed to get database connection" << std::endl;
            return false;
        }

        // 开始事务
        conn->beginTransaction();
        
        // 从邮件内容中提取主题和其他元数据
        std::string subject = "No Subject";
        std::string messageId;
        std::string date;
        
        std::regex subjectRegex(R"(Subject:\s*(.+?)(?:\r\n|\n))");
        std::regex messageIdRegex(R"(Message-ID:\s*(.+?)(?:\r\n|\n))");
        std::regex dateRegex(R"(Date:\s*(.+?)(?:\r\n|\n))");
        
        std::smatch match;
        if (std::regex_search(context.messageData, match, subjectRegex) && match.size() > 1) {
            subject = match[1].str();
        }
        
        if (std::regex_search(context.messageData, match, messageIdRegex) && match.size() > 1) {
            messageId = match[1].str();
        }
        
        if (std::regex_search(context.messageData, match, dateRegex) && match.size() > 1) {
            date = match[1].str();
        }
        
        // 插入邮件记录
        const std::string insertMailQuery = 
            "INSERT INTO mails (sender, subject, content, message_id, mail_date, created_at) "
            "VALUES (?, ?, ?, ?, ?, NOW())";
        
        auto stmt = conn->prepareStatement(insertMailQuery);
        if (!stmt) {
            std::cerr << "Failed to prepare mail insert statement" << std::endl;
            conn->rollback();
            return false;
        }
        
        stmt->setString(1, context.sender);
        stmt->setString(2, subject);
        stmt->setString(3, context.messageData);
        stmt->setString(4, messageId);
        stmt->setString(5, date);
        
        int rowsAffected = stmt->executeUpdate();
        if (rowsAffected != 1) {
            std::cerr << "Failed to insert mail record" << std::endl;
            conn->rollback();
            return false;
        }
        
        // 获取插入的邮件ID
        auto result = conn->executeQuery("SELECT LAST_INSERT_ID()");
        if (!result || !result->next()) {
            std::cerr << "Failed to get last insert ID" << std::endl;
            conn->rollback();
            return false;
        }
        
        int mailId = result->getInt(1);
        if (mailId <= 0) {
            std::cerr << "Invalid mail ID: " << mailId << std::endl;
            conn->rollback();
            return false;
        }
        
        // 插入收件人记录
        const std::string insertRecipientQuery = 
            "INSERT INTO mail_recipients (mail_id, recipient, recipient_type) VALUES (?, ?, ?)";
        
        auto recipientStmt = conn->prepareStatement(insertRecipientQuery);
        if (!recipientStmt) {
            std::cerr << "Failed to prepare recipient insert statement" << std::endl;
            conn->rollback();
            return false;
        }
        
        for (const auto& recipient : context.recipients) {
            recipientStmt->setInt(1, mailId);
            recipientStmt->setString(2, recipient);
            recipientStmt->setString(3, "TO"); // 可以扩展为 CC, BCC 等
            
            rowsAffected = recipientStmt->executeUpdate();
            if (rowsAffected != 1) {
                std::cerr << "Failed to insert recipient: " << recipient << std::endl;
                conn->rollback();
                return false;
            }
        }
        
        // 提交事务
        conn->commit();
        std::cout << "Mail saved successfully with ID: " << mailId << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error saving mail: " << e.what() << std::endl;
        if (conn) {
            try {
                conn->rollback();
            } catch (const std::exception& rollbackEx) {
                std::cerr << "Error during rollback: " << rollbackEx.what() << std::endl;
            }
        }
        return false;
    }
}

bool BoostMsmSmtpsFsm::isValidEmailAddress(const std::string& email) {
    if (email.empty() || email.length() > 254) {
        std::cerr << "Email validation failed: empty or too long" << std::endl;
        return false;
    }

    try {
        // RFC 5322 compliant email regex
        // Local part: Allows quoted strings and special characters
        // Domain part: Allows international domains and subdomains
        const std::regex emailRegex(
            R"((?:[a-z0-9!#$%&'*+/=?^_`{|}~-]+(?:\.[a-z0-9!#$%&'*+/=?^_`{|}~-]+)*|"(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21\x23-\x5b\x5d-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])*")@(?:(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z0-9](?:[a-z0-9-]*[a-z0-9])?|\[(?:(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9]))\.){3}(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]|[1-9]?[0-9])|[a-z0-9-]*[a-z0-9]:(?:[\x01-\x08\x0b\x0c\x0e-\x1f\x21-\x5a\x53-\x7f]|\\[\x01-\x09\x0b\x0c\x0e-\x7f])+)\]))",
            std::regex::icase
        );

        if (!std::regex_match(email, emailRegex)) {
            std::cerr << "Email validation failed: invalid format for " << email << std::endl;
            return false;
        }

        // Additional checks
        size_t atPos = email.find('@');
        if (atPos == std::string::npos) {
            std::cerr << "Email validation failed: missing @ symbol" << std::endl;
            return false;
        }

        std::string localPart = email.substr(0, atPos);
        std::string domainPart = email.substr(atPos + 1);

        // Check local part length
        if (localPart.empty() || localPart.length() > 64) {
            std::cerr << "Email validation failed: local part length invalid" << std::endl;
            return false;
        }

        // Check domain part length
        if (domainPart.empty() || domainPart.length() > 255) {
            std::cerr << "Email validation failed: domain part length invalid" << std::endl;
            return false;
        }

        // Check for consecutive dots
        if (email.find("..") != std::string::npos) {
            std::cerr << "Email validation failed: consecutive dots not allowed" << std::endl;
            return false;
        }

        // Check start and end characters
        if (email.front() == '.' || email.back() == '.') {
            std::cerr << "Email validation failed: cannot start or end with dot" << std::endl;
            return false;
        }

        std::cout << "Email validation passed for: " << email << std::endl;
        return true;

    } catch (const std::regex_error& e) {
        std::cerr << "Regex error during email validation: " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Unexpected error during email validation: " << e.what() << std::endl;
        return false;
    }
}

void BoostMsmSmtpsFsm::dispatchEvent(SmtpsEvent event, SmtpsContext& context) {
    switch (event) {
        case SmtpsEvent::EHLO:
        case SmtpsEvent::HELO:
            m_stateMachine->process_event(EhloEvent());
            break;
        case SmtpsEvent::MAIL:
            m_stateMachine->process_event(MailEvent());
            break;
        case SmtpsEvent::RCPT:
            m_stateMachine->process_event(RcptEvent());
            break;
        case SmtpsEvent::DATA:
            m_stateMachine->process_event(DataEvent());
            break;
        case SmtpsEvent::DATA_LINE:
            m_stateMachine->process_event(DataLineEvent());
            break;
        case SmtpsEvent::DATA_END:
            m_stateMachine->process_event(DataEndEvent());
            break;
        case SmtpsEvent::AUTH:
            m_stateMachine->process_event(AuthEvent());
            break;
        case SmtpsEvent::AUTH_DATA:
            m_stateMachine->process_event(AuthDataEvent());
            break;
        case SmtpsEvent::STARTTLS:
            m_stateMachine->process_event(StartTlsEvent());
            break;
        default:
            m_stateMachine->process_event(UnknownEvent());
            break;
    }
    
    // 更新上下文状态
    context.state = m_context.state;
}

} // namespace SMTPS
} // namespace mail_system