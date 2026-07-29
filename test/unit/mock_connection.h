#ifndef MOCK_CONNECTION_H
#define MOCK_CONNECTION_H
#include "framework/connection/i_connection.h"

namespace mail_system {

// 零 I/O 的模拟连接 — 所有 async_* 操作同步调用回调，不涉及网络
// 用于测量纯 FSM 业务逻辑吞吐
class MockConnection : public IConnection {
public:
    MockConnection() = default;

    // --- data injection ---
    void set_read_data(const std::string& data) {
        read_buf_ = data;
        read_pos_ = 0;
    }
    void append_read_data(const std::string& data) {
        read_buf_ += data;
    }
    const std::string& written() const { return write_buf_; }
    void clear_written() { write_buf_.clear(); }
    void set_closed(bool c) { closed_ = c; }

    // 将 async_write 的数据同步捕获到外部 string（用于 STARTTLS 等连接被释放后的断言）
    void capture_to(std::string* target) { capture_target_ = target; }

    // --- IConnection impl ---
    void async_read(boost::asio::mutable_buffer buf, ReadHandler h) override {
        size_t n = std::min(buf.size(), read_buf_.size() - read_pos_);
        if (n > 0) {
            std::memcpy(buf.data(), read_buf_.data() + read_pos_, n);
            read_pos_ += n;
            h(boost::system::error_code(), n);
        } else {
            // 数据已耗尽 → 模拟对端关闭连接
            h(boost::asio::error::eof, 0);
        }
    }

    boost::asio::any_io_executor get_executor() override {
        // 同步模式下不会被用到
        return boost::asio::any_io_executor{};
    }

    void async_write(boost::asio::const_buffer buf, WriteHandler h) override {
        write_buf_.append(static_cast<const char*>(buf.data()), buf.size());
        if (capture_target_) *capture_target_ = write_buf_;
        h(boost::system::error_code(), buf.size());  // 同步回调
    }

    void async_handshake(boost::asio::ssl::stream_base::handshake_type, HandshakeHandler h) override {
        h(boost::system::error_code());
    }

    void close() override { closed_ = true; }
    bool is_open() const override { return !closed_; }
    uint16_t get_local_port() const override { return 0; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() override { return nullptr; }

private:
    std::string read_buf_;
    size_t read_pos_ = 0;
    std::string write_buf_;
    bool closed_ = false;
    std::string* capture_target_ = nullptr;
};

} // namespace mail_system
#endif
