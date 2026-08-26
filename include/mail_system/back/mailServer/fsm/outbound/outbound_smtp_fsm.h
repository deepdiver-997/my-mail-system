#ifndef OUTBOUND_SMTP_FSM_H
#define OUTBOUND_SMTP_FSM_H

#include "framework/fsm_base.h"
#include "mail_system/back/mailServer/outbound/outbound_types.hpp"
#include "mail_system/back/common/logger.h"

namespace mail_system {
namespace outbound {

// ================================================================
// OutboundSmtpFsm — 出站 SMTP 客户端状态机
// ================================================================
template <typename ConnectionType>
class OutboundSmtpFsm : public FsmBase<ConnectionType, OutboundSmtpState, OutboundSmtpEvent> {
public:
    OutboundSmtpFsm() { init_transition_table(); }

    // 2026-08-26 fix: 之前硬编码 OutboundSmtpState::CONNECTED 绕过整个 transition table，
    // 导致 (CONNECTED, CONNECTED) / (CONNECTED, EHLO_250) 等全部 on_invalid_transition → close。
    // 现在读 session 的真 current_state（参考 smtps_session.tpp:108-110 + smtps_fsm.tpp:213）。
    // 注：FsmBase::process_event 不是 virtual，所以这里"覆写"是 name hiding 形式（不写 override）。
    void process_event(std::shared_ptr<SessionBase<ConnectionType>> session, OutboundSmtpEvent event) {
        auto cur = static_cast<OutboundSmtpState>(session->get_current_state());
        this->dispatch(session, cur, event);
        // 派发后按 transition_table 自动推进状态。替代 smtps 那种"handler 末尾手动 set_current_state"
        // 模式——outbound 链长（EHLO→MAIL→RCPT→DATA→WAIT_ACCEPT×N），易漏一处。
        // transition_table_ 是 base protected，加 this-> 让模板基类查找明确。
        auto it = this->transition_table_.find({cur, event});
        if (it != this->transition_table_.end()) {
            session->set_current_state(static_cast<int>(it->second));
        }
    }

private:
    void init_transition_table() {
        using S = OutboundSmtpState;
        using E = OutboundSmtpEvent;

        this->add_transition(S::INIT,       E::CONNECT,         S::CONNECTING);
        this->add_transition(S::CONNECTING, E::CONNECTED,       S::CONNECTED);
        this->add_transition(S::CONNECTED,  E::GREETING_220,    S::EHLO);
        this->add_transition(S::EHLO,       E::EHLO_250,        S::MAIL_FROM);
        this->add_transition(S::MAIL_FROM,  E::MAIL_250,        S::RCPT_TO);
        this->add_transition(S::RCPT_TO,    E::RCPT_250,        S::DATA);
        this->add_transition(S::DATA,       E::DATA_354,        S::DATA_BODY);
        this->add_transition(S::DATA_BODY,  E::ACCEPT_250,     S::WAIT_ACCEPT);  // body 末 .\r\n 后 server 回 250
        this->add_transition(S::DATA_BODY,  E::CONNECT,         S::WAIT_ACCEPT);  // 语义错（DATA_BODY 不该收 CONNECT），保留以兼容
        this->add_transition(S::WAIT_ACCEPT, E::ACCEPT_250,     S::MAIL_FROM);
        this->add_transition(S::WAIT_ACCEPT, E::ERROR_4XX,      S::MAIL_FROM);
        this->add_transition(S::WAIT_ACCEPT, E::ERROR_5XX,      S::MAIL_FROM);

        for (int i = 0; i <= static_cast<int>(S::WAIT_ACCEPT); ++i) {
            auto s = static_cast<S>(i);
            if (s != S::INIT && s != S::CONNECTING && s != S::CLOSED) {
                this->add_transition(s, E::QUIT_221, S::CLOSED);
                this->add_transition(s, E::CONNECTION_LOST, S::CLOSED);
            }
            this->add_transition(s, E::ERROR_5XX, s);
        }
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

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_SMTP_FSM_H
