#include "mail_system/back/mailServer/smtps_server.h"
#include <iostream>

namespace mail_system {

SmtpsServer::SmtpsServer(const ServerConfig& config) :ServerBase(config){
    m_fsm = std::make_shared<SmtpsFsm>();
}

SmtpsServer::~SmtpsServer() {
    // 确保服务器停止
    stop();
}

void SmtpsServer::handle_accept(std::unique_ptr<SmtpsSession> session, const boost::system::error_code& error) {
    if (!error) {
        try {
            std::cout << "New SMTPS connection from " << session->get_client_ip() << std::endl;
            
            // 启动会话
            session->start();
        }
        catch (const std::exception& e) {
            std::cerr << "Error starting SMTPS session: " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "SMTPS accept error: " << error.message() << std::endl;
    }

}

} // namespace mail_system