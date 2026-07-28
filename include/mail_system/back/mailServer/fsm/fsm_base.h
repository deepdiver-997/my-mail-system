#ifndef MAIL_SYSTEM_FSM_BASE_H
#define MAIL_SYSTEM_FSM_BASE_H

#include "mail_system/back/mailServer/session/session_base.h"
#include <functional>
#include <map>
#include <memory>

namespace mail_system {

template <typename ConnectionType, typename State, typename Event>
class FsmBase {
public:
    using Handler = std::function<void(
        std::shared_ptr<SessionBase<ConnectionType>>, const std::string&)>;

    virtual ~FsmBase() = default;

protected:
    using TransitionTable = std::map<std::pair<State, Event>, State>;
    using HandlerMap      = std::map<State, std::map<Event, Handler>>;

    TransitionTable transition_table_;
    HandlerMap      state_handlers_;

    void add_transition(State from, Event on, State to) {
        transition_table_[{from, on}] = to;
    }
    void add_handler(State s, Event e, Handler h) {
        state_handlers_[s][e] = std::move(h);
    }

    bool dispatch(std::shared_ptr<SessionBase<ConnectionType>> session,
                  State current_state, Event event,
                  const std::string& args)
    {
        if (is_terminal_state(current_state)) {
            session->close();
            return true;
        }
        auto trans_it = transition_table_.find({current_state, event});
        if (trans_it == transition_table_.end()) {
            on_invalid_transition(current_state, event, session, args);
            return true;
        }
        auto state_it = state_handlers_.find(current_state);
        if (state_it == state_handlers_.end()) {
            on_handler_not_found(current_state, event, session, args);
            return true;
        }
        auto ev_it = state_it->second.find(event);
        if (ev_it == state_it->second.end()) {
            on_handler_not_found(current_state, event, session, args);
            return true;
        }
        pre_dispatch(current_state, event, session, args);
        invoke_handler(ev_it->second, session, args);
        return true;
    }

    virtual bool is_terminal_state(State s) const = 0;
    virtual void on_invalid_transition(State s, Event e,
        std::shared_ptr<SessionBase<ConnectionType>> session,
        const std::string& args) = 0;
    virtual void on_handler_not_found(State s, Event e,
        std::shared_ptr<SessionBase<ConnectionType>> session,
        const std::string& args) {}
    virtual void pre_dispatch(State s, Event e,
        std::shared_ptr<SessionBase<ConnectionType>> session,
        const std::string& args) {}
    virtual void invoke_handler(Handler& h,
        std::shared_ptr<SessionBase<ConnectionType>> session,
        const std::string& args)
    {
        h(session, args);
    }
};

} // namespace mail_system

#endif // MAIL_SYSTEM_FSM_BASE_H
