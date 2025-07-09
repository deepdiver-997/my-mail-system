#include "include/mail_system/back/mailServer/fsm/TraditionalSmtpsFsm.h"
#include <regex>
#include <sstream>

namespace mail_system {
namespace SMTPS {

TraditionalSmtpsFsm::TraditionalSmtpsFsm() {
    initializeStateTransitions();
}

void TraditionalSmtpsFsm::initialize(std::shared_ptr<DBPool> dbPool) {
    m_dbPool = dbPool;
}

void TraditionalSmtpsFsm::initializeStateTransitions() {
    // 初始化状态转换表
    m_stateTransitions[SmtpsState::INIT] = {
        {SmtpsEvent::EHLO, [this](const std::string& args, SmtpsContext& ctx) { return handleEhlo(args, ctx); }},
        {SmtpsEvent::HELO, [this](const std::string& args, SmtpsContext& ctx) { return handleEhlo(args, ctx); }}
    };

    m_stateTransitions[SmtpsState::EHLO] = {
        {SmtpsEvent::MAIL, [this](const std::string& args, SmtpsContext& ctx) { return handleMailFrom(args, ctx); }},
        {SmtpsEvent::AUTH, [this](const std::string& args, SmtpsContext& ctx) { return handleAuth(args, ctx); }},
        {SmtpsEvent::STARTTLS, [this](const std::string& args, SmtpsContext& ctx) { return handleTlsStart(args, ctx); }}
    };

    m_stateTransitions[SmtpsState::MAIL_FROM] = {
        {SmtpsEvent::RCPT, [this](const std::string& args, SmtpsContext& ctx) { return handleRcptTo(args, ctx); }}
    };

    m_stateTransitions[SmtpsState::RCPT_TO] = {
        {SmtpsEvent::RCPT, [this](const std::string& args, SmtpsContext& ctx) { return handleRcptTo(args, ctx); }},
        {SmtpsEvent::DATA, [this](const std::string& args, SmtpsContext& ctx) { return handleData(args, ctx); }}
    };

    m_stateTransitions[SmtpsState::DATA] = {
        {SmtpsEvent::DATA_LINE, [this](const std::string& args, SmtpsContext& ctx) { return handleDataContent(args, ctx); }}
    };

    // 其他状态的转换处理...
}

std::pair<int, std::string> TraditionalSmtpsFsm::processEvent(
    SmtpsEvent event, const std::string& args, SmtpsContext& context) {
    
    // 检查是否是全局命令（在任何状态都可以执行的命令）
    if (event == SmtpsEvent::QUIT) {
        return handleQuit(args, context);
    }
    if (event == SmtpsEvent::RSET) {
        reset(context);
        return {250, "OK"};
    }
    if (event == SmtpsEvent::NOOP) {
        return {250, "OK"};
    }

    // 检查状态转换是否有效
    auto currentState = context.state;
    if (!isValidTransition(currentState, event)) {
        return {503, "Bad sequence of commands"};
    }

    // 执行状态处理函数
    auto& stateHandlers = m_stateTransitions[currentState];
    auto handlerIt = stateHandlers.find(event);
    if (handlerIt != stateHandlers.end()) {
        return handlerIt->second(args, context);
    }

    return {500, "Unknown command"};
}

SmtpsState TraditionalSmtpsFsm::getCurrentState(const SmtpsContext& context) const {
    return context.state;
}

void TraditionalSmtpsFsm::reset(SmtpsContext& context) {
    context.state = SmtpsState::EHLO;
    context.sender.clear();
    context.recipients.clear();
    context.messageData.clear();
}

bool TraditionalSmtpsFsm::authenticateUser(const std::string& username, const std::string& password) {
    try {
        auto conn = m_dbPool->getConnection();
        // TODO: 实现实际的用户认证逻辑
        return true;  // 临时返回true
    } catch (const std::exception& e) {
        return false;
    }
}

bool TraditionalSmtpsFsm::saveMailToDB(const SmtpsContext& context) {
    try {
        auto conn = m_dbPool->getConnection();
        // TODO: 实现邮件保存逻辑
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

bool TraditionalSmtpsFsm::isValidEmailAddress(const std::string& email) {
    std::regex emailRegex(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    return std::regex_match(email, emailRegex);
}

// 状态处理函数实现
std::pair<int, std::string> TraditionalSmtpsFsm::handleEhlo(const std::string& args, SmtpsContext& context) {
    if (args.empty()) {
        return {501, "Syntax error in parameters or arguments"};
    }
    
    context.state = SmtpsState::EHLO;
    context.clientName = args;
    
    std::ostringstream response;
    response << "250-Hello " << args << "\r\n";
    response << "250-SIZE 35882577\r\n";
    response << "250-AUTH LOGIN PLAIN\r\n";
    response << "250 HELP";
    
    return {250, response.str()};
}

std::pair<int, std::string> TraditionalSmtpsFsm::handleMailFrom(const std::string& args, SmtpsContext& context) {
    std::regex fromRegex(R"(FROM:\s*<([^>]+)>)", std::regex::icase);
    std::smatch match;
    
    if (std::regex_search(args, match, fromRegex) && match.size() > 1) {
        std::string sender = match[1].str();
        
        if (!isValidEmailAddress(sender)) {
            return {501, "Syntax error in parameters or arguments"};
        }
        
        context.state = SmtpsState::MAIL_FROM;
        context.sender = sender;
        return {250, "OK"};
    }
    
    return {501, "Syntax error in parameters or arguments"};
}

std::pair<int, std::string> TraditionalSmtpsFsm::handleRcptTo(const std::string& args, SmtpsContext& context) {
    std::regex toRegex(R"(TO:\s*<([^>]+)>)", std::regex::icase);
    std::smatch match;
    
    if (std::regex_search(args, match, toRegex) && match.size() > 1) {
        std::string recipient = match[1].str();
        
        if (!isValidEmailAddress(recipient)) {
            return {501, "Syntax error in parameters or arguments"};
        }
        
        context.state = SmtpsState::RCPT_TO;
        context.recipients.push_back(recipient);
        return {250, "OK"};
    }
    
    return {501, "Syntax error in parameters or arguments"};
}

std::pair<int, std::string> TraditionalSmtpsFsm::handleData(const std::string& args, SmtpsContext& context) {
    context.state = SmtpsState::DATA_CONTENT;
    return {354, "Start mail input; end with <CRLF>.<CRLF>"};
}

std::pair<int, std::string> TraditionalSmtpsFsm::handleDataContent(const std::string& args, SmtpsContext& context) {
    if (args == ".") {
        if (saveMailToDB(context)) {
            context.state = SmtpsState::EHLO;
            return {250, "OK"};
        }
        return {554, "Transaction failed"};
    }
    
    context.messageData += args + "\r\n";
    return {0, ""}; // 继续接收数据
}

std::pair<int, std::string> TraditionalSmtpsFsm::handleAuth(const std::string& args, SmtpsContext& context) {
    std::istringstream iss(args);
    std::string authType;
    iss >> authType;
    
    if (authType == "LOGIN" || authType == "PLAIN") {
        context.state = SmtpsState::AUTH;
        context.authType = authType;
        return {334, "VXNlcm5hbWU6"}; // Base64 encoded "Username:"
    }
    
    return {504, "Unrecognized authentication type"};
}

std::pair<int, std::string> TraditionalSmtpsFsm::handleQuit(const std::string& args, SmtpsContext& context) {
    return {221, "Bye"};
}

bool TraditionalSmtpsFsm::isValidTransition(SmtpsState currentState, SmtpsEvent event) const {
    auto stateIt = m_stateTransitions.find(currentState);
    if (stateIt == m_stateTransitions.end()) {
        return false;
    }
    
    return stateIt->second.find(event) != stateIt->second.end();
}

} // namespace SMTPS
} // namespace mail_system