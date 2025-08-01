#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include <iostream>

namespace mail_system {

TraditionalSmtpsFsm::TraditionalSmtpsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
                                           std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                                           std::shared_ptr<DBPool> db_pool)
    : SmtpsFsm(io_thread_pool, worker_thread_pool, db_pool) {
    init_transition_table();
    init_state_handlers();
}

void TraditionalSmtpsFsm::process_event(std::weak_ptr<SmtpsSession> s, SmtpsEvent event, const std::string& args) {
    std::cout << "enter process_event\n";
    auto session = s.lock();
    if (!session) {
        std::cerr << "Session is expired in process_event" << std::endl;
        return;
    }
    if(session->get_current_state() == SmtpsState::CLOSED) {
        session->close();
        return;
    }

    if(session->get_current_state() == SmtpsState::IN_MESSAGE && event == SmtpsEvent::DATA)
        std::cout << "in message with data\n";
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
                m_workerThreadPool->post([session, handler = event_handler_it->second, args]() {
                    // std::cout << "before handler\n";
                    handler(session, args);
                    // 更新会话状态
                    // session->set_current_state(next_state);
                });
            }
        }
        else {
            std::cout << "No handler for state " << get_state_name(session->get_current_state()) << " and event " << get_event_name(event) << std::endl;
            return;
        }
        
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
    transition_table_[std::make_pair(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO)] = SmtpsState::WAIT_AUTH;
    transition_table_[std::make_pair(SmtpsState::GREETING, SmtpsEvent::EHLO)] = SmtpsState::WAIT_AUTH;
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH)] = SmtpsState::WAIT_AUTH_USERNAME;
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH)] = SmtpsState::WAIT_AUTH_PASSWORD;
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH)] = SmtpsState::WAIT_MAIL_FROM;
    // 添加可选认证路径 - 允许直接从WAIT_AUTH状态转到WAIT_MAIL_FROM状态
    transition_table_[std::make_pair(SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;
    transition_table_[std::make_pair(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;
    transition_table_[std::make_pair(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO)] = SmtpsState::WAIT_DATA;
    transition_table_[std::make_pair(SmtpsState::WAIT_DATA, SmtpsEvent::DATA)] = SmtpsState::IN_MESSAGE;
    transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA)] = SmtpsState::IN_MESSAGE;
    transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END)] = SmtpsState::WAIT_QUIT;
    
    // QUIT命令可以在多个状态下接收
    for (int i = 0;i < 11; ++i)
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::QUIT)] = SmtpsState::CLOSED;
    
    for (int i = 0;i < 11; ++i) {
        // 错误处理
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::ERROR)] = static_cast<SmtpsState>(i);
        // 超时处理
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT)] = static_cast<SmtpsState>(i);
    }
}

void TraditionalSmtpsFsm::init_state_handlers() {
    // 初始化状态处理函数
    state_handlers_[SmtpsState::INIT][SmtpsEvent::CONNECT] = 
        std::bind(&TraditionalSmtpsFsm::handle_init_connect, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_EHLO][SmtpsEvent::EHLO] = 
        std::bind(&TraditionalSmtpsFsm::handle_greeting_ehlo, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::GREETING][SmtpsEvent::EHLO] = 
        std::bind(&TraditionalSmtpsFsm::handle_greeting_ehlo, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_AUTH][SmtpsEvent::AUTH] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_auth, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_AUTH_USERNAME][SmtpsEvent::AUTH] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_username, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::WAIT_AUTH_PASSWORD][SmtpsEvent::AUTH] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_password, this, std::placeholders::_1, std::placeholders::_2);
    
    // 添加可选认证路径的处理函数
    state_handlers_[SmtpsState::WAIT_AUTH][SmtpsEvent::MAIL_FROM] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_auth_mail_from, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_MAIL_FROM][SmtpsEvent::MAIL_FROM] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_mail_from_mail_from, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_RCPT_TO][SmtpsEvent::RCPT_TO] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::WAIT_DATA][SmtpsEvent::DATA] = 
        std::bind(&TraditionalSmtpsFsm::handle_wait_data_data, this, std::placeholders::_1, std::placeholders::_2);

    state_handlers_[SmtpsState::IN_MESSAGE][SmtpsEvent::DATA] = 
        std::bind(&TraditionalSmtpsFsm::handle_in_message_data, this, std::placeholders::_1, std::placeholders::_2);
    
    state_handlers_[SmtpsState::IN_MESSAGE][SmtpsEvent::DATA_END] = 
        std::bind(&TraditionalSmtpsFsm::handle_in_message_data_end, this, std::placeholders::_1, std::placeholders::_2);
    
    // 退出处理函数
    for (int i = 1; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
        state_handlers_[static_cast<SmtpsState>(i)][SmtpsEvent::ERROR] = 
            std::bind(&TraditionalSmtpsFsm::handle_wait_quit_quit, this, std::placeholders::_1, std::placeholders::_2);
    }
    
    // 错误处理函数
    for (int i = 0; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
        state_handlers_[static_cast<SmtpsState>(i)][SmtpsEvent::ERROR] = 
            std::bind(&TraditionalSmtpsFsm::handle_error, this, std::placeholders::_1, std::placeholders::_2);
    }
}

