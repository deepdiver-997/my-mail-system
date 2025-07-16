#include "mail_system/back/mailServer/pop3s/pop3s_session.h"
#include <iostream>
#include <boost/algorithm/string.hpp>

namespace mail_system {

Pop3sSession::Pop3sSession(
    boost::asio::io_context& io_context,
    boost::asio::ssl::context& ssl_context,
    std::shared_ptr<Pop3sFsmFactory> fsm_factory,
    std::shared_ptr<DBPool> db_pool
)
    : SessionBase(io_context, ssl_context),
      m_fsmFactory(fsm_factory),
      m_dbPool(db_pool),
      m_multilineResponse(false) {
    
    // 创建状态机
    m_fsm = m_fsmFactory->create_fsm();
}

Pop3sSession::~Pop3sSession() {
    // 确保会话关闭
    close();
}

void Pop3sSession::start() {
    // 调用基类的start方法
    SessionBase::start();
}

void Pop3sSession::handle_handshake(const boost::system::error_code& error) {
    if (!error) {
        try {
            std::cout << "POP3S session established with " << get_client_ip() << std::endl;
            
            // 发送欢迎消息
            send_welcome_message();
            
            // 开始读取数据
            read_async();
        }
        catch (const std::exception& e) {
            std::cerr << "Error in POP3S handshake: " << e.what() << std::endl;
            close();
        }
    }
    else {
        std::cerr << "POP3S handshake failed: " << error.message() << std::endl;
        close();
    }
}

void Pop3sSession::handle_read(const std::string& data) {
    try {
        // 去除行尾的\r\n
        std::string line = data;
        boost::algorithm::trim_right_if(line, boost::algorithm::is_any_of("\r\n"));
        
        // 处理命令
        process_command(line);
    }
    catch (const std::exception& e) {
        std::cerr << "Error handling POP3S data: " << e.what() << std::endl;
        send_response("-ERR Internal server error");
    }
}

void Pop3sSession::send_welcome_message() {
    // 发送POP3欢迎消息
    send_response("+OK POP3 server ready");
}

void Pop3sSession::process_command(const std::string& command) {
    try {
        std::cout << "POP3S command: " << command << std::endl;
        
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
        
        // 处理命令
        auto response = m_fsm->process_command(cmd, args);
        
        // 检查是否为多行响应
        if (response.find("\r\n") != std::string::npos) {
            m_multilineResponse = true;
        }
        
        send_response(response);
        
        // 如果是QUIT命令，关闭连接
        if (cmd == "QUIT") {
            close();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error processing POP3S command: " << e.what() << std::endl;
        send_response("-ERR Internal server error");
    }
}

void Pop3sSession::send_response(const std::string& response) {
    try {
        std::cout << "POP3S response: " << response << std::endl;
        
        if (m_multilineResponse) {
            // 多行响应，确保以\r\n.\r\n结尾
            write_async(response);
            m_multilineResponse = false;
        }
        else {
            // 单行响应，确保以\r\n结尾
            write_async(response + "\r\n");
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error sending POP3S response: " << e.what() << std::endl;
        close();
    }
}

} // namespace mail_system