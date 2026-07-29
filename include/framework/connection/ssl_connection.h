#ifndef SSL_CONNECTION_H
#define SSL_CONNECTION_H

#include "i_connection.h"
#include <boost/asio/ssl.hpp>
#include <memory>

namespace mail_system {

// SSL 连接实现
class SslConnection : public IConnection {
public:
    SslConnection(
        std::unique_ptr<boost::asio::ip::tcp::socket> socket,
        boost::asio::ssl::context& ssl_context
    ) : stream_(
          std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
              std::move(*socket), ssl_context)) {}

    SslConnection(
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket>&& stream
    ) : stream_(std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(stream))) {}

    SslConnection(
        std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream
    ) : stream_(std::move(stream)) {}
    
    ~SslConnection() override = default;

    // 异步读取
    void async_read(
        boost::asio::mutable_buffer buffer,
        ReadHandler handler
    ) override {
        stream_->async_read_some(buffer, handler);
    }

    // 获取 executor
    boost::asio::any_io_executor get_executor() override {
        return stream_->get_executor();
    }

    // 异步写入
    void async_write(
        boost::asio::const_buffer buffer,
        WriteHandler handler
    ) override {
        boost::asio::async_write(*stream_, buffer, handler);
    }

    // SSL 握手
    void async_handshake(
        boost::asio::ssl::stream_base::handshake_type type,
        HandshakeHandler handler
    ) override {
        stream_->async_handshake(type, handler);
    }

    // 关闭连接
    void close() override {
        boost::system::error_code ec;
        stream_->shutdown(ec);
        stream_->lowest_layer().close(ec);
    }

    // 检查连接是否打开
    bool is_open() const override {
        return stream_->lowest_layer().is_open();
    }

    // 获取本地端口
    uint16_t get_local_port() const override {
        boost::system::error_code ec;
        return stream_->lowest_layer().local_endpoint(ec).port();
    }

    // 获取远程 IP 地址
    std::string get_remote_ip() const override {
        boost::system::error_code ec;
        return stream_->lowest_layer().remote_endpoint(ec).address().to_string();
    }

    // 释放原始 socket（从 SSL stream 中提取）
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() override {
        return std::make_unique<boost::asio::ip::tcp::socket>(
            std::move(stream_->next_layer())
        );
    }

private:
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream_;
};

} // namespace mail_system

#endif // SSL_CONNECTION_H
