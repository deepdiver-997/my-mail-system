#include "mail_system/back/mailServer/server_base.h"
#include <iostream>
#include <fstream>

namespace mail_system {

ServerBase::ServerBase(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<DBPool> dbPool) 
    : m_sslContext(boost::asio::ssl::context::sslv23),
      m_endpoint(boost::asio::ip::make_address(config.address), config.port),
      m_state(ServerState::Stopped),
      ssl_in_worker(config.ssl_in_worker),
      m_ioThreadPool(ioThreadPool),
      m_workerThreadPool(wokerThreadPool),
      m_dbPool(dbPool),
      has_listener_thread(false) {
try {
        if(config.io_thread_count > 0 && m_ioThreadPool == nullptr) {
            m_ioThreadPool = std::make_shared<IOThreadPool>(config.io_thread_count);
            m_ioThreadPool->start();
            std::cout << "IOThreadPools started in function ServerBase::ServerBase" << std::endl;
        }
        
        if(config.worker_thread_count > 0 && m_workerThreadPool == nullptr) {
            m_workerThreadPool = std::make_shared<BoostThreadPool>(config.worker_thread_count);
            m_workerThreadPool->start();
            std::cout << "WorkerThreadPools started in function ServerBase::ServerBase" << std::endl;
        }

        if (config.db_pool_config.achieve == "mysql" && m_dbPool == nullptr) {
            m_dbPool = MySQLPoolFactory::get_instance().create_pool(config.db_pool_config, std::make_shared<MySQLService>());
            std::cout << "dbPool created in function ServerBase::ServerBase" << std::endl;
        } else {
            m_dbPool = nullptr;
        }
    
        m_ioContext = std::make_shared<boost::asio::io_context>();
        m_acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(*m_ioContext);
        m_workGuard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(m_ioContext->get_executor());
        // 配置SSL上下文
        m_sslContext.set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use
        );

        // 加载证书
        load_certificates(config.certFile, config.keyFile, config.dhFile);

        // 打开接受器
        m_acceptor->open(m_endpoint.protocol());
        
        // 设置地址重用选项
        m_acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        
        // 绑定端点
        m_acceptor->bind(m_endpoint);

        // 开始监听
        m_acceptor->listen();

        std::cout << "Server initialized on " << config.address << ":" << config.port << std::endl;

        m_state = ServerState::Paused;
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
    std::cout << "Waiting for new connection..." << std::endl;
    // 创建新的TCP socket和SSL流
    auto socket = std::make_unique<boost::asio::ip::tcp::socket>(std::static_pointer_cast<IOThreadPool>(m_ioThreadPool)->get_io_context());
    auto ssl_socket = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(*socket), m_sslContext);
    // 接受连接
    m_acceptor->async_accept(
        ssl_socket->next_layer(),
        [this, ssl_socket = std::move(ssl_socket)](const boost::system::error_code& ec) mutable {
            if (!ec) {
                std::cout << "New connection accepted" << std::endl;
                std::cout << "Start handling connection" << std::endl;
                // 会话已建立
                handle_accept(std::move(ssl_socket), ec);
                // std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            else {
                std::cerr << "Error accepting connection: " << ec.message() << std::endl;
            }
            
            // 继续接受连接
            if(m_state.load() == ServerState::Running)
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
                // boost::asio::async_write(
                //     s->get_ssl_socket(),
                //     boost::asio::buffer(response),
                //     [s](const boost::system::error_code& ec, std::size_t /*length*/) {
                //         if (ec) {
                //             std::cerr << "Error sending response: " << ec.message() << std::endl;
                //         }
                //         else {
                //             std::cout << "Response sent successfully" << std::endl;
                //         }
                //     }
                // );
            }
            catch (const std::exception& e) {
                std::cerr << "Error sending response: " << e.what() << std::endl;
            }
        }
    });
}

void ServerBase::start() {
    if(m_state.load() == ServerState::Running) {
        return;
    }
    if (m_state.load() != ServerState::Stopped) {
        m_state.store(ServerState::Running);
    if (!m_acceptor->is_open()) {
        m_acceptor->open(m_acceptor->local_endpoint().protocol());
        m_acceptor->listen();
    }
        
        try {
            // 开始接受连接
            if(has_listener_thread == false) {
                m_listenerThread = std::thread([this]() {
                    // 异步接受连接
                    accept_connection();
                    m_ioContext->run();
                });
                has_listener_thread = true;
                m_listenerThread.detach();
            }
            else {
                accept_connection();
            }
            std::cout << "Server started" << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Error starting server: " << e.what() << std::endl;
            stop();
        }
    }
}

void ServerBase::stop(ServerState next_state) {

    if(next_state == ServerState::Running) {
        std::cout << "Can't switch to running state in function ServerBase::stop" << std::endl;
        return;
    }

    if (m_state.load() == ServerState::Running) {
        try {
            m_state.store(next_state);
            // boost::asio::post(*m_ioContext, [this]() {
            //     m_acceptor->close();
            //     if(m_workGuard->owns_work() && m_state == ServerState::Stopped)
            //         m_workGuard->reset();
            // });
            if(m_listenerThread.joinable())
                m_listenerThread.join();
            std::cout << "Listener thread stopped in function ServerBase::stop" << std::endl;
            if(m_ioThreadPool)
                m_ioThreadPool->stop();
            if(m_workerThreadPool)
                m_workerThreadPool->stop();
            std::cout << "ThreadPools stopped in function ServerBase::stop" << std::endl;

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

ServerState ServerBase::get_state() const {
    return m_state.load();
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

void ServerBase::load_certificates(const std::string& cert_file, const std::string& key_file, const std::string& dh_file) {
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
        if(dh_file != "") {
            if (!std::ifstream(dh_file.c_str()).good()) {
                throw std::runtime_error("DH file not found: " + dh_file);
            }
            m_sslContext.use_tmp_dh_file(dh_file);
        }

        std::cout << "SSL certificates loaded successfully" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error loading SSL certificates: " << e.what() << std::endl;
        throw;
    }
}

} // namespace mail_system