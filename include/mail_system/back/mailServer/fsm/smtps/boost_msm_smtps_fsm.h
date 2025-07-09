#ifndef BOOST_MSM_SMTPS_FSM_H
#define BOOST_MSM_SMTPS_FSM_H

#include "smtps_fsm.h"
#include <boost/msm/back/state_machine.hpp>
#include <boost/msm/front/state_machine_def.hpp>
#include <boost/msm/front/functor_row.hpp>

namespace mail_system {

// Boost MSM的SMTPS状态机实现
class BoostMsmSmtpsFsm : public SmtpsFsm {
public:
    BoostMsmSmtpsFsm();
    ~BoostMsmSmtpsFsm() override = default;

    // 处理事件
    void process_event(SmtpsSession* session, SmtpsEvent event, const std::string& args) override;

private:
    // 前端状态机定义
    struct SmtpsFsmDef : public boost::msm::front::state_machine_def<SmtpsFsmDef> {
        // 状态定义
        struct Init : public boost::msm::front::state<> {};
        struct Greeting : public boost::msm::front::state<> {};
        struct WaitEhlo : public boost::msm::front::state<> {};
        struct WaitAuth : public boost::msm::front::state<> {};
        struct WaitMailFrom : public boost::msm::front::state<> {};
        struct WaitRcptTo : public boost::msm::front::state<> {};
        struct WaitData : public boost::msm::front::state<> {};
        struct InMessage : public boost::msm::front::state<> {};
        struct WaitQuit : public boost::msm::front::state<> {};
        struct Error : public boost::msm::front::state<> {};

        // 初始状态
        using initial_state = Init;

        // 事件定义
        struct Connect {};
        struct Ehlo { std::string args; SmtpsSession* session; };
        struct Auth { std::string args; SmtpsSession* session; };
        struct MailFrom { std::string args; SmtpsSession* session; };
        struct RcptTo { std::string args; SmtpsSession* session; };
        struct Data { std::string args; SmtpsSession* session; };
        struct DataEnd { std::string args; SmtpsSession* session; };
        struct Quit { std::string args; SmtpsSession* session; };
        struct Error_ { std::string args; SmtpsSession* session; };
        struct Timeout { std::string args; SmtpsSession* session; };

        // 动作定义
        struct HandleInitConnect {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理连接事件
                }
            }
        };

        struct HandleGreetingEhlo {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理EHLO命令
                }
            }
        };

        struct HandleWaitEhloEhlo {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理EHLO命令
                }
            }
        };

        struct HandleWaitAuthAuth {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理AUTH命令
                }
            }
        };

        struct HandleWaitMailFromMailFrom {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理MAIL FROM命令
                }
            }
        };

        struct HandleWaitRcptToRcptTo {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理RCPT TO命令
                }
            }
        };

        struct HandleWaitDataData {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理DATA命令
                }
            }
        };

        struct HandleInMessageDataEnd {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理数据结束
                }
            }
        };

        struct HandleWaitQuitQuit {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理QUIT命令
                }
            }
        };

        struct HandleError {
            template <class Event, class FSM, class SourceState, class TargetState>
            void operator()(Event const& evt, FSM& fsm, SourceState&, TargetState&) {
                if (evt.session) {
                    // 处理错误
                }
            }
        };

        // 状态转换表
        struct transition_table : boost::mpl::vector<
            // Start         Event      Next           Action                        Guard
            boost::msm::front::Row<Init,        Connect,   Greeting,      HandleInitConnect,            boost::msm::front::none>,
            boost::msm::front::Row<Greeting,    Ehlo,      WaitEhlo,      HandleGreetingEhlo,           boost::msm::front::none>,
            boost::msm::front::Row<WaitEhlo,    Ehlo,      WaitAuth,      HandleWaitEhloEhlo,           boost::msm::front::none>,
            boost::msm::front::Row<WaitAuth,    Auth,      WaitMailFrom,  HandleWaitAuthAuth,           boost::msm::front::none>,
            boost::msm::front::Row<WaitMailFrom,MailFrom,  WaitRcptTo,    HandleWaitMailFromMailFrom,   boost::msm::front::none>,
            boost::msm::front::Row<WaitRcptTo,  RcptTo,    WaitData,      HandleWaitRcptToRcptTo,       boost::msm::front::none>,
            boost::msm::front::Row<WaitData,    Data,      InMessage,     HandleWaitDataData,           boost::msm::front::none>,
            boost::msm::front::Row<InMessage,   DataEnd,   WaitMailFrom,  HandleInMessageDataEnd,       boost::msm::front::none>,
            boost::msm::front::Row<WaitMailFrom,Quit,      WaitQuit,      HandleWaitQuitQuit,           boost::msm::front::none>,
            boost::msm::front::Row<WaitRcptTo,  Quit,      WaitQuit,      HandleWaitQuitQuit,           boost::msm::front::none>,
            boost::msm::front::Row<WaitData,    Quit,      WaitQuit,      HandleWaitQuitQuit,           boost::msm::front::none>,
            boost::msm::front::Row<InMessage,   Quit,      WaitQuit,      HandleWaitQuitQuit,           boost::msm::front::none>,
            boost::msm::front::Row<WaitQuit,    Quit,      WaitQuit,      HandleWaitQuitQuit,           boost::msm::front::none>
        > {};

        // 处理未定义的转换
        template <class FSM, class Event>
        void no_transition(Event const& e, FSM&, int state) {
            // 处理未定义的转换
        }
    };

    // 状态机类型
    using SmtpsFsm = boost::msm::back::state_machine<SmtpsFsmDef>;

    // 状态机实例
    SmtpsFsm fsm_;

    // 将Boost MSM状态转换为SmtpsState
    SmtpsState convert_state(int state) const;

    // 将SmtpsEvent转换为Boost MSM事件
    void dispatch_event(SmtpsSession* session, SmtpsEvent event, const std::string& args);
};

} // namespace mail_system

#endif // BOOST_MSM_SMTPS_FSM_H