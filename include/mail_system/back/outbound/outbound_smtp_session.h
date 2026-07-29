#ifndef OUTBOUND_SMTP_SESSION_H
#define OUTBOUND_SMTP_SESSION_H

#include "framework/session_base.h"
#include "framework/fsm_base.h"
#include "framework/connection/tcp_connection.h"
#include "mail_system/back/outbound/outbound_types.hpp"
#include "mail_system/back/common/logger.h"
#include <queue>
#include <mutex>
#include <memory>
#include <string>

namespace mail_system {
namespace outbound {

// ================================================================
// OutboundSmtpFsm — 出站 SMTP 客户端状态机
// ================================================================
template <typename ConnectionType>
class OutboundSmtpFsm : public FsmBase<ConnectionType, OutboundSmtpState, OutboundSmtpEvent> {
public:
    OutboundSmtpFsm() {
        init_transition_table();
        init_state_handlers();
    }

private:
    void init_transition_table() {
        using S = OutboundSmtpState;
        using E = OutboundSmtpEvent;

        this->add_transition(S::INIT,      E::CONNECT,         S::CONNECTING);
        this->add_transition(S::CONNECTING,E::CONNECTED,       S::CONNECTED);
        this->add_transition(S::CONNECTED, E::GREETING_220,    S::EHLO);
        this->add_transition(S::EHLO,      E::EHLO_250,        S::MAIL_FROM);
        this->add_transition(S::EHLO,      E::STARTTLS_220,    S::MAIL_FROM);
        this->add_transition(S::MAIL_FROM, E::MAIL_250,        S::RCPT_TO);
        this->add_transition(S::RCPT_TO,   E::RCPT_250,        S::DATA);
        this->add_transition(S::DATA,      E::DATA_354,        S::DATA_BODY);
        this->add_transition(S::DATA_BODY, E::CONNECT,         S::WAIT_ACCEPT);
        this->add_transition(S::WAIT_ACCEPT, E::ACCEPT_250,    S::MAIL_FROM);  // 继续下一封
        this->add_transition(S::WAIT_ACCEPT, E::ERROR_4XX,     S::MAIL_FROM);  // 重试下一封

        // QUIT 链：任何错误 → QUIT
        for (int i = 0; i <= static_cast<int>(S::WAIT_ACCEPT); ++i) {
            auto s = static_cast<S>(i);
            if (s != S::INIT && s != S::CONNECTING && s != S::CLOSED) {
                this->add_transition(s, E::QUIT_221, S::CLOSED);
                this->add_transition(s, E::CONNECTION_LOST, S::CLOSED);
            }
            // 永久错误 → 放弃当前邮件
            this->add_transition(s, E::ERROR_5XX, s);  // stay, handler decides
        }
    }

    void init_state_handlers() {
        // 处理器在 OutboundSmtpSession 中绑定，这里只注册骨架
        // 因为 handler 需要访问 session 的队列和连接状态
    }

    bool is_terminal_state(OutboundSmtpState s) const override {
        return s == OutboundSmtpState::CLOSED;
    }

    void on_invalid_transition(OutboundSmtpState, OutboundSmtpEvent,
        std::shared_ptr<SessionBase<ConnectionType>> session) override
    {
        LOG_SMTP_WARN("Outbound invalid transition, closing");
        session->close();
    }
};

// ================================================================
// OutboundSmtpSession — 出站 SMTP 会话
//
//   主动 connect 到 MX，流水线投递多封邮件。
//   内置队列 + in_callback 模式。
// ================================================================
template <typename ConnectionType = TcpConnection>
class OutboundSmtpSession : public SessionBase<ConnectionType> {
public:
    static constexpr int MAX_MAILS_PER_CONNECTION = 100;

    OutboundSmtpSession(ServerBase* server, const std::string& mx_host, int mx_port = 25)
        : SessionBase<ConnectionType>(nullptr, server)
        , mx_host_(mx_host), mx_port_(mx_port)
        , fsm_(std::make_shared<OutboundSmtpFsm<ConnectionType>>())
    {
        // 注册所有状态处理
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
        if (should_start) {
            start_delivery_loop();
        }
    }

    size_t queue_size() const {
        std::lock_guard<std::mutex> lk(queue_mu_);
        return queue_.size();
    }

    const std::string& mx_host() const { return mx_host_; }

