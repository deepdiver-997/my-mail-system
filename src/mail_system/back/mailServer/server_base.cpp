#include "mail_system/back/mailServer/server_base.h"
#include <iostream>
#include <fstream>

namespace mail_system {

ServerBase::ServerBase(const ServerConfig& config)
    : m_sslContext(boost::asio::ssl::context::sslv23),
      m_running(false) {
try {
        m_ioThreadPool = std::make_shared<ThreadPoolBase>(config.io_thread_count);
        m_workerThreadPool = std::make_shared<ThreadPoolBase>(config.worker_thread_count);
        m_dbPool = std::make_shared<DBPool>(config.db_connection_string, config.db_pool_size);
    
        m_ioContext = std::make_shared<boost::asio::io_context>();
        m_acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(*m_ioContext);
        // 配置SSL上下文
        m_sslContext.set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use
        );

        // 加载证书
        load_certificates(config.certFile, config.keyFile);

        // 创建端点
        boost::asio::ip::tcp::endpoint endpoint(
            boost::asio::ip::make_address(config.address),
            config.port
        );

        // 打开接受器
        m_acceptor->open(endpoint.protocol());
        
        // 设置地址重用选项
        m_acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        
        // 绑定端点
        m_acceptor->bind(endpoint);

        // 开始监听
        m_acceptor->listen();

        std::cout << "Server initialized on " << config.address << ":" << config.port << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error initializing server: " << e.what() << std::endl;
        throw;
    }
}

ServerBase::~ServerBase() {
    stop();
}

void ServerBase::accept_connection() {
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(*m_ioContext);
    auto ssl_socket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>>(*socket, m_sslContext);
    // 接受连接
    m_acceptor->async_accept(
        *socket,
        [this, socket, ssl_socket](const boost::system::error_code& ec) {
            if (!ec) {
                // 会话已建立
                auto session = std::make_shared<SessionBase>(ssl_socket);
                handle_accept(session, ec);
            }
            else {
                std::cerr << "Error accepting connection: " << ec.message() << std::endl;
            }
            
            // 继续接受连接
            accept_connection();
        }
    );
}

void ServerBase::send_async_response(std::weak_ptr<SessionBase> session, const std::string& response) {
    // 异步发送响应
    m_ioThreadPool->post([session, response]() {
        if (auto s = session.lock()) {
            try {
                // 发送响应
                boost::asio::async_write(
                    s->get_ssl_socket(),
                    boost::asio::buffer(response),
                    [s](const boost::system::error_code& ec, std::size_t /*length*/) {
                        if (ec) {
                            std::cerr << "Error sending response: " << ec.message() << std::endl;
                        }
                        else {
                            std::cout << "Response sent successfully" << std::endl;
                        }
                    }
                );
            }
            catch (const std::exception& e) {
                std::cerr << "Error sending response: " << e.what() << std::endl;
            }
        }
    });
}

void ServerBase::start() {
    if (!m_running) {
        m_running = true;
        
        try {
            // 开始接受连接
            m_listenerThread = std::thread([this]() {
                while (m_running) {
                    // 创建新会话
                    accept_connection();
                }
            });
            
            std::cout << "Server started" << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Error starting server: " << e.what() << std::endl;
            stop();
        }
    }
}

void ServerBase::stop() {
    if (m_running) {
        try {
            m_running = false;
            
            // 关闭接受器
            boost::system::error_code ec;
            m_acceptor->close(ec);
            if (ec) {
                std::cerr << "Error closing acceptor: " << ec.message() << std::endl;
            }
            
            std::cout << "Server stopped" << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Error stopping server: " << e.what() << std::endl;
        }
    }
}

bool ServerBase::is_running() const {
    return m_running;
}

std::shared_ptr<boost::asio::io_context> ServerBase::get_io_context() {
    return m_ioContext;
}

boost::asio::ssl::context& ServerBase::get_ssl_context() {
    return m_sslContext;
}

std::shared_ptr<boost::asio::ip::tcp::acceptor> ServerBase::get_acceptor() {
    return m_acceptor;
}

void ServerBase::load_certificates(const std::string& cert_file, const std::string& key_file) {
    try {
        // 检查证书文件是否存在
        if (!std::ifstream(cert_file.c_str()).good()) {
            throw std::runtime_error("Certificate file not found: " + cert_file);
        }
        if (!std::ifstream(key_file.c_str()).good()) {
            throw std::runtime_error("Private key file not found: " + key_file);
        }

        // 加载证书
        m_sslContext.use_certificate_chain_file(cert_file);
        
        // 加载私钥
        m_sslContext.use_private_key_file(key_file, boost::asio::ssl::context::pem);
        
        // 验证私钥
        m_sslContext.use_tmp_dh_file(cert_file);

        std::cout << "SSL certificates loaded successfully" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error loading SSL certificates: " << e.what() << std::endl;
        throw;
    }
}

} // namespace mail_system