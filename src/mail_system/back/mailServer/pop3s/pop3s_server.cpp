#include "mail_system/back/mailServer/pop3s/pop3s_server.h"
#include "mail_system/back/mailServer/pop3s/pop3s_session.h"
#include <iostream>

namespace mail_system {

Pop3sServer::Pop3sServer(
    boost::asio::io_context& io_context,
    const std::string& address,
    unsigned short port,
    const std::string& cert_file,
    const std::string& key_file,
    std::shared_ptr<DBPool> db_pool,
    std::shared_ptr<Pop3sFsmFactory> fsm_factory
)
    : ServerBase(io_context, address, port, cert_file, key_file, db_pool),
      m_fsmFactory(fsm_factory) {
    
    std::cout << "POP3S server starting on " << address << ":" << port << std::endl;
}

Pop3sServer::~Pop3sServer() {
    // 确保服务器停止
    stop();
}

void Pop3sServer::create_session() {
    try {
        // 创建新的POP3S会话
        auto session = std::make_shared<Pop3sSession>(
            get_io_context(),
            get_ssl_context(),
            m_fsmFactory,
            get_db_pool()
        );

        // 开始接受新连接
        get_acceptor().async_accept(
            session->socket().lowest_layer(),
            [this, session](const boost::system::error_code& error) {
                handle_accept(session, error);
            }
        );
    }
    catch (const std::exception& e) {
        std::cerr << "Error creating POP3S session: " << e.what() << std::endl;
    }
}

void Pop3sServer::handle_accept(std::shared_ptr<SessionBase> session, const boost::system::error_code& error) {
    if (!error) {
        try {
            std::cout << "New POP3S connection from " << session->get_client_ip() << std::endl;
            
            // 启动会话
            session->start();
        }
        catch (const std::exception& e) {
            std::cerr << "Error starting POP3S session: " << e.what() << std::endl;
        }
    }
    else {
        std::cerr << "POP3S accept error: " << error.message() << std::endl;
    }

    // 继续接受新连接
    create_session();
}

} // namespace mail_system