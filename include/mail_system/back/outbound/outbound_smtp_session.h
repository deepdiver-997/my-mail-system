#ifndef OUTBOUND_SMTP_SESSION_H
#define OUTBOUND_SMTP_SESSION_H

#include "framework/session_base.h"
#include "framework/connection/tcp_connection.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "mail_system/back/outbound/outbound_types.hpp"
#include "mail_system/back/outbound/outbound_smtp_fsm.h"
#include "mail_system/back/common/logger.h"
#include <queue>
#include <mutex>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>

namespace mail_system {
namespace outbound {

// ================================================================
// OutboundSmtpSession — 出站 SMTP 会话
// ================================================================
template <typename ConnectionType = TcpConnection>
class OutboundSmtpSession : public SessionBase<ConnectionType> {
public:
    static constexpr int MAX_MAILS_PER_CONNECTION = 100;

    OutboundSmtpSession(ServerBase* server, const std::string& mx_host, int mx_port = 25)
        : SessionBase<ConnectionType>(nullptr, server)
        , mx_host_(mx_host), mx_port_(mx_port)
        , helo_domain_(server->m_domain)
        , fsm_(std::make_shared<OutboundSmtpFsm<ConnectionType>>())
    {
        register_handlers();
    }

    // ── 公开接口 ──────────────────────────────────────────────
    void submit(std::unique_ptr<MailDeliveryTask> task) {
        bool should_start = false;
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            queue_.push(std::move(task));
            if (!in_callback_) {
                in_callback_ = true;
                should_start = true;
            }
        }
        if (should_start) start_delivery_loop();
    }

    size_t queue_size() const {
        std::lock_guard<std::mutex> lk(queue_mu_);
        return queue_.size();
    }

    // 投递完成回调: void(uint64_t record_id, bool success)
    using CompletionCb = std::function<void(uint64_t, bool)>;
    void set_completion_cb(CompletionCb cb) { completion_cb_ = std::move(cb); }

    const std::string& mx_host() const { return mx_host_; }
    bool is_connected() const { return this->connection_ && this->connection_->is_open(); }
    bool has_active_task() const { return current_task_ != nullptr; }

    // ── SessionBase 接口 ──────────────────────────────────────
    void handle_read(const std::string& data) override {
        // 追加到多行响应缓冲区
        response_buf_ += data;
        // 检查是否收到完整的 SMTP 响应（最后一行第4字符是空格）
        if (is_response_complete(response_buf_)) {
            OutboundSmtpEvent event = parse_response(response_buf_);
            response_buf_.clear();
            fsm_->process_event(this->shared_from_this(), event);
        } else {
            this->do_async_read();
        }
    }

    void process_read() override {}
    void* get_fsm() const override { return fsm_.get(); }
    void* get_context() override { return nullptr; }
    void set_current_state(int) override {}
    void set_next_event(int) override {}
    int  get_current_state() const override { return 0; }
    int  get_next_event() const override { return 0; }
    std::string get_last_command_args() const override { return ""; }
    std::chrono::milliseconds compute_reply_delay() const override { return std::chrono::milliseconds(0); }

    bool has_buffered_input() const override {
        return this->command_read_buffer_.find('\n') != std::string::npos;
    }
    std::string extract_one_line() override {
        auto pos = this->command_read_buffer_.find('\n');
        if (pos == std::string::npos) return {};
        std::string line = this->command_read_buffer_.substr(0, pos + 1);
        this->command_read_buffer_.erase(0, pos + 1);
        return line;
    }

private:
    // ── 多行 SMTP 响应 ────────────────────────────────────────
    // SMTP 多行响应格式: "250-xxx\r\n250-xxx\r\n250 yyy\r\n"
    // 最后一行第4字符是空格，中间行第4字符是 '-'
    static bool is_response_complete(const std::string& buf) {
        if (buf.size() < 5) return false;
        // 从末尾找最后一个 \r\n 对，检查它前面的行是否以 "ddd " 开头
        auto last_nl = buf.rfind("\r\n");
        if (last_nl == std::string::npos || last_nl < 4) return false;
        // 找这一行的开头
        auto line_start = buf.rfind("\r\n", last_nl - 1);
        size_t start = (line_start == std::string::npos) ? 0 : line_start + 2;
        // 检查第4个字符是否是空格
        return (start + 3 < buf.size()) && buf[start + 3] == ' ';
    }

    OutboundSmtpEvent parse_response(const std::string& buf) {
        if (buf.size() < 3) return OutboundSmtpEvent::CONNECTION_LOST;
        char c = buf[0];
        const auto* code = buf.c_str();
        if (c == '2') {
            if (strncmp(code, "220", 3) == 0) return OutboundSmtpEvent::GREETING_220;
            if (strncmp(code, "250", 3) == 0) return OutboundSmtpEvent::EHLO_250;
            if (strncmp(code, "221", 3) == 0) return OutboundSmtpEvent::QUIT_221;
            return OutboundSmtpEvent::ACCEPT_250;
        }
        if (c == '3') return OutboundSmtpEvent::DATA_354;
        if (c == '4') return OutboundSmtpEvent::ERROR_4XX;
        if (c == '5') return OutboundSmtpEvent::ERROR_5XX;
        return OutboundSmtpEvent::CONNECTION_LOST;
    }