void TraditionalSmtpsFsm::handle_init_connect(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    std::cout << "handle_init_connect calling" << std::endl;
    if(auto s = session.lock())
    s->do_handshake([](std::weak_ptr<mail_system::SessionBase> session, const boost::system::error_code &ec){
        auto s = std::dynamic_pointer_cast<SmtpsSession>(session.lock());
        if (!s) {
            std::cerr << "Session is expired in handle_init_connect" << std::endl;
            return;
        }
        s->set_current_state(SmtpsState::GREETING);
        s->async_write("220 SMTPS Server\r\n", [s](const boost::system::error_code &e){
            if (e) {
                std::cerr << "An error occurred when sending greeting: " << e.message() << std::endl;
                return;
            }
            s->set_current_state(SmtpsState::WAIT_EHLO);
        });
    });
    else {
        std::cerr << "Session is expired in handle_init_connect" << std::endl;
        return;
    }
}

void TraditionalSmtpsFsm::handle_greeting_ehlo(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    auto s = session.lock();
    if(!s) {
        std::cerr << "Session is expired in handle_greeting_ehlo" << std::endl;
        return;
    }
    // 处理EHLO命令
    if (args.empty()) {
        s->async_write("501 Syntax error in parameters or arguments\r\n");
        return;
    }

    // 发送支持的SMTP扩展
    std::string response = "250-" + args + " Hello\r\n"
                          "250-SIZE 10240000\r\n"  // 10MB 最大消息大小
                          "250-8BITMIME\r\n"
                          "250 SMTPUTF8\r\n";
    s->async_write(response, [s](const boost::system::error_code &){
        s->set_current_state(SmtpsState::WAIT_AUTH);
    });
}

void TraditionalSmtpsFsm::handle_wait_auth_auth(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_auth_auth" << std::endl;
        return;
    }
    // 处理AUTH命令
    if (args.empty()) {
        s->async_write("501 Syntax error in parameters or arguments\r\n");
        return;
    }

    // 发送认证请求
    s->async_write("334 VXNlcm5hbWU6\r\n", [s](const boost::system::error_code &){
        s->set_current_state(SmtpsState::WAIT_AUTH_USERNAME);
    }); // "Username:" in base64
}

void TraditionalSmtpsFsm::handle_wait_auth_username(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    // 保存用户名
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_auth_username" << std::endl;
        return;
    }
    s->context_.client_username = args;
    s->async_write("334 UGFzc3dvcmQ6\r\n", [s](const boost::system::error_code &){
        s->set_current_state(SmtpsState::WAIT_AUTH_PASSWORD);
    }); // "Password:" in base64
}

void TraditionalSmtpsFsm::handle_wait_auth_password(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    // 验证用户名和密码
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_auth_password" << std::endl;
        return;
    }
    if (auth_user(s, s->context_.client_username, args)) {
        s->context_.is_authenticated = true;
        s->async_write("235 Authentication successful\r\n", [s](const boost::system::error_code &){
            s->set_current_state(SmtpsState::WAIT_MAIL_FROM);
    });
    } else {
        s->async_write("535 Authentication failed\r\n");
        handle_error(s, "Authentication failed");
    }
}

