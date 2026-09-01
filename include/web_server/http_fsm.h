#ifndef WEB_SERVER_HTTP_FSM_H
#define WEB_SERVER_HTTP_FSM_H
// ──────────────────────────────────────────────────────────────────
// HttpFsm — HTTP/1.1 状态机（FastFsmBase 派生的核心）
//
// 状态图（keep-alive 闭环）：
//
//   WAIT_REQUEST_LINE ──REQUEST_LINE──▶ WAIT_HEADERS
//   WAIT_HEADERS     ──HEADER_LINE──▶  WAIT_HEADERS
//   WAIT_HEADERS     ──HEADER_END──▶   WAIT_BODY    (有 body)
//   WAIT_HEADERS     ──HEADER_END──▶   RESPOND      (无 body)
//   WAIT_BODY        ──BODY──▶         WAIT_BODY
//   WAIT_BODY        ──BODY_END──▶     RESPOND
//   RESPOND          ──(写回调)──▶     WAIT_REQUEST_LINE  (keep-alive)
//   任意            ──TIMEOUT/ERROR──▶ CLOSED    (终态)
//
// handler 与 SMTP 同一签名：void(std::shared_ptr<mail_system::SessionBase<...>>)。
// "何时读"由 SessionBase 的 do_async_read + 覆写的 extract_one_line 决定；
// "这一块怎么解析"在 HttpSession::handle_read；本类只做状态流转 + 触发响应。
// ──────────────────────────────────────────────────────────────────
#include "framework/session_base.h"   // 先于 fast_fsm_base.h：后者用未限定的 SessionBase
#include "framework/fast_fsm_base.h"
#include "web_server/http_types.hpp"
#include <memory>

namespace web_server {

template <typename ConnectionType>
class HttpSession;   // 前向声明

template <typename ConnectionType>
class HttpFsm : public mail_system::FastFsmBase<ConnectionType, HttpState, HttpEvent> {
    using Session = mail_system::SessionBase<ConnectionType>;
public:
    explicit HttpFsm() { init_transitions(); init_handlers(); }
    ~HttpFsm() override = default;

    // Session::process_read 调用的公共入口：按当前态派发 next_event_
    void auto_process_event(std::shared_ptr<Session> session);

protected:
    bool is_terminal_state(HttpState s) const override {
        return s == HttpState::CLOSED;
    }
    void on_invalid_transition(HttpState s, HttpEvent e,
        std::shared_ptr<Session> session) override;

private:
    void init_transitions();
    void init_handlers();

    void handle_request_line(std::shared_ptr<Session> session);
    void handle_header_line(std::shared_ptr<Session> session);
    void handle_header_end(std::shared_ptr<Session> session);
    void handle_body(std::shared_ptr<Session> session);
    void handle_body_end(std::shared_ptr<Session> session);

    // 响应收尾：写响应并处理 keep-alive / 断开，供 header_end / body_end 共用。
    void serve(std::shared_ptr<Session> session);
};

} // namespace web_server

#endif // WEB_SERVER_HTTP_FSM_H