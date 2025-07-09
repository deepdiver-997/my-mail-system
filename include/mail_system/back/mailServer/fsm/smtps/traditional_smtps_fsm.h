#ifndef TRADITIONAL_SMTPS_FSM_H
#define TRADITIONAL_SMTPS_FSM_H

#include "smtps_fsm.h"
#include <map>
#include <functional>

namespace mail_system {

// 传统的SMTPS状态机实现
class TraditionalSmtpsFsm : public SmtpsFsm {
public:
    TraditionalSmtpsFsm();
    ~TraditionalSmtpsFsm() override = default;

    // 处理事件
    void process_event(SmtpsSession* session, SmtpsEvent event, const std::string& args) override;


private:

    // 状态转换表类型
    using StateTransitionTable = std::map<std::pair<SmtpsState, SmtpsEvent>, SmtpsState>;
    
    // 状态转换表
    StateTransitionTable transition_table_;

    // 状态处理函数表
    std::map<SmtpsState, std::map<SmtpsEvent, StateHandler>> state_handlers_;

    // 初始化状态转换表
    void init_transition_table();

    // 初始化状态处理函数
    void init_state_handlers();

    // 状态处理函数
    void handle_init_connect(SmtpsSession* session, const std::string& args);
    void handle_greeting_ehlo(SmtpsSession* session, const std::string& args);
    void handle_wait_auth_auth(SmtpsSession* session, const std::string& args);
    void handle_wait_ehlo_ehlo(SmtpsSession* session, const std::string& args);
    void handle_wait_mail_from_mail_from(SmtpsSession* session, const std::string& args);
    void handle_wait_rcpt_to_rcpt_to(SmtpsSession* session, const std::string& args);
    void handle_wait_data_data(SmtpsSession* session, const std::string& args);
    void handle_in_message_data_end(SmtpsSession* session, const std::string& args);
    void handle_wait_quit_quit(SmtpsSession* session, const std::string& args);
    void handle_error(SmtpsSession* session, const std::string& args);
};

} // namespace mail_system

#endif // TRADITIONAL_SMTPS_FSM_H