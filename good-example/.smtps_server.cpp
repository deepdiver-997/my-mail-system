#include "mail_system/back/mailServer/smtps_server.h"
#include <iostream>
#include <sstream>
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>

namespace mail_system {

SMTPSServer::SMTPSServer(const SMTPConfig& config, std::shared_ptr<DBPool> dbPool)
    : m_config(config)
    , m_dbPool(dbPool)
    , m_running(false)
    , m_sslContext(boost::asio::ssl::context::sslv23)
{
    // 初始化线程池
    m_ioThreadPool = std::make_shared<IOThreadPool>(m_config.io_thread_count);
    m_workerThreadPool = std::make_shared<BoostThreadPool>(m_config.worker_thread_count);
    
    // 获取IO上下文
    m_ioContext = m_ioThreadPool->get_io_context();
    
    // 配置SSL上下文
    m_sslContext.set_options(
        boost::asio::ssl::context::default_workarounds
        | boost::asio::ssl::context::no_sslv2
        | boost::asio::ssl::context::single_dh_use);
    m_sslContext.use_certificate_chain_file(m_config.certFile);
    m_sslContext.use_private_key_file(m_config.keyFile, boost::asio::ssl::context::pem);
    
    // 创建接收器
    m_acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(
        *m_ioContext,
        boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), m_config.port)
    );
    
    // 创建状态机
    if (m_config.useMSM) {
        m_stateMachine = std::make_unique<SMTPS::BoostMsmSmtpsFsm>();
    } else {
        m_stateMachine = std::make_unique<SMTPS::TraditionalSmtpsFsm>();
    }
    
    // 初始化命令处理器
    initCommandHandlers();
}

SMTPSServer::~SMTPSServer() {
    stop();
}

void SMTPSServer::start() {
    if (m_running) {
        return;
    }
    
    m_running = true;
    
    // 启动线程池
    m_ioThreadPool->start();
    m_workerThreadPool->start();
    
    // 启动监听线程
    m_listenerThread = std::thread(&SMTPSServer::listener_thread_func, this);
}

void SMTPSServer::stop() {
    if (!m_running) {
        return;
    }
    
    m_running = false;
    
    // 停止接收新连接
    if (m_acceptor) {
        boost::system::error_code ec;
        m_acceptor->close(ec);
    }
    
    // 等待监听线程结束
    if (m_listenerThread.joinable()) {
        m_listenerThread.join();
    }
    
    // 停止线程池
    m_workerThreadPool->stop(true);
    m_ioThreadPool->stop(true);
}

void SMTPSServer::listener_thread_func() {
    try {
        while (m_running) {
            accept_connection();
        }
    } catch (const std::exception& e) {
        if (m_running) {
            std::cerr << "Error in listener thread: " << e.what() << std::endl;
        }
    }
}

void SMTPSServer::accept_connection() {
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(*m_ioContext);
    auto ssl_socket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>>(*socket, m_sslContext);
    
    m_acceptor->async_accept(
        *socket,
        [this, socket, ssl_socket](const boost::system::error_code& error) {
            if (!error) {
                // 创建新的客户端会话
                auto session = std::make_shared<ClientSession>(std::move(ssl_socket));

                // 在工作线程池中处理客户端连接
                m_workerThreadPool->post([this, session]() mutable {
                    handle_client(*session.get());
                });
                
                // 继续接受新的连接
                if (m_running) {
                    accept_connection();
                }
            } else if (m_running) {
                std::cerr << "Accept error: " << error.message() << std::endl;
            }
        }
    );
}

void SMTPSServer::handle_client(ClientSession& session) {
    try {
        // 执行SSL握手
        session.socket->async_handshake(
            boost::asio::ssl::stream_base::server,
            [this, &session](const boost::system::error_code& error) {
                if (!error) {
                    // 发送欢迎消息
                    send_async_response(session, 220, "SMTP Server Ready");
                    
                    // 开始读取命令
                    auto buffer = std::make_shared<boost::asio::streambuf>();
                    read_command(session, buffer);
                } else {
                    std::cerr << "SSL handshake failed: " << error.message() << std::endl;
                }
            }
        );
    } catch (const std::exception& e) {
        std::cerr << "Error handling client: " << e.what() << std::endl;
    }
}

void SMTPSServer::read_command(ClientSession& session, std::shared_ptr<boost::asio::streambuf> buffer) {
    if (!m_running) return;
    
    boost::asio::async_read_until(
        *session.socket,
        *buffer,
        "\r\n",
        [this, &session, buffer](const boost::system::error_code& error, std::size_t bytes_transferred) {
            if (!error) {
                std::string command;
                std::istream is(buffer.get());
                std::getline(is, command);
                
                // 移除末尾的\r\n
                if (!command.empty() && command.back() == '\r') {
                    command.pop_back();
                }
                
                // 在工作线程池中处理命令
                m_workerThreadPool->post([this, &session, command]() {
                    parse_smtp_command(command, session);
                });
                
                // 继续读取下一个命令
                buffer->consume(bytes_transferred);
                read_command(session, buffer);
            } else if (error != boost::asio::error::eof) {
                std::cerr << "Read error: " << error.message() << std::endl;
            }
        }
    );
}