    // ── SessionBase 接口 ──────────────────────────────────────
    void handle_read(const std::string& data) override {
        // 解析 SMTP 响应码，映射为事件，驱动 FSM
        OutboundSmtpEvent event = parse_response(data);
        fsm_->process_event(this->shared_from_this(), event);
    }

    void process_read() override {
        // outbound 由 FSM handler 主动驱动，不需要 auto_process
    }

    void* get_fsm() const override { return fsm_.get(); }
    void* get_context() override { return nullptr; }
    void set_current_state(int) override {}
    void set_next_event(int) override {}
    int  get_current_state() const override { return 0; }
    int  get_next_event() const override { return 0; }
    std::string get_last_command_args() const override { return ""; }
    std::chrono::milliseconds compute_reply_delay() const override {
        return std::chrono::milliseconds(0);
    }

    // SMTP 行结束符
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

    std::string next_recipient() const { return next_recipient_; }
    std::string next_sender()    const { return next_sender_; }

private:
    // ── 状态处理函数 ──────────────────────────────────────────
    void register_handlers() {
        using S = OutboundSmtpState;
        using E = OutboundSmtpEvent;
        auto h = [this](auto handler) {
            return [this, handler](auto s) { (this->*handler)(s); };
        };

        fsm_->add_handler(S::CONNECTED, E::GREETING_220,  h(&OutboundSmtpSession::handle_connected_greeting));
        fsm_->add_handler(S::EHLO,      E::EHLO_250,       h(&OutboundSmtpSession::handle_ehlo_ok));
        fsm_->add_handler(S::MAIL_FROM, E::MAIL_250,       h(&OutboundSmtpSession::handle_mail_ok));
        fsm_->add_handler(S::RCPT_TO,   E::RCPT_250,       h(&OutboundSmtpSession::handle_rcpt_ok));
        fsm_->add_handler(S::DATA,      E::DATA_354,       h(&OutboundSmtpSession::handle_data_ok));
        fsm_->add_handler(S::WAIT_ACCEPT, E::ACCEPT_250,   h(&OutboundSmtpSession::handle_accept_ok));
        fsm_->add_handler(S::WAIT_ACCEPT, E::ERROR_4XX,    h(&OutboundSmtpSession::handle_temp_error));
        fsm_->add_handler(S::WAIT_ACCEPT, E::ERROR_5XX,    h(&OutboundSmtpSession::handle_perm_error));
        fsm_->add_handler(S::CLOSED,    E::CONNECTION_LOST, h(&OutboundSmtpSession::handle_closed));
    }

    // ── 投递循环 ──────────────────────────────────────────────
    void start_delivery_loop() {
        if (!this->connection_ || !this->connection_->is_open()) {
            connect_to_mx();
        } else {
            // 已有连接，取下一封
            deliver_next();
        }
    }

    void connect_to_mx() {
        // 创建 TCP 连接并发起 async_connect
        LOG_SMTP_INFO("Outbound: connecting to {}:{}", mx_host_, mx_port_);
        // TODO: 实际 async_connect 实现
        // 连接成功后: fsm_->process_event(self, OutboundSmtpEvent::CONNECTED);
    }

    void deliver_next() {
        std::unique_ptr<MailDeliveryTask> task;
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            if (queue_.empty()) {
                in_callback_ = false;
                return;
            }
            task = std::move(queue_.front());
            queue_.pop();
        }

        current_task_ = std::move(task);
        mails_sent_on_conn_++;

        // RFC 5321: MAIL FROM:
        next_sender_ = current_task_->sender;
        next_recipient_ = current_task_->recipient;

        auto self = this->shared_from_this();
        this->do_async_write("MAIL FROM:<" + next_sender_ + ">\r\n",
            [self](std::shared_ptr<SessionBase<ConnectionType>>, const boost::system::error_code&) mutable {
                self->fsm_->process_event(self, OutboundSmtpEvent::MAIL_250);
            });
    }

