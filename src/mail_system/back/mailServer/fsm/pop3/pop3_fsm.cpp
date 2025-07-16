#include "mail_system/back/mailServer/fsm/pop3/pop3_fsm.h"

namespace mail_system {

Pop3Fsm::Pop3Fsm() : currentState_(Pop3State::INITIAL) {}

void Pop3Fsm::init() {
    currentState_ = Pop3State::INITIAL;
}

void Pop3Fsm::handleEvent(Pop3Event event) {
    switch (currentState_) {
        case Pop3State::INITIAL:
            handleInitialState(event);
            break;
        case Pop3State::AUTHORIZATION:
            handleAuthorizationState(event);
            break;
        case Pop3State::TRANSACTION:
            handleTransactionState(event);
            break;
        case Pop3State::UPDATE:
            handleUpdateState(event);
            break;
        case Pop3State::ERROR:
            handleErrorState(event);
            break;
        default:
            break;
    }
}

Pop3State Pop3Fsm::getCurrentState() const {
    return currentState_;
}

void Pop3Fsm::transitionTo(Pop3State newState) {
    currentState_ = newState;
}

// 状态处理函数实现
void Pop3Fsm::handleInitialState(Pop3Event event) {
    // 实现初始状态处理逻辑
}

void Pop3Fsm::handleAuthorizationState(Pop3Event event) {
    // 实现授权状态处理逻辑
}

void Pop3Fsm::handleTransactionState(Pop3Event event) {
    // 实现事务状态处理逻辑
}

void Pop3Fsm::handleUpdateState(Pop3Event event) {
    // 实现更新状态处理逻辑
}

void Pop3Fsm::handleErrorState(Pop3Event event) {
    // 实现错误状态处理逻辑
}

} // namespace mail_system