#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/mailServer/server_base.h"
#include <iostream>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace mail_system {

SessionBase::SessionBase(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> &&socket, ServerBase* server)
    : m_socket(std::move(socket)), mail_(nullptr), usr_(nullptr), m_server(server), closed_(false), read_buffer_(4096) {
    // // 生成唯一的会话ID
    // boost::uuids::random_generator generator;
    // m_sessionId = to_string(generator());
    
    // // 初始化客户端IP为未知
    // m_clientIp = "unknown";
}
SessionBase::~SessionBase() {
    if(!closed_) {
        close();
    }
    std::cout << "SessionBase destructor called." << std::endl;
}

void SessionBase::do_handshake(std::function<void(std::weak_ptr<SessionBase> session, const boost::system::error_code&)> callback) {
    if(closed_) {
        return; // 已经关闭
    }
    auto self = shared_from_this(); // 确保在回调中可以安全地使用this
    int count = self.use_count();
    std::cout << "shared number in handshake: " << count << std::endl;
    if(closed_) {
        std::cout << "Session socket already closed in do_handshake." << std::endl;
        return; // 已经关闭
    }
    m_socket->async_handshake(boost::asio::ssl::stream_base::server,
        [self, callback](const boost::system::error_code& error) {
            if (self->closed_) {
                return; // 已经关闭
            }
            if (!error) {
                std::cout << "SSL handshake successful with " << self->get_client_ip() << std::endl;
                callback(self, error); // 调用回调函数
            } else {
                std::cerr << "SSL handshake failed: " << error.message() << std::endl;
                self->close();
            }
        });
}

void SessionBase::async_read(std::function<void(const boost::system::error_code&, std::size_t)> callback) {
    if(closed_) {
        return; // 已经关闭
    }
    auto self = shared_from_this(); // 确保在回调中可以安全地使用this
    // 读取数据
    m_socket->async_read_some(boost::asio::buffer(read_buffer_),
        [self, callback](const boost::system::error_code& error, size_t bytes_transferred) {
            if (!error) {
                if (self->closed_) {
                    return; // 已经关闭
                }
                if (callback) {
                    callback(error, bytes_transferred); // 调用回调函数
                    std::cout << "async reading " << bytes_transferred << " bytes with callback.\n";
                }
                else {
                    std::cout << "async reading " << bytes_transferred << " bytes without callback.\n";
                }
                // 处理读取的数据
                self->handle_read(std::string(self->read_buffer_.data(), bytes_transferred));
            } else {
                std::cerr << "Error reading data: " << error.message() << std::endl;
                self->handle_error(error);
            }
        });
}

void SessionBase::async_write(const std::string& data, std::function<void(const boost::system::error_code&)> callback) {
    if(closed_) {
        return; // 已经关闭
    }
    auto self = shared_from_this(); // 确保在回调中可以安全地使用this
    // 写入数据
    boost::asio::async_write(*m_socket, boost::asio::buffer(data),
        [self, callback, data](const boost::system::error_code& error, size_t bytes_transferred) {
            if (self->closed_) {
                return; // 已经关闭
            }
            if (!error) {
                if(callback) {
                    callback(error); // 调用回调函数
                    std::cout << "async writing " << data << " with callback.\n";
                }
                else {
                    std::cout << "async writing " << data << " without callback.\n";
                }
                // 写入成功，继续读取
                self->async_read();
            } else {
                std::cerr << "Error writing data: " << error.message() << std::endl;
                self->handle_error(error);
            }
        });
}

std::string SessionBase::get_client_ip() const {
    if (!client_address_.empty()) {
        return client_address_;
    }
    if (closed_) {
        throw std::runtime_error("Session closed"); // 已经关闭
    }
    try {
        auto endpoint = m_socket->lowest_layer().remote_endpoint();
        client_address_ = endpoint.address().to_string();
    }
    catch (const std::exception& e) {
        std::cerr << "Error getting client IP: " << e.what() << std::endl;
        client_address_ = "unknown";
    }
    return client_address_;
}

boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& SessionBase::get_ssl_socket() {
    return *m_socket;
}

void SessionBase::set_server(ServerBase* server) {
    m_server = server;
}

// ServerBase* SessionBase::get_server() {
//     return m_server;
// }

// std::string SessionBase::get_session_id() const {
//     return m_sessionId;
// }

mail* SessionBase::get_mail() {
    return mail_.release();
}

void SessionBase::handle_error(const boost::system::error_code& error) {
    std::cerr << "SessionBase Error: " << error.message() << std::endl;
    close();
}

void SessionBase::close() {
    if(closed_) {
        return;
    }
    closed_ = true;
    try {
        boost::system::error_code ec;
        // 只在socket有效且未关闭时访问remote_endpoint
        if (m_socket && m_socket->lowest_layer().is_open()) {
            // try {
            //     std::cout << "Closing session for " << m_socket->lowest_layer().remote_endpoint() << std::endl;
            // } catch (const std::exception& e) {
            //     std::cout << "Closing session for unknown endpoint." << std::endl;
            // }
            // 可选：取消所有异步操作
            m_socket->lowest_layer().cancel(ec);
            // 关闭SSL连接
            m_socket->shutdown(ec);
            // 关闭套接字
            m_socket->lowest_layer().close(ec);
            std::cout << "Session closed." << std::endl;
        } else {
            std::cout << "Session already closed or socket not open." << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error closing session: " << e.what() << std::endl;
    }
}

bool SessionBase::is_closed() const {
    return closed_;
}

} // namespace mail_system