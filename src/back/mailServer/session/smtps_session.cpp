#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.h"
#include <iostream>
#include <boost/algorithm/string.hpp>

namespace mail_system {

SmtpsSession::SmtpsSession(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> &&socket, std::shared_ptr<SmtpsFsm> fsm)
    : SessionBase(std::move(socket)), current_state_(SmtpsState::INIT), m_fsm(fsm), m_receivingData(false) {}

SmtpsSession::~SmtpsSession() {
    // 确保会话关闭
    close();
}

void SmtpsSession::start() {
    // 执行SSL握手
    do_handshake();
    //启动状态机
    async_read();
}

void SmtpsSession::handle_read(const std::string& data) {
    try {
        // 去除行尾的\r\n
        std::string line = data;
        boost::algorithm::trim_right_if(line, boost::algorithm::is_any_of("\r\n"));
        
        if (m_receivingData) {
            // 检查是否为数据结束标记
            if (line == ".") {
                m_receivingData = false;
                
                // 处理邮件数据结束事件
                m_fsm->process_event(this, SmtpsEvent::DATA_END, nullptr);
                m_mailData.clear();
                
                // 发送响应
                async_write("250 OK\r\n");
            }
            else {
                // 如果行以.开头，去掉一个.（SMTP协议规定）
                if (!line.empty() && line[0] == '.') {
                    line = line.substr(1);
                }
                mail_->header = line.substr(0, line.find("\n\n"));
                mail_->body = line.substr(line.find("\n\n") + 2);
                
                // 处理数据事件（可选，取决于状态机是否需要处理每一行数据）
                m_fsm->process_event(this, SmtpsEvent::DATA, nullptr);
            }
        }
        else {
            // 处理命令
            process_command(line);
        }
        // 继续异步读取
        async_read();
    }
    catch (const std::exception& e) {
        std::cerr << "Error handling SMTPS data: " << e.what() << std::endl;
        m_fsm->process_event(this, SmtpsEvent::ERROR, e.what());
        handle_error(boost::system::error_code(boost::system::errc::io_error, boost::system::generic_category()));
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
        } else if (cmd == "QUIT") {
            event = SmtpsEvent::QUIT;
        } else {
            // 未知命令
            event = SmtpsEvent::ERROR;
            args = "Unknown command: " + cmd;
        }
        
        // 处理事件
        m_fsm->process_event(this, event, args);
    }
    catch (const std::exception& e) {
        std::cerr << "Error processing SMTPS command: " << e.what() << std::endl;
        // 处理错误事件
    }
}

} // namespace mail_system