    // ── 处理器注册 ────────────────────────────────────────────
    void register_handlers() {
        using S = OutboundSmtpState;
        using E = OutboundSmtpEvent;
        auto h = [this](auto handler) {
            return [this, handler](auto s) { (this->*handler)(s); };
        };
        fsm_->add_handler(S::CONNECTED,  E::GREETING_220, h(&OutboundSmtpSession::handle_greeting_220));
        fsm_->add_handler(S::EHLO,       E::EHLO_250,     h(&OutboundSmtpSession::handle_ehlo_250));
        fsm_->add_handler(S::MAIL_FROM,  E::MAIL_250,     h(&OutboundSmtpSession::handle_mail_250));
        fsm_->add_handler(S::RCPT_TO,    E::RCPT_250,     h(&OutboundSmtpSession::handle_rcpt_250));
        fsm_->add_handler(S::DATA,       E::DATA_354,     h(&OutboundSmtpSession::handle_data_354));
        fsm_->add_handler(S::WAIT_ACCEPT,E::ACCEPT_250,   h(&OutboundSmtpSession::handle_accept_250));
        fsm_->add_handler(S::WAIT_ACCEPT,E::ERROR_4XX,    h(&OutboundSmtpSession::handle_temp_error));
        fsm_->add_handler(S::WAIT_ACCEPT,E::ERROR_5XX,    h(&OutboundSmtpSession::handle_perm_error));
        fsm_->add_handler(S::CLOSED,     E::CONNECTION_LOST, h(&OutboundSmtpSession::handle_closed));
    }

    // ── 投递循环 ──────────────────────────────────────────────
    void start_delivery_loop() {
        if (!is_connected()) {
            connect_to_mx();
        } else {
            deliver_next();
        }
    }

    void connect_to_mx() {
        auto& io_ctx = static_cast<IOThreadPool*>(this->m_server->m_ioThreadPool.get())->get_io_context();
        auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(io_ctx);

        auto self = std::static_pointer_cast<OutboundSmtpSession>(this->shared_from_this());
        resolver->async_resolve(mx_host_, std::to_string(mx_port_),
            [self, resolver](const boost::system::error_code& ec,
                             boost::asio::ip::tcp::resolver::results_type endpoints) mutable {
                if (ec || endpoints.empty()) {
                    LOG_SMTP_ERROR("Outbound: DNS resolve failed for {}: {}", self->mx_host_, ec.message());
                    self->handle_connect_failure();
                    return;
                }
                auto& io_ctx2 = static_cast<IOThreadPool*>(
                    self->m_server->m_ioThreadPool.get())->get_io_context();
                auto sock = std::make_unique<boost::asio::ip::tcp::socket>(io_ctx2);

                boost::asio::async_connect(*sock, endpoints,
                    [self, sock = std::move(sock)](const boost::system::error_code& ec,
                                                   boost::asio::ip::tcp::endpoint) mutable {
                        if (ec) {
                            LOG_SMTP_ERROR("Outbound: connect to {} failed: {}",
                                           self->mx_host_, ec.message());
                            self->handle_connect_failure();
                            return;
                        }
                        LOG_SMTP_INFO("Outbound: connected to {}", self->mx_host_);
                        self->connection_ = std::make_unique<TcpConnection>(std::move(sock));
                        self->mails_sent_on_conn_ = 0;
                        self->fsm_->process_event(self, OutboundSmtpEvent::CONNECTED);
                        self->do_async_read();
                    });
            });
    }

    void handle_connect_failure() {
        // 连接失败：当前邮件放回队列头
        if (current_task_) {
            std::lock_guard<std::mutex> lk(queue_mu_);
            queue_.push(std::move(current_task_));
        }
        in_callback_ = false;
    }

    void deliver_next() {
        std::unique_ptr<MailDeliveryTask> task;
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            if (queue_.empty()) {
                in_callback_ = false;
                do_graceful_quit();
                return;
            }
            task = std::move(queue_.front());
            queue_.pop();
        }
        current_task_ = std::move(task);
        mails_sent_on_conn_++;

        auto self = std::static_pointer_cast<OutboundSmtpSession>(this->shared_from_this());
        this->do_async_write("MAIL FROM:<" + current_task_->sender + ">\r\n",
            [self](auto, const boost::system::error_code&) mutable {
                self->do_async_read();
            });
    }

