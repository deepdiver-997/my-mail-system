#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/mailServer/server_base.h"
#include <iostream>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace mail_system {

SessionBase::SessionBase(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> &&socket)
    : m_socket(std::move(socket)), mail_(nullptr), usr_(nullptr) {
    // // 生成唯一的会话ID
    // boost::uuids::random_generator generator;
    // m_sessionId = to_string(generator());
    
    // // 初始化客户端IP为未知
    // m_clientIp = "unknown";
}
SessionBase::~SessionBase() {
    close();
}

void SessionBase::do_handshake() {
    m_socket->async_handshake(boost::asio::ssl::stream_base::server,
        [this](const boost::system::error_code& error) {
            if (!error) {
                std::cout << "SSL handshake successful with " << get_client_ip() << std::endl;
            } else {
                std::cerr << "SSL handshake failed: " << error.message() << std::endl;
                close();
            }
        });
}

void SessionBase::async_read() {
    // 读取数据
    m_socket->async_read_some(boost::asio::buffer(read_buffer_),
        [this](const boost::system::error_code& error, size_t bytes_transferred) {
            if (!error) {
                // 处理读取的数据
                handle_read(std::string(read_buffer_.data(), bytes_transferred));
            } else {
                handle_error(error);
            }
        });
}

void SessionBase::async_write(const std::string& data) {
    // 写入数据
    boost::asio::async_write(*m_socket, boost::asio::buffer(data),
        [this](const boost::system::error_code& error, size_t bytes_transferred) {
            if (!error) {
                // 写入成功，继续读取
                async_read();
            } else {
                handle_error(error);
            }
        });
}

std::string SessionBase::get_client_ip() const {
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

// std::string SessionBase::get_session_id() const {
//     return m_sessionId;
// }

void SessionBase::close() {
    try {
        boost::system::error_code ec;
        
        // 关闭SSL连接
        m_socket->shutdown(ec);
        if (ec) {
            std::cerr << "Error shutting down SSL: " << ec.message() << std::endl;
        }
        
        // 关闭套接字
        m_socket->lowest_layer().close(ec);
        if (ec) {
            std::cerr << "Error closing socket: " << ec.message() << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error closing session: " << e.what() << std::endl;
    }
}


} // namespace mail_system