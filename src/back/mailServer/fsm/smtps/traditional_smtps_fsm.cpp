#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include <iostream>

namespace mail_system {

TraditionalSmtpsFsm::TraditionalSmtpsFsm() {
    init_transition_table();
    init_state_handlers();
}

void TraditionalSmtpsFsm::process_event(SmtpsSession* session, SmtpsEvent event, const std::string& args) {
    // 查找状态转换
    auto transition_key = std::make_pair(session->get_current_state(), event);
    auto transition_it = transition_table_.find(transition_key);
    
    if (transition_it != transition_table_.end()) {
        // 找到有效的状态转换
        SmtpsState next_state = transition_it->second;
        
        // 查找状态处理函数
        auto state_handler_it = state_handlers_.find(session->get_current_state());
        if (state_handler_it != state_handlers_.end()) {
            auto event_handler_it = state_handler_it->second.find(event);
            if (event_handler_it != state_handler_it->second.end()) {
                // 执行状态处理函数
                event_handler_it->second(session, args);
            }
        }
        
        // 更新当前状态
        session->set_current_state(next_state);
        
        std::cout << "SMTPS FSM: " << get_state_name(session->get_current_state()) << " -> " 
                  << get_event_name(event) << " -> " << get_state_name(next_state) << std::endl;
    } else {
        // 无效的状态转换
        std::cerr << "SMTPS FSM: Invalid transition from " << get_state_name(session->get_current_state()) 
                  << " on event " << get_event_name(event) << std::endl;
        
        // 处理错误
        handle_error(session, "Invalid command sequence");
    }
}

void TraditionalSmtpsFsm::init_transition_table() {
    // 初始化状态转换表
    transition_table_[std::make_pair(SmtpsState::INIT, SmtpsEvent::CONNECT)] = SmtpsState::GREETING;
    transition_table_[std::make_pair(SmtpsState::GREETING, SmtpsEvent::EHLO)] = SmtpsState::WAIT_MAIL_FROM;
    transition_table_[std::make_pair(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO)] = SmtpsState::WAIT_MAIL_FROM;
    transition_table_[std::make_pair(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;
    transition_table_[std::make_pair(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO)] = SmtpsState::WAIT_DATA;
    transition_table_[std::make_pair(SmtpsState::WAIT_DATA, SmtpsEvent::DATA)] = SmtpsState::IN_MESSAGE;
    transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END)] = SmtpsState::WAIT_MAIL_FROM;
    
    // QUIT命令可以在多个状态下接收
    transition_table_[std::make_pair(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::QUIT)] = SmtpsState::WAIT_QUIT;
    transition_table_[std::make_pair(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::QUIT)] = SmtpsState::WAIT_QUIT;
    transition_table_[std::make_pair(SmtpsState::WAIT_DATA, SmtpsEvent::QUIT)] = SmtpsState::WAIT_QUIT;
    transition_table_[std::make_pair(SmtpsState::WAIT_QUIT, SmtpsEvent::QUIT)] = SmtpsState::WAIT_QUIT;
    
    // 错误处理
    transition_table_[std::make_pair(SmtpsState::INIT, SmtpsEvent::ERROR)] = SmtpsState::INIT;
    transition_table_[std::make_pair(SmtpsState::GREETING, SmtpsEvent::ERROR)] = SmtpsState::GREETING;
    transition_table_[std::make_pair(SmtpsState::WAIT_EHLO, SmtpsEvent::ERROR)] = SmtpsState::WAIT_EHLO;
    transition_table_[std::make_pair(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::ERROR)] = SmtpsState::WAIT_MAIL_FROM;
    transition_table_[std::make_pair(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::ERROR)] = SmtpsState::WAIT_RCPT_TO;
    transition_table_[std::make_pair(SmtpsState::WAIT_DATA, SmtpsEvent::ERROR)] = SmtpsState::WAIT_DATA;
    transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::ERROR)] = SmtpsState::IN_MESSAGE;
    transition_table_[std::make_pair(SmtpsState::WAIT_QUIT, SmtpsEvent::ERROR)] = SmtpsState::WAIT_QUIT;
    
    // 超时处理
    transition_table_[std::make_pair(SmtpsState::INIT, SmtpsEvent::TIMEOUT)] = SmtpsState::INIT;
    transition_table_[std::make_pair(SmtpsState::GREETING, SmtpsEvent::TIMEOUT)] = SmtpsState::GREETING;
    transition_table_[std::make_pair(SmtpsState::WAIT_EHLO, SmtpsEvent::TIMEOUT)] = SmtpsState::WAIT_EHLO;
    transition_table_[std::make_pair(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::TIMEOUT)] = SmtpsState::WAIT_MAIL_FROM;
    transition_table_[std::make_pair(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::TIMEOUT)] = SmtpsState::WAIT_RCPT_TO;
    transition_table_[std::make_pair(SmtpsState::WAIT_DATA, SmtpsEvent::TIMEOUT)] = SmtpsState::WAIT_DATA;
    transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::TIMEOUT)] = SmtpsState::IN_MESSAGE;
    transition_table_[std::make_pair(SmtpsState::WAIT_QUIT, SmtpsEvent::TIMEOUT)] = SmtpsState::WAIT_QUIT;
}

