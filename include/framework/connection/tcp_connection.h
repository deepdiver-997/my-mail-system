#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include "i_connection.h"
#include <boost/asio.hpp>
#include <memory>

namespace mail_system {

// TCP 连接实现
class TcpConnection : public IConnection {
public:
    explicit TcpConnection(std::unique_ptr<boost::asio::ip::tcp::socket> socket)
        : socket_(std::move(socket)) {}

    ~TcpConnection() override = default;

    // 异步读取
    void async_read(
        boost::asio::mutable_buffer buffer,
        ReadHandler handler
    ) override {
        socket_->async_read_some(buffer, handler);
    }

    // 获取 executor
    boost::asio::any_io_executor get_executor() override {
        return socket_->get_executor();
    }

    // 异步写入
    void async_write(
        boost::asio::const_buffer buffer,
        WriteHandler handler
    ) override {
        boost::asio::async_write(*socket_, buffer, handler);
    }

    // TCP 连接无需握手，直接调用成功回调
    void async_handshake(
        boost::asio::ssl::stream_base::handshake_type,
        HandshakeHandler handler
    ) override {
        handler(boost::system::error_code());
    }

    // 关闭连接
    void close() override {
        boost::system::error_code ec;
        socket_->close(ec);
    }

    // 检查连接是否打开
    bool is_open() const override {
        return socket_->is_open();
    }

    // 获取本地端口
    uint16_t get_local_port() const override {
        boost::system::error_code ec;
        return socket_->local_endpoint(ec).port();
    }

    // 获取远程 IP 地址
    std::string get_remote_ip() const override {
        boost::system::error_code ec;
        return socket_->remote_endpoint(ec).address().to_string();
    }

    // 释放原始 socket
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() override {
        return std::move(socket_);
    }

private:
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
};

} // namespace mail_system

#endif // TCP_CONNECTION_H
