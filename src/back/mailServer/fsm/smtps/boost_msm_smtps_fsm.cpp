#include "mail_system/back/mailServer/fsm/smtps/boost_msm_smtps_fsm.h"
#include <iostream>

namespace mail_system {

BoostMsmSmtpsFsm::BoostMsmSmtpsFsm() {
    fsm_.start();
}

void BoostMsmSmtpsFsm::process_event(SmtpsSession* session, SmtpsEvent event, const std::string& args) {
    // 将SmtpsEvent转换为Boost MSM事件并分发
    dispatch_event(session, event, args);
}

SmtpsState BoostMsmSmtpsFsm::convert_state(int state) const {
    // 将Boost MSM状态ID转换为SmtpsState
    switch (state) {
        case 0: return SmtpsState::INIT;
        case 1: return SmtpsState::GREETING;
        case 2: return SmtpsState::WAIT_EHLO;
        case 3: return SmtpsState::WAIT_MAIL_FROM;
        case 4: return SmtpsState::WAIT_RCPT_TO;
        case 5: return SmtpsState::WAIT_DATA;
        case 6: return SmtpsState::IN_MESSAGE;
        case 7: return SmtpsState::WAIT_QUIT;
        default: return SmtpsState::INIT;
    }
}

void BoostMsmSmtpsFsm::dispatch_event(SmtpsSession* session, SmtpsEvent event, const std::string& args) {
    try {
        switch (event) {
            case SmtpsEvent::CONNECT:
                fsm_.process_event(SmtpsFsmDef::Connect{});
                break;
                
            case SmtpsEvent::EHLO:
                fsm_.process_event(SmtpsFsmDef::Ehlo{args, session});
                break;
                
            case SmtpsEvent::AUTH:
                fsm_.process_event(SmtpsFsmDef::Auth{args, session});
                break;
                
            case SmtpsEvent::MAIL_FROM:
                fsm_.process_event(SmtpsFsmDef::MailFrom{args, session});
                break;
                
            case SmtpsEvent::RCPT_TO:
                fsm_.process_event(SmtpsFsmDef::RcptTo{args, session});
                break;
                
            case SmtpsEvent::DATA:
                fsm_.process_event(SmtpsFsmDef::Data{args, session});
                break;
                
            case SmtpsEvent::DATA_END:
                fsm_.process_event(SmtpsFsmDef::DataEnd{args, session});
                break;
                
            case SmtpsEvent::QUIT:
                fsm_.process_event(SmtpsFsmDef::Quit{args, session});
                break;
                
            case SmtpsEvent::ERROR:
                fsm_.process_event(SmtpsFsmDef::Error_{args, session});
                break;
                
            case SmtpsEvent::TIMEOUT:
                fsm_.process_event(SmtpsFsmDef::Timeout{args, session});
                break;
        }
        
        // 更新当前状态
        session->set_current_state(convert_state(fsm_.current_state()[0]));
        
        std::cout << "SMTPS FSM (Boost MSM): " << get_state_name(session->get_current_state()) << " -> " 
                  << get_event_name(event) << std::endl;
                  
    } catch (const std::exception& e) {
        std::cerr << "SMTPS FSM (Boost MSM) error: " << e.what() << std::endl;
        
        // 处理错误
        if (session) {
            session->async_write("500 Internal server error\r\n");
        }
    }
}

} // namespace mail_system