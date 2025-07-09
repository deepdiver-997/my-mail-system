#ifndef MAIL_SYSTEM_TRADITIONAL_SMTPS_FSM_H
#define MAIL_SYSTEM_TRADITIONAL_SMTPS_FSM_H

#include "SmtpsFsm.h"
#include <unordered_map>
#include <functional>

namespace mail_system {
namespace SMTPS {

class TraditionalSmtpsFsm : public SmtpsFsm {
public:
    TraditionalSmtpsFsm();
    ~TraditionalSmtpsFsm() override = default;

    // 实现基类接口
    void initialize(std::shared_ptr<DBPool> dbPool) override;
    std::pair<int, std::string> processEvent(SmtpsEvent event, const std::string& args, SmtpsContext& context) override;
    SmtpsState getCurrentState(const SmtpsContext& context) const override;
    void reset(SmtpsContext& context) override;
    bool authenticateUser(const std::string& username, const std::string& password) override;
    bool saveMailToDB(const SmtpsContext& context) override;
    bool isValidEmailAddress(const std::string& email) override;

private:
    // 状态处理函数类型
    using StateHandler = std::function<std::pair<int, std::string>(const std::string&, SmtpsContext&)>;
    
    // 状态转换表类型
    using StateTransitionMap = std::unordered_map<
        SmtpsState,
        std::unordered_map<SmtpsEvent, StateHandler>
    >;

    // 初始化状态转换表
    void initializeStateTransitions();

    // 状态处理函数
    std::pair<int, std::string> handleInit(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleEhlo(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleMailFrom(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleRcptTo(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleData(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleDataContent(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleAuth(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleAuthUsername(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleAuthPassword(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleTlsStart(const std::string& args, SmtpsContext& context);
    std::pair<int, std::string> handleQuit(const std::string& args, SmtpsContext& context);

    // 辅助函数
    std::pair<int, std::string> invalidTransition(const std::string& args, SmtpsContext& context);
    bool isValidTransition(SmtpsState currentState, SmtpsEvent event) const;

    // 成员变量
    StateTransitionMap m_stateTransitions;
    std::shared_ptr<DBPool> m_dbPool;
};

} // namespace SMTPS
} // namespace mail_system

#endif // MAIL_SYSTEM_TRADITIONAL_SMTPS_FSM_H