#ifndef OUTBOUND_SMTP_FSM_H
#define OUTBOUND_SMTP_FSM_H

#include "framework/fsm_base.h"
#include "mail_system/back/outbound/outbound_types.hpp"
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

    void process_event(std::shared_ptr<SessionBase<ConnectionType>> session, OutboundSmtpEvent event) {
        this->dispatch(session, OutboundSmtpState::CONNECTED, event);
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
        this->add_transition(S::DATA_BODY,  E::CONNECT,         S::WAIT_ACCEPT);
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
