#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.h"
#include <unordered_map>

namespace mail_system {

std::string SmtpsFsm::get_state_name(SmtpsState state) {
    static const std::unordered_map<SmtpsState, std::string> state_names = {
        {SmtpsState::INIT, "INIT"},
        {SmtpsState::GREETING, "GREETING"},
        {SmtpsState::WAIT_EHLO, "WAIT_EHLO"},
        {SmtpsState::WAIT_MAIL_FROM, "WAIT_MAIL_FROM"},
        {SmtpsState::WAIT_RCPT_TO, "WAIT_RCPT_TO"},
        {SmtpsState::WAIT_DATA, "WAIT_DATA"},
        {SmtpsState::IN_MESSAGE, "IN_MESSAGE"},
        {SmtpsState::WAIT_QUIT, "WAIT_QUIT"}
    };

    auto it = state_names.find(state);
    if (it != state_names.end()) {
        return it->second;
    }
    return "UNKNOWN_STATE";
}

std::string SmtpsFsm::get_event_name(SmtpsEvent event) {
    static const std::unordered_map<SmtpsEvent, std::string> event_names = {
        {SmtpsEvent::CONNECT, "CONNECT"},
        {SmtpsEvent::EHLO, "EHLO"},
        {SmtpsEvent::MAIL_FROM, "MAIL_FROM"},
        {SmtpsEvent::RCPT_TO, "RCPT_TO"},
        {SmtpsEvent::DATA, "DATA"},
        {SmtpsEvent::DATA_END, "DATA_END"},
        {SmtpsEvent::QUIT, "QUIT"},
        {SmtpsEvent::ERROR, "ERROR"},
        {SmtpsEvent::TIMEOUT, "TIMEOUT"}
    };

    auto it = event_names.find(event);
    if (it != event_names.end()) {
        return it->second;
    }
    return "UNKNOWN_EVENT";
}

} // namespace mail_system