void SMTPSServer::send_async_response(const ClientSession& session, int code, const std::string& message) {
    if (!m_running) return;
    
    std::ostringstream response;
    response << code << " " << message << "\r\n";
    std::string response_str = response.str();
    
    boost::asio::async_write(
        *session.socket,
        boost::asio::buffer(response_str),
        [](const boost::system::error_code& error, std::size_t /*bytes_transferred*/) {
            if (error) {
                std::cerr << "Write error: " << error.message() << std::endl;
            }
        }
    );
}

void SMTPSServer::parse_smtp_command(const std::string& command, ClientSession& session) {
    // 解析命令和参数
    std::string cmd;
    std::string args;
    
    size_t space_pos = command.find(' ');
    if (space_pos != std::string::npos) {
        cmd = command.substr(0, space_pos);
        args = command.substr(space_pos + 1);
        // 去除参数前后的空格
        boost::trim(args);
    } else {
        cmd = command;
        args = "";
    }
    
    // 将命令转换为大写以进行比较
    boost::to_upper(cmd);
    
    // 确定SMTP事件
    SMTPS::SmtpsEvent event;
    if (cmd == "EHLO" || cmd == "HELO") {
        event = SMTPS::SmtpsEvent::EHLO;
    } else if (cmd == "MAIL") {
        event = SMTPS::SmtpsEvent::MAIL;
    } else if (cmd == "RCPT") {
        event = SMTPS::SmtpsEvent::RCPT;
    } else if (cmd == "DATA") {
        event = SMTPS::SmtpsEvent::DATA;
    } else if (cmd == "AUTH") {
        event = SMTPS::SmtpsEvent::AUTH;
    } else if (cmd == "STARTTLS") {
        event = SMTPS::SmtpsEvent::STARTTLS;
    } else if (cmd == "RSET") {
        event = SMTPS::SmtpsEvent::RSET;
    } else if (cmd == "NOOP") {
        event = SMTPS::SmtpsEvent::NOOP;
    } else if (cmd == "VRFY") {
        event = SMTPS::SmtpsEvent::VRFY;
    } else if (cmd == "QUIT") {
        event = SMTPS::SmtpsEvent::QUIT;
    } else {
        event = SMTPS::SmtpsEvent::UNKNOWN;
    }
    
    // 特殊处理DATA状态下的内容
    if (session.context.state == SMTPS::SmtpsState::DATA_CONTENT) {
        if (command == ".") {
            event = SMTPS::SmtpsEvent::DATA_END;
        } else {
            event = SMTPS::SmtpsEvent::DATA_LINE;
        }
    }
    
    // 使用状态机处理命令
    auto result = m_stateMachine->processEvent(event, args, session.context);

    // 根据状态机的结果发送响应
    send_async_response(session, result.first, result.second);
}

bool SMTPSServer::upgradeToTLS(ClientSession& session) {
    try {
        session.socket->async_handshake(
            boost::asio::ssl::stream_base::server,
            [this, &session](const boost::system::error_code& error) {
                if (!error) {
                    send_async_response(session, 220, "Ready to start TLS");
                } else {
                    send_async_response(session, 454, "TLS not available due to temporary reason");
                }
            }
        );
        return true;
    } catch (const std::exception& e) {
        std::cerr << "TLS upgrade error: " << e.what() << std::endl;
        return false;
    }
}

bool SMTPSServer::authenticateUser(const std::string& username, const std::string& password) {
    // 在工作线程池中执行认证
    auto future = m_workerThreadPool->submit([this, username, password]() {
        // TODO: 实现实际的用户认证逻辑
        return true;
    });
    
    return future.get();
}

void SMTPSServer::saveMailToDB(const ClientSession& session) {
    // 在工作线程池中执行数据库操作
    m_workerThreadPool->post([this, &session]() {
        try {
            // TODO: 实现实际的数据库保存逻辑
        } catch (const std::exception& e) {
            std::cerr << "Database error: " << e.what() << std::endl;
        }
    });
}

void SMTPSServer::resetSession(ClientSession& session) {
    session.context = SMTPS::SmtpsContext();
}

bool SMTPSServer::isValidEmailAddress(const std::string& email) {
    // TODO: 实现邮件地址验证逻辑
    return true;
}

void SMTPSServer::initCommandHandlers() {
    // TODO: 实现命令处理器初始化
}

} // namespace mail_system