    void do_graceful_quit() {
        if (!is_connected()) return;
        auto self = std::static_pointer_cast<OutboundSmtpSession>(this->shared_from_this());
        this->do_async_write("QUIT\r\n",
            [self](auto, const boost::system::error_code&) mutable {
                self->fsm_->process_event(self, OutboundSmtpEvent::QUIT_221);
            });
    }

    // ── 邮件正文 ──────────────────────────────────────────────
    std::string load_and_stuff_body() {
        if (!current_task_ || !current_task_->mail_ptr) return ".\r\n";
        const auto& m = *current_task_->mail_ptr;

        // 优先从内存读取，否则从文件读取
        if (!m.body.empty()) return dot_stuff(m.body) + "\r\n.\r\n";

        std::ifstream file(m.body_path, std::ios::binary);
        if (!file.is_open()) {
            LOG_SMTP_ERROR("Outbound: cannot open mail body {}", m.body_path);
            return ".\r\n";
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        return dot_stuff(ss.str()) + "\r\n.\r\n";
    }

    // dot-stuffing: 行首的 '.' → '..'
    static std::string dot_stuff(const std::string& body) {
        std::string out;
        out.reserve(body.size() + body.size() / 50);  // 预分配 ~2% 余量
        for (size_t i = 0; i < body.size(); ++i) {
            char c = body[i];
            out.push_back(c);
            // 行首是 '.' → 双写
            if (c == '\n' && i + 1 < body.size() && body[i + 1] == '.')
                out.push_back('.');
        }
        // 如果整个 body 以 '.' 开头
        if (!body.empty() && body[0] == '.') {
            out.insert(0, ".");
        }
        return out;
    }

    // ── 状态处理器 ────────────────────────────────────────────
    void handle_greeting_220(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        LOG_SMTP_INFO("Outbound: 220 from {}", self->mx_host_);
        self->do_async_write("EHLO " + self->helo_domain_ + "\r\n",
            [self](auto, const boost::system::error_code&) mutable { self->do_async_read(); });
    }

    void handle_ehlo_250(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        self->deliver_next();
    }

    void handle_mail_250(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        self->do_async_write("RCPT TO:<" + self->current_task_->recipient + ">\r\n",
            [self](auto, const boost::system::error_code&) mutable { self->do_async_read(); });
    }

    void handle_rcpt_250(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        self->do_async_write("DATA\r\n",
            [self](auto, const boost::system::error_code&) mutable { self->do_async_read(); });
    }

    void handle_data_354(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        std::string body = self->load_and_stuff_body();
        self->do_async_write(body,
            [self](auto, const boost::system::error_code&) mutable { self->do_async_read(); });
    }

    void handle_accept_250(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        LOG_SMTP_INFO("Outbound: mail {} accepted by {}", self->current_task_->mail_id, self->mx_host_);
        if (self->completion_cb_) {
            self->completion_cb_(self->current_task_->record_id, true);
        }
        self->current_task_.reset();

        if (self->mails_sent_on_conn_ >= MAX_MAILS_PER_CONNECTION) {
            self->do_graceful_quit();
            return;
        }
        // RSET 后发下一封
        self->do_async_write("RSET\r\n",
            [self](auto, const boost::system::error_code&) mutable {
                self->do_async_read();
                // RSET 的 250 响应也会触发 handle_read，但 RSET 不需要特殊处理
                // 直接在回调里发下一封
                self->deliver_next();
            });
    }

    void handle_temp_error(std::shared_ptr<SessionBase<ConnectionType>>) {
        LOG_SMTP_WARN("Outbound: 4xx temp error for {} to {}", current_task_->mail_id, mx_host_);
        deliver_next();
    }

    void handle_perm_error(std::shared_ptr<SessionBase<ConnectionType>>) {
        LOG_SMTP_WARN("Outbound: 5xx perm error for {} to {}", current_task_->mail_id, mx_host_);
        if (completion_cb_) completion_cb_(current_task_->record_id, false);
        current_task_.reset();
        deliver_next();
    }

    void handle_closed(std::shared_ptr<SessionBase<ConnectionType>>) {
        LOG_SMTP_INFO("Outbound: connection to {} closed", mx_host_);
        if (current_task_) {
            std::lock_guard<std::mutex> lk(queue_mu_);
            queue_.push(std::move(current_task_));
        }
        in_callback_ = false;
    }

    // ── 成员 ──────────────────────────────────────────────────
    std::string mx_host_;
    int mx_port_;
    std::string helo_domain_;
    std::shared_ptr<OutboundSmtpFsm<ConnectionType>> fsm_;
    std::string response_buf_;  // 多行 SMTP 响应缓冲区

    mutable std::mutex queue_mu_;
    std::queue<std::unique_ptr<MailDeliveryTask>> queue_;
    bool in_callback_ = false;
    CompletionCb completion_cb_;

    std::unique_ptr<MailDeliveryTask> current_task_;
    int mails_sent_on_conn_ = 0;
};

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_SMTP_SESSION_H
