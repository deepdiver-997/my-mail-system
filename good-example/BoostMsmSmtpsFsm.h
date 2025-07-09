#ifndef MAIL_SYSTEM_BOOST_MSM_SMTPS_FSM_H
#define MAIL_SYSTEM_BOOST_MSM_SMTPS_FSM_H

#include "SmtpsFsm.h"
#include <boost/msm/front/state_machine_def.hpp>
#include <boost/msm/back/state_machine.hpp>
#include <boost/msm/front/functor_row.hpp>
#include <boost/msm/front/row2.hpp>
#include <boost/mpl/vector.hpp>

// // 定义事件类型
// namespace mail_system {
// namespace SMTPS {

struct EhloEvent {};
struct MailEvent {};
struct RcptEvent {};
struct DataEvent {};
struct DataLineEvent {};
struct DataEndEvent {};
struct AuthEvent {};
struct AuthDataEvent {};
struct StartTlsEvent {};
struct UnknownEvent {};


namespace mail_system {
namespace SMTPS {

// 前向声明
class SmtpsMsm;

// MSM状态机前端类
class SmtpsMsmDef : public boost::msm::front::state_machine_def<SmtpsMsmDef> {
public:
    // 状态声明
    struct Init : public boost::msm::front::state<> {};
    struct Ehlo : public boost::msm::front::state<> {};
    struct MailFrom : public boost::msm::front::state<> {};
    struct RcptTo : public boost::msm::front::state<> {};
    struct Data : public boost::msm::front::state<> {};
    struct DataContent : public boost::msm::front::state<> {};
    struct Auth : public boost::msm::front::state<> {};
    struct AuthUsername : public boost::msm::front::state<> {};
    struct AuthPassword : public boost::msm::front::state<> {};
    struct TlsStart : public boost::msm::front::state<> {};
    
    // 设置初始状态
    typedef Init initial_state;
    
    // 转换动作
    struct TransitionAction {
        template <class EVT, class FSM, class SourceState, class TargetState>
        void operator()(EVT const& evt, FSM& fsm, SourceState& src, TargetState& tgt) {
            // 在状态转换时更新上下文
            fsm.context.state = convertState<TargetState>();
        }
    };
    
    // 转换守卫
    struct TransitionGuard {
        template <class EVT, class FSM, class SourceState, class TargetState>
        bool operator()(EVT const& evt, FSM& fsm, SourceState& src, TargetState& tgt) {
            return true; // 实际实现中根据具体条件判断
        }
    };
    
    // 状态机转换表
    struct transition_table : boost::mpl::vector<
        //    Start         Event           Next            Action           Guard
        boost::msm::front::Row < Init,       EhloEvent,      Ehlo,         TransitionAction, TransitionGuard >,
        boost::msm::front::Row < Ehlo,       MailEvent,      MailFrom,     TransitionAction, TransitionGuard >,
        boost::msm::front::Row < MailFrom,   RcptEvent,      RcptTo,       TransitionAction, TransitionGuard >,
        boost::msm::front::Row < RcptTo,     DataEvent,      DataContent,  TransitionAction, TransitionGuard >,
        boost::msm::front::Row < DataContent, DataEndEvent,  Ehlo,         TransitionAction, TransitionGuard >,
        boost::msm::front::Row < Ehlo,       AuthEvent,      Auth,         TransitionAction, TransitionGuard >,
        boost::msm::front::Row < Auth,       AuthDataEvent,  AuthUsername, TransitionAction, TransitionGuard >,
        boost::msm::front::Row < AuthUsername, AuthDataEvent, AuthPassword, TransitionAction, TransitionGuard >,
        boost::msm::front::Row < Ehlo,       StartTlsEvent,  TlsStart,     TransitionAction, TransitionGuard >
    > {};
    
    // 处理未定义的转换
    template <class FSM, class Event>
    void no_transition(Event const& e, FSM&, int state) {
        // 处理未定义的转换
    }
    
    // 状态转换辅助函数
    template <typename State>
    static SmtpsState convertState() {
        if constexpr (std::is_same_v<State, Init>) return SmtpsState::INIT;
        else if constexpr (std::is_same_v<State, Ehlo>) return SmtpsState::EHLO;
        else if constexpr (std::is_same_v<State, MailFrom>) return SmtpsState::MAIL_FROM;
        else if constexpr (std::is_same_v<State, RcptTo>) return SmtpsState::RCPT_TO;
        else if constexpr (std::is_same_v<State, Data>) return SmtpsState::DATA;
        else if constexpr (std::is_same_v<State, DataContent>) return SmtpsState::DATA_CONTENT;
        else if constexpr (std::is_same_v<State, Auth>) return SmtpsState::AUTH;
        else if constexpr (std::is_same_v<State, AuthUsername>) return SmtpsState::AUTH_USERNAME;
        else if constexpr (std::is_same_v<State, AuthPassword>) return SmtpsState::AUTH_PASSWORD;
        else if constexpr (std::is_same_v<State, TlsStart>) return SmtpsState::TLS_START;
        else return SmtpsState::INIT;
    }
    
    // 成员变量
    SmtpsContext& context;
    std::shared_ptr<DBPool> dbPool;
    
    // 构造函数
    explicit SmtpsMsmDef(SmtpsContext& ctx) : context(ctx) {}
};

// MSM状态机后端
using SmtpsMsm = boost::msm::back::state_machine<SmtpsMsmDef>;

// Boost.MSM实现的SMTP状态机
class BoostMsmSmtpsFsm : public SmtpsFsm {
public:
    BoostMsmSmtpsFsm();
    ~BoostMsmSmtpsFsm() override = default;
    
    // 实现基类接口
    void initialize(std::shared_ptr<DBPool> dbPool) override;
    std::pair<int, std::string> processEvent(SmtpsEvent event, const std::string& args, SmtpsContext& context) override;
    
    // 辅助方法：将SmtpsEvent转换为相应的事件类型
    void dispatchEvent(SmtpsEvent event, SmtpsContext& context);
    SmtpsState getCurrentState(const SmtpsContext& context) const override;
    void reset(SmtpsContext& context) override;
    bool authenticateUser(const std::string& username, const std::string& password) override;
    bool saveMailToDB(const SmtpsContext& context) override;
    bool isValidEmailAddress(const std::string& email) override;

private:
    std::unique_ptr<SmtpsMsm> m_stateMachine;
    std::shared_ptr<DBPool> m_dbPool;
    SmtpsContext m_context;
};

} // namespace SMTPS
} // namespace mail_system

#endif // MAIL_SYSTEM_BOOST_MSM_SMTPS_FSM_H