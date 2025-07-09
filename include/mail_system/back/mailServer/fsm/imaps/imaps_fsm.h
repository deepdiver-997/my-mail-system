#ifndef MAIL_SYSTEM_IMAPS_FSM_H
#define MAIL_SYSTEM_IMAPS_FSM_H

#include <functional>
#include <map>
#include <boost/msm/front/state_machine_def.hpp>
#include <boost/msm/front/functor_row.hpp>
#include <boost/msm/back/state_machine.hpp>
#include <boost/mpl/vector.hpp>

namespace mail_system {

enum class ImapsState {
    INITIAL,
    NOT_AUTHENTICATED,
    AUTHENTICATED,
    SELECTED,
    LOGOUT,
    ERROR
};

enum class ImapsEvent {
    CONNECT,
    LOGIN,
    SELECT,
    LOGOUT,
    COMMAND
};

class ImapsContext {
public:
    ImapsContext() : authenticated(false), selected_mailbox("") {}
    bool authenticated;  // 是否已认证
    std::string selected_mailbox;  // 当前选择的邮箱
};

class ImapsFsm {
public:
    ImapsFsm() = default;
    virtual ~ImapsFsm();

    // 处理事件
    void processEvent(const ImapsEvent& event, const ImapsContext& context);
    // 获取当前状态
};


} // namespace mail_system

#endif // MAIL_SYSTEM_IMAPS_FSM_H