void TraditionalSmtpsFsm::handle_wait_auth_mail_from(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    // 检查是否需要强制认证（这里可以根据配置或其他条件来决定）
    bool require_auth = false; // 默认不强制认证
    
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_auth_mail_from" << std::endl;
        return;
    }
    // 如果需要强制认证但客户端未认证
    if (require_auth && !s->context_.is_authenticated) {
        // 发送认证要求
        s->async_write("530 Authentication required\r\n");
        return;
    }
    
    // 处理MAIL FROM命令，与handle_wait_mail_from_mail_from相同
    std::regex mail_from_regex(R"(FROM:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, mail_from_regex) && matches.size() > 1) {
        // 保存发件人地址
        s->context_.sender_address = matches[1];
        s->async_write("250 Ok\r\n", [s](const boost::system::error_code &){
            s->set_current_state(SmtpsState::WAIT_RCPT_TO);
    });
    } else {
        s->async_write("501 Syntax error in parameters or arguments\r\n");
    }
}

void TraditionalSmtpsFsm::handle_wait_mail_from_mail_from(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    // 解析MAIL FROM命令
    
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_auth_mail_from" << std::endl;
        return;
    }
    
    // 处理MAIL FROM命令，与handle_wait_mail_from_mail_from相同
    std::regex mail_from_regex(R"(FROM:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, mail_from_regex) && matches.size() > 1) {
        // 保存发件人地址
        s->context_.sender_address = matches[1];
        s->async_write("250 Ok\r\n", [s](const boost::system::error_code &){
            s->set_current_state(SmtpsState::WAIT_RCPT_TO);
    });
    } else {
        s->async_write("501 Syntax error in parameters or arguments\r\n");
    }
}

void TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_rcpt_to_rcpt_to" << std::endl;
        return;
    }
    // 解析RCPT TO命令
    std::regex rcpt_to_regex(R"(TO:\s*<([^>]*)>)", std::regex_constants::icase);
    std::smatch matches;
    if (std::regex_search(args, matches, rcpt_to_regex) && matches.size() > 1) {
        for(auto& match : matches)
            s->context_.recipient_addresses.push_back(match);
        s->async_write("250 Ok\r\n", [s](const boost::system::error_code &){
            s->set_current_state(SmtpsState::WAIT_DATA);
    });
    } else {
        s->async_write("501 Syntax error in parameters or arguments\r\n");
    }
}

void TraditionalSmtpsFsm::handle_wait_data_data(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_data_data" << std::endl;
        return;
    }

    if (!args.empty()) {
        s->async_write("501 Syntax error in parameters or arguments\r\n");
        return;
    }

    s->async_write("354 Start mail input; end with <CRLF>.<CRLF>\r\n", [s](const boost::system::error_code& ec){
        std::dynamic_pointer_cast<SmtpsSession>(s)->set_current_state(SmtpsState::IN_MESSAGE);
    });
}

void TraditionalSmtpsFsm::handle_in_message_data(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    std::cout << "keep receiving data" << std::endl;
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_in_message_data" << std::endl;
        return;
    }
    s->async_read();
}

void TraditionalSmtpsFsm::handle_in_message_data_end(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_in_message_data_end" << std::endl;
        return;
    }
    s->async_write("250 Message accepted for delivery\r\n", [s](const boost::system::error_code &){
        s->set_current_state(SmtpsState::WAIT_QUIT);
    });
}

void TraditionalSmtpsFsm::handle_wait_quit_quit(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_wait_quit_quit" << std::endl;
        return;
    }

    if (!args.empty()) {
        s->async_write("501 Syntax error in parameters or arguments\r\n", [s](const boost::system::error_code& ec){s->close();});
        return;
    }
    s->async_write("221 Bye\r\n", [session, this](const boost::system::error_code& ec){
        auto s = session.lock();
        if (!s) {
            std::cerr << "Session is expired in handle_wait_quit_quit" << std::endl;
            return;
        }
        mail *m = s->get_mail();
        s->m_server->m_workerThreadPool->post([this, m](){save_mail_data(m);});
        s->context_.clear(); // 清理上下文数据
        s->close();
    });
}

void TraditionalSmtpsFsm::handle_error(std::weak_ptr<SmtpsSession> session, const std::string& args) {
    auto s = session.lock();
    if (!s) {
        std::cerr << "Session is expired in handle_error" << std::endl;
        return;
    }
    s->stay_times++;
    if(s->stay_times > 3)
        s->close();
    else
    s->async_write("500 Error: " + args + "\r\n");
}

} // namespace mail_system