    // ── SMTP 响应解析 ─────────────────────────────────────────
    OutboundSmtpEvent parse_response(const std::string& line) {
        if (line.length() < 3) return OutboundSmtpEvent::CONNECTION_LOST;
        char c = line[0];
        if (c == '2') {
            if (line.find("220") == 0) return OutboundSmtpEvent::GREETING_220;
            if (line.find("250") == 0) return OutboundSmtpEvent::EHLO_250;
            if (line.find("221") == 0) return OutboundSmtpEvent::QUIT_221;
            return OutboundSmtpEvent::ACCEPT_250;
        }
        if (c == '3') return OutboundSmtpEvent::DATA_354;
        if (c == '4') return OutboundSmtpEvent::ERROR_4XX;
        if (c == '5') return OutboundSmtpEvent::ERROR_5XX;
        return OutboundSmtpEvent::CONNECTION_LOST;
    }

    // ── 状态处理器实现 ────────────────────────────────────────
    void handle_connected_greeting(std::shared_ptr<SessionBase<ConnectionType>> session) {
        LOG_SMTP_INFO("Outbound: got 220 from {}", mx_host_);
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        self->do_async_write("EHLO " + self->mx_host_ + "\r\n",
            [self](std::shared_ptr<SessionBase<ConnectionType>>, const boost::system::error_code&) mutable {
                self->fsm_->process_event(self, OutboundSmtpEvent::EHLO_250);
            });
    }

    void handle_ehlo_ok(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        self->deliver_next();
    }

    void handle_mail_ok(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        self->do_async_write("RCPT TO:<" + self->next_recipient_ + ">\r\n",
            [self](std::shared_ptr<SessionBase<ConnectionType>>, const boost::system::error_code&) mutable {
                self->fsm_->process_event(self, OutboundSmtpEvent::RCPT_250);
            });
    }

    void handle_rcpt_ok(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        self->do_async_write("DATA\r\n",
            [self](std::shared_ptr<SessionBase<ConnectionType>>, const boost::system::error_code&) mutable {
                self->fsm_->process_event(self, OutboundSmtpEvent::DATA_354);
            });
    }

    void handle_data_ok(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        // TODO: 从文件读取邮件正文，处理 dot-stuffing
        std::string body = "From: <" + self->next_sender_ + ">\r\n"
                          "To: <" + self->next_recipient_ + ">\r\n"
                          "Subject: Test\r\n\r\n"
                          "Hello, world!\r\n.\r\n";
        self->do_async_write(body,
            [self](std::shared_ptr<SessionBase<ConnectionType>>, const boost::system::error_code&) mutable {
                self->fsm_->process_event(self, OutboundSmtpEvent::ACCEPT_250);
            });
    }

    void handle_accept_ok(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        LOG_SMTP_INFO("Outbound: mail {} accepted by {}", self->current_task_->mail_id, self->mx_host_);

        if (self->mails_sent_on_conn_ >= MAX_MAILS_PER_CONNECTION) {
            self->do_async_write("QUIT\r\n",
                [self](std::shared_ptr<SessionBase<ConnectionType>>, const boost::system::error_code&) mutable {
                    self->fsm_->process_event(self, OutboundSmtpEvent::QUIT_221);
                });
            return;
        }
        self->deliver_next();
    }

    void handle_temp_error(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        LOG_SMTP_WARN("Outbound: temp error for mail {} to {}", self->current_task_->mail_id, self->mx_host_);
        // 保留邮件在队列头？或者标记重试
        self->deliver_next();
    }

    void handle_perm_error(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        LOG_SMTP_WARN("Outbound: perm error for mail {} to {}", self->current_task_->mail_id, self->mx_host_);
        self->current_task_.reset();
        self->deliver_next();
    }

    void handle_closed(std::shared_ptr<SessionBase<ConnectionType>> session) {
        auto self = std::static_pointer_cast<OutboundSmtpSession>(session);
        LOG_SMTP_INFO("Outbound: connection to {} closed", self->mx_host_);
        // 未完成的邮件重新入队
        if (self->current_task_) {
            std::lock_guard<std::mutex> lk(self->queue_mu_);
            self->queue_.push(std::move(self->current_task_));
        }
    }

    // ── 成员 ──────────────────────────────────────────────────
    std::string mx_host_;
    int mx_port_;
    std::shared_ptr<OutboundSmtpFsm<ConnectionType>> fsm_;

    mutable std::mutex queue_mu_;
    std::queue<std::unique_ptr<MailDeliveryTask>> queue_;
    bool in_callback_ = false;

    std::unique_ptr<MailDeliveryTask> current_task_;
    std::string next_sender_;
    std::string next_recipient_;
    int mails_sent_on_conn_ = 0;
};

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_SMTP_SESSION_H
