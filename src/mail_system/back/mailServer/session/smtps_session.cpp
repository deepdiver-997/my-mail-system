#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.h"
#include <iostream>
#include <boost/algorithm/string.hpp>

namespace mail_system {

SmtpsSession::SmtpsSession(ServerBase* server, std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> &&socket, std::shared_ptr<SmtpsFsm> fsm)
    : SessionBase(std::move(socket), server), current_state_(SmtpsState::INIT), m_fsm(fsm), m_receivingData(false), stay_times(0) {
    if (!m_fsm) {
        throw std::invalid_argument("SmtpsSession: FSM cannot be null");
    }
}

SmtpsSession::~SmtpsSession() {
    // 确保会话关闭
    close();
}

void SmtpsSession::start() {
    // std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    std::cout << "SMTPS Session started" << std::endl;
    if(m_socket == nullptr) {
        std::cerr << "SMTPS Session socket is null in start." << std::endl;
        return; // 确保socket已初始化
    }
    if(!m_socket->lowest_layer().is_open()) {
        std::cerr << "SMTPS Session socket is not open in start." << std::endl;
        return; // 确保socket已打开
    }
    if(closed_) {
        std::cout << "Session already closed in SmtpsSession::start." << std::endl;
        return; // 已经关闭
    }
    // std::cout << "ready to call process event\n";
    // m_fsm->process_event(std::dynamic_pointer_cast<SmtpsSession>(shared_from_this()), SmtpsEvent::CONNECT, nullptr);
    // std::cout << "start event CONNECT called in SmtpsSession::start" << std::endl;
    auto self = shared_from_this();
    // 执行SSL握手
    do_handshake([self](std::weak_ptr<SessionBase> session, const boost::system::error_code& error) {
        if(error)
            return;
        // if (auto s = std::dynamic_pointer_cast<SmtpsSession>(session.lock())) {
        //     s->m_fsm->process_event(s, SmtpsEvent::CONNECT, nullptr);
        // }
        self->async_write("220 SMTPS Server\r\n", [self](const boost::system::error_code& error) {
            if (error) {
                std::cerr << "Error in SmtpsSession::start: " << error.message() << std::endl;
                return;
            }
            std::dynamic_pointer_cast<SmtpsSession>(self)->set_current_state(SmtpsState::WAIT_DATA);
            std::cout << "220 hello callback end\n";
        });
        std::cout << "handshake callback end\n";
    });
    std::cout << "async handshake called in SmtpsSession::start" << std::endl;
}

void SmtpsSession::handle_read(const std::string& data) {
    try {
        // 对象有效性检查
        if (this == nullptr) {
            std::cerr << "CRITICAL: Invalid this pointer in handle_read" << std::endl;
            return;
        }
        try {
            auto self = shared_from_this(); // 验证shared_ptr有效性
        } catch (const std::bad_weak_ptr& e) {
            std::cerr << "CRITICAL: Invalid shared_from_this(): " << e.what() << std::endl;
            return;
        }
        if (!m_fsm) {
            std::cerr << "CRITICAL: m_fsm is null in handle_read" << std::endl;
            return;
        }
        std::cout << "enter handle_read in SmtpsSession" << std::endl;
        auto self = shared_from_this();
        // 去除行尾的\r\n
        std::string line = data;
        boost::algorithm::trim_right_if(line, boost::algorithm::is_any_of("\r\n"));
        
        if (m_receivingData) {
            std::cout << "enter handle_read in SmtpsSession, m_receivingData is true" << std::endl;
            // 检查是否为数据结束标记
            if (line == ".") {
                m_receivingData = false;
                std::cout << "data end" << std::endl;
                
                // 处理邮件数据结束事件
                m_fsm->process_event(std::dynamic_pointer_cast<SmtpsSession>(self), SmtpsEvent::DATA_END, std::string());
                // self->async_write("250 OK\r\n", [self](const boost::system::error_code& error) {
                //     if (error) {
                //         std::cerr << "Error: " << error.message() << std::endl;
                //         return;
                //     }
                //     std::cout << "process data end up" << std::endl;
                //     std::dynamic_pointer_cast<SmtpsSession>(self)->set_current_state(SmtpsState::WAIT_QUIT);
                //     self->async_read();
                // });
                std::cout << "process data end up" << std::endl;
                
                return;
            }
            else {
                // 如果行以.开头，去掉一个.（SMTP协议规定）
                if (!line.empty() && line[0] == '.') {
                    line = line.substr(1);
                }
                // if (mail_ == nullptr) {
                //     mail_ = std::make_unique<mail>();
                // }
                // mail_->header = line.substr(0, line.find("\n\n"));
                // mail_->body = line.substr(line.find("\n\n") + 2);
                std::cout << "line(" << line.length() << "): " << line << std::endl;

                std::cout << "ready to enter process event IN_MESSAGE DATA" << std::endl << self.use_count() << std::endl;
                
                // 处理数据事件（可选，取决于状态机是否需要处理每一行数据）
                // // 调试信息
                std::cout << "[DEBUG] Object address: " << this << std::endl;
                std::cout << "[DEBUG] FSM vtable: " << typeid(*m_fsm).name() << std::endl;
                // 调用栈回溯
                std::cout << "[DEBUG] Call stack trace: " << std::endl;
                // 实际调用
                m_fsm->process_event(std::dynamic_pointer_cast<SmtpsSession>(self), SmtpsEvent::DATA, std::string());
                // self->async_read();
                std::cout << "process event IN_MESSAGE DATA end\n";
            }
        }
        else {
            // 处理命令
            process_command(line);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error handling SMTPS data: " << e.what() << std::endl;
        m_fsm->process_event(std::dynamic_pointer_cast<SmtpsSession>(shared_from_this()), SmtpsEvent::ERROR, e.what());
        // handle_error(boost::system::error_code(boost::system::errc::io_error, boost::system::generic_category()));
    }
}

void SmtpsSession::process_command(const std::string& command) {
    try {
        std::cout << "SMTPS command: " << command << std::endl;
        
        // 提取命令和参数
        std::string cmd;
        std::string args;
        
        size_t space_pos = command.find(' ');
        if (space_pos != std::string::npos) {
            cmd = command.substr(0, space_pos);
            args = command.substr(space_pos + 1);
        }
        else {
            cmd = command;
            args = "";
        }

        if(current_state_ == SmtpsState::WAIT_AUTH_USERNAME || current_state_ == SmtpsState::WAIT_AUTH_PASSWORD) {
            m_fsm->process_event(std::dynamic_pointer_cast<SmtpsSession>(shared_from_this()), SmtpsEvent::AUTH, cmd);
            return;
        }
        
        // 转换命令为大写
        boost::algorithm::to_upper(cmd);
        
        // 将命令映射到事件
        SmtpsEvent event;
        if (cmd == "EHLO" || cmd == "HELO") {
            event = SmtpsEvent::EHLO;
        }
        else if (cmd == "AUTH") {
            event = SmtpsEvent::AUTH;
        } else if (cmd == "MAIL") {
            event = SmtpsEvent::MAIL_FROM;
        } else if (cmd == "RCPT") {
            event = SmtpsEvent::RCPT_TO;
        } else if (cmd == "DATA") {
            event = SmtpsEvent::DATA;
            m_receivingData = true;
        } else if (cmd == "QUIT") {
            event = SmtpsEvent::QUIT;
            // 强制关闭会话
            async_write("221 Bye\r\n", [this](const boost::system::error_code& error) {
                if (!error) {
                    close();
                }
            });
            return;
        } else {
            // 未知命令
            event = SmtpsEvent::ERROR;
            args = "Unknown command: " + cmd;
        }
        
        // 处理事件
        m_fsm->process_event(std::dynamic_pointer_cast<SmtpsSession>(shared_from_this()), event, args);
    }
    catch (const std::exception& e) {
        std::cerr << "Error processing SMTPS command: " << e.what() << std::endl;
        // 处理错误事件
    }
}

} // namespace mail_system