#ifndef MOCK_CONNECTION_H
#define MOCK_CONNECTION_H
#include "framework/connection/i_connection.h"
#include "mock_io_context.h"

#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mail_system {

// ================================================================
// MockConnection — 基于 MockIoContext 的零 I/O 模拟连接
//
//   异步读/写完成被包装成任务 post 到内部 MockIoContext：
//     - 同步模式（默认，未 start()）：任务内联执行 → 与旧行为一致，
//       供串行 FSM 逻辑测试（process_event 同步完成）。
//     - 线程模式（start() 后）：任务由独立工作线程执行 → 还原 asio 的
//       completion 投递语义，供并发/异步测试（跨线程投递用户缓冲区）。
// ================================================================
class MockConnection : public IConnection {
public:
    MockConnection() = default;

    // ---- MockIoContext 生命周期 ----
    void start() { ctx_.start(); }
    void stop() { ctx_.stop(); }
    test::MockIoContext& context() { return ctx_; }
    bool wait_idle(int timeout_ms = 3000) { return ctx_.wait_idle(timeout_ms); }

    // --- data injection ---
    void set_read_data(const std::string& data) {
        std::lock_guard<std::mutex> lk(mu_);
        read_buf_ = data;
        read_pos_ = 0;
    }
    void append_read_data(const std::string& data) {
        std::lock_guard<std::mutex> lk(mu_);
        read_buf_ += data;
    }
    std::string written() const {
        std::lock_guard<std::mutex> lk(mu_);
        return write_buf_;
    }
    void clear_written() {
        std::lock_guard<std::mutex> lk(mu_);
        write_buf_.clear();
    }
    void set_closed(bool c) {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = c;
    }

    // 将 async_write 的数据同步捕获到外部 string（用于 STARTTLS 等连接被释放后的断言）
    void capture_to(std::string* target) {
        std::lock_guard<std::mutex> lk(mu_);
        capture_target_ = target;
    }

    // ---- IDLE / deferred read support ----
    // 无数据时保存 {缓冲, handler}，由 trigger_deferred_read 投递数据。
    void set_deferred_read(bool v) {
        std::lock_guard<std::mutex> lk(mu_);
        deferred_read_ = v;
    }
    bool has_pending_read() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<bool>(pending_read_.h);
    }
    void trigger_deferred_read(const std::string& data) {
        PendingRead pr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            read_buf_ += data;
            pr = std::move(pending_read_);
            pending_read_ = PendingRead{};
        }
        if (!pr.h) return;
        ctx_.post([this, pr = std::move(pr)]() mutable {
            boost::system::error_code ec;
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                size_t avail = read_buf_.size() - read_pos_;
                n = std::min(pr.buf.size(), avail);
                if (n > 0) {
                    std::memcpy(pr.buf.data(), read_buf_.data() + read_pos_, n);
                    read_pos_ += n;
                } else {
                    ec = boost::asio::error::eof;
                }
            }
            pr.h(ec, n);
        });
    }

    // ---- deferred write support（模拟慢写 / 写完成晚于 session 逻辑结束） ----
    // 数据仍立即写入 write_buf_，仅延迟完成回调；默认关闭（同步回调）。
    void set_deferred_write(bool v) {
        std::lock_guard<std::mutex> lk(mu_);
        deferred_write_ = v;
    }
    bool has_pending_write() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_write_handler_ != nullptr;
    }
    void trigger_deferred_write() {
        WriteHandler h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            h = std::move(pending_write_handler_);
            pending_write_handler_ = nullptr;
        }
        if (h) h(boost::system::error_code(), 0);
    }

    // --- IConnection impl ---
    void async_read(boost::asio::mutable_buffer buf, ReadHandler h) override {
        bool defer = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (deferred_read_ && read_buf_.size() <= read_pos_) {
                pending_read_ = PendingRead{buf, std::move(h)};
                defer = true;
            }
        }
        if (defer) return;

        ctx_.post([this, buf, h = std::move(h)]() mutable {
            boost::system::error_code ec;
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                size_t avail = read_buf_.size() - read_pos_;
                if (avail > 0) {
                    n = std::min(buf.size(), avail);
                    // 工作线程把用户缓冲区内容投递到调用方缓冲（跨线程）
                    std::memcpy(buf.data(), read_buf_.data() + read_pos_, n);
                    read_pos_ += n;
                } else {
                    ec = boost::asio::error::eof;
                }
            }
            h(ec, n);
        });
    }

    boost::asio::any_io_executor get_executor() override {
        return boost::asio::any_io_executor{};
    }

    void async_write(boost::asio::const_buffer buf, WriteHandler h) override {
        size_t n = buf.size();
        {
            std::lock_guard<std::mutex> lk(mu_);
            write_buf_.append(static_cast<const char*>(buf.data()), n);
            if (capture_target_) *capture_target_ = write_buf_;
            if (deferred_write_) {
                pending_write_handler_ = std::move(h);
                return;   // 延迟完成回调，由 trigger_deferred_write() 触发
            }
        }
        ctx_.post([h = std::move(h), n]() mutable {
            h(boost::system::error_code(), n);
        });
    }

    void async_write_with_delay(boost::asio::const_buffer buf,
                                std::chrono::milliseconds delay,
                                WriteHandler h) override {
        // Mock 无真实定时器：忽略 delay，按普通异步写投递
        (void)delay;
        async_write(buf, std::move(h));
    }

    void async_handshake(boost::asio::ssl::stream_base::handshake_type, HandshakeHandler h) override {
        ctx_.post([h = std::move(h)]() mutable {
            h(boost::system::error_code());
        });
    }

    void close() override {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }
    bool is_open() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return !closed_;
    }
    uint16_t get_local_port() const override { return 0; }
    std::string get_remote_ip() const override { return "127.0.0.1"; }
    std::unique_ptr<boost::asio::ip::tcp::socket> release_socket() override { return nullptr; }

private:
    // deferred read：保存缓冲视图 + handler（缓冲指向 session 的 read_buffer_）
    struct PendingRead {
        boost::asio::mutable_buffer buf;
        ReadHandler h;
    };

    mutable std::mutex mu_;
    test::MockIoContext ctx_;
    std::string read_buf_;
    size_t read_pos_ = 0;
    std::string write_buf_;
    bool closed_ = false;
    std::string* capture_target_ = nullptr;
    bool deferred_read_ = false;
    bool deferred_write_ = false;
    PendingRead pending_read_;
    WriteHandler pending_write_handler_;
};

} // namespace mail_system
#endif