void TraditionalSmtpsFsm::init_state_handlers() {
    // 初始化状态处理函数
    state_handlers_[SmtpsState::INIT][SmtpsEvent::CONNECT] = 
        std::bind(&TraditionalSmtpsFsm::handle_init_connect, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::GREETING][SmtpsEvent::EHLO] = 
        std::bind(&TraditionalSmtpsFsm::handle_greeting_ehlo, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_EHLO][SmtpsEvent::EHLO] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_ehlo_ehlo, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_MAIL_FROM][SmtpsEvent::MAIL_FROM] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_mail_from_mail_from, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_RCPT_TO][SmtpsEvent::RCPT_TO] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_DATA][SmtpsEvent::DATA] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_data_data, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::IN_MESSAGE][SmtpsEvent::DATA_END] = 
        std::bind(&TraditionalSmtpsFsm::handle_in_message_data_end, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_QUIT][SmtpsEvent::QUIT] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_quit_quit, this, std::placeholders::_1, std::placeholders::_2);
    
    // 错误处理函数
    for (int i = 0; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
        state_handlers_[static_cast<SmtpsState>(i)][SmtpsEvent::ERROR] = 
            std::bind(&TraditionalSmtpsFsm::handle_error, this, std::placeholders::_1, std::placeholders::_2);
    }
}

void TraditionalSmtpsFsm::handle_init_connect(SmtpsSession* session, const std::string& args) {
    session->async_write("220 SMTP Server Ready\r\n");
}

void TraditionalSmtpsFsm::handle_greeting_ehlo(SmtpsSession* session, const std::string& args) {
    // 处理EHLO命令
    if (args.empty()) {
        session->async_write("501 Syntax error in parameters or arguments\r\n");
        return;
    }

    // 发送支持的SMTP扩展
    std::string response = "250-" + args + " Hello\r\n"
                          "250-SIZE 10240000\r\n"  // 10MB 最大消息大小
                          "250-8BITMIME\r\n"
                          "250 SMTPUTF8\r\n";
    session->async_write(response);
}

void TraditionalSmtpsFsm::handle_wait_auth_auth(SmtpsSession* session, const std::string& args) {
    // 处理AUTH命令
    if (args.empty()) {
        session->async_write("501 Syntax error in parameters or arguments\r\n");
        return;
    }

    // 发送认证成功响应
    session->async_write("235 Authentication successful\r\n");
}

void TraditionalSmtpsFsm::handle_wait_ehlo_ehlo(SmtpsSession* session, const std::string& args) {
    // 与handle_greeting_ehlo相同
    handle_greeting_ehlo(session, args);
}

void TraditionalSmtpsFsm::handle_wait_mail_from_mail_from(SmtpsSession* session, const std::string& args) {
    // 解析MAIL FROM命令
    std::regex mail_from_regex(R"(FROM:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, mail_from_regex) && matches.size() > 1) {
        session->async_write("250 Ok\r\n");
    } else {
        session->async_write("501 Syntax error in parameters or arguments\r\n");
    }
}

void TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to(SmtpsSession* session, const std::string& args) {
    // 解析RCPT TO命令
    std::regex rcpt_to_regex(R"(TO:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, rcpt_to_regex) && matches.size() > 1) {
        session->async_write("250 Ok\r\n");
    } else {
        session->async_write("501 Syntax error in parameters or arguments\r\n");
    }
}

void TraditionalSmtpsFsm::handle_wait_data_data(SmtpsSession* session, const std::string& args) {
    if (!args.empty()) {
        session->async_write("501 Syntax error in parameters or arguments\r\n");
        return;
    }

    session->async_write("354 Start mail input; end with <CRLF>.<CRLF>\r\n");
}

void TraditionalSmtpsFsm::handle_in_message_data_end(SmtpsSession* session, const std::string& args) {
    session->async_write("250 Message accepted for delivery\r\n");
}

void TraditionalSmtpsFsm::handle_wait_quit_quit(SmtpsSession* session, const std::string& args) {
    if (!args.empty()) {
        session->async_write("501 Syntax error in parameters or arguments\r\n");
        return;
    }

    session->async_write("221 Bye\r\n");
    session->close();
}

void TraditionalSmtpsFsm::handle_error(SmtpsSession* session, const std::string& args) {
    session->async_write("500 Error: " + args + "\r\n");
}

} // namespace mail_system