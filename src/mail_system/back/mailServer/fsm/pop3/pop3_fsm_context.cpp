#include "mail_system/back/mailServer/fsm/pop3/pop3_fsm_context.h"

namespace mail_system {

Pop3FsmContext::Pop3FsmContext() : fsm_() {
    fsm_.init();
}

Pop3FsmContext::~Pop3FsmContext() = default;

void Pop3FsmContext::handleEvent(Pop3Event event) {
    fsm_.handleEvent(event);
}

Pop3State Pop3FsmContext::getCurrentState() const {
    return fsm_.getCurrentState();
}

} // namespace mail_system