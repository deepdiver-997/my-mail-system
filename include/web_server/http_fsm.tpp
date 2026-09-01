#ifndef WEB_SERVER_HTTP_FSM_TPP
#define WEB_SERVER_HTTP_FSM_TPP
// 依赖：需以完整定义可见的方式包含，见 http_fsm_inst.cpp 的 include 顺序。
// 本文件在实例化前依赖 HttpSession<ConnectionType> 的完整定义（serve() 等）。
#include "web_server/http_session.h"
#include "web_server/http_session.tpp"
#include "mail_system/back/common/logger.h"

namespace web_server {

template <typename ConnectionType>
void HttpFsm<ConnectionType>::auto_process_event(
    std::shared_ptr<Session> session)
{
    this->dispatch(session, static_cast<HttpState>(session->get_current_state()),
                   static_cast<HttpEvent>(session->get_next_event()));
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::init_transitions() {
    using H = HttpState; using E = HttpEvent;
    this->add_transition(H::WAIT_REQUEST_LINE, E::REQUEST_LINE, H::WAIT_HEADERS);
    this->add_transition(H::WAIT_HEADERS,      E::HEADER_LINE, H::WAIT_HEADERS);
    this->add_transition(H::WAIT_HEADERS,      E::HEADER_END,  H::WAIT_BODY);   // 有 body
    this->add_transition(H::WAIT_HEADERS,      E::HEADER_END,  H::RESPOND);     // 无 body
    this->add_transition(H::WAIT_BODY,         E::BODY,        H::WAIT_BODY);
    this->add_transition(H::WAIT_BODY,         E::BODY_END,    H::RESPOND);
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::init_handlers() {
    using H = HttpState; using E = HttpEvent;
    this->add_handler(H::WAIT_REQUEST_LINE, E::REQUEST_LINE,
        [this](auto s){ handle_request_line(std::move(s)); });
    this->add_handler(H::WAIT_HEADERS, E::HEADER_LINE,
        [this](auto s){ handle_header_line(std::move(s)); });
    this->add_handler(H::WAIT_HEADERS, E::HEADER_END,
        [this](auto s){ handle_header_end(std::move(s)); });
    this->add_handler(H::WAIT_BODY, E::BODY,
        [this](auto s){ handle_body(std::move(s)); });
    this->add_handler(H::WAIT_BODY, E::BODY_END,
        [this](auto s){ handle_body_end(std::move(s)); });
    // fallback：WAIT_BODY 里收到非预期（畸形/超限帧）→ 400 后关。
    this->add_fallback_handler(H::WAIT_BODY,
        [this](auto s){ on_invalid_transition(H::WAIT_BODY, E::ERROR, std::move(s)); });
    this->add_fallback_handler(H::RESPOND,
        [this](auto s){ on_invalid_transition(H::RESPOND, E::ERROR, std::move(s)); });
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_request_line(
    std::shared_ptr<Session> session)
{
    // 请求行已在 HttpSession::handle_read 里解析进 request_。这里只入下一态。
    session->set_current_state(static_cast<int>(HttpState::WAIT_HEADERS));
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_header_line(
    std::shared_ptr<Session> session)
{
    session->set_current_state(static_cast<int>(HttpState::WAIT_HEADERS));
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_header_end(
    std::shared_ptr<Session> session)
{
    auto* s = static_cast<HttpSession<ConnectionType>*>(session.get());
    if (s->has_body_pending()) {
        s->begin_body();                      // 武装 body 消费模式（extract 改判帧）
        s->set_current_state(static_cast<int>(HttpState::WAIT_BODY));
    } else {
        s->set_current_state(static_cast<int>(HttpState::RESPOND));
        serve(session);
    }
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_body(
    std::shared_ptr<Session> session)
{
    // 本段 body 已在 handle_read 消费；body 未收完，保持 WAIT_BODY。
    session->set_current_state(static_cast<int>(HttpState::WAIT_BODY));
}

template <typename ConnectionType>
void HttpFsm<ConnectionType>::handle_body_end(
    std::shared_ptr<Session> session)
{
    auto* s = static_cast<HttpSession<ConnectionType>*>(session.get());
    s->set_current_state(static_cast<int>(HttpState::RESPOND));
    s->serve();
}

// 响应：转交 HttpSession（它持连接/写原语/请求上下文）。
template <typename ConnectionType>
void HttpFsm<ConnectionType>::serve(
    std::shared_ptr<Session> session)
{
    static_cast<HttpSession<ConnectionType>*>(session.get())->serve();
}

// 非法转换：畸形请求 → 400 并关连接。
template <typename ConnectionType>
void HttpFsm<ConnectionType>::on_invalid_transition(HttpState /*s*/, HttpEvent /*e*/,
    std::shared_ptr<Session> session)
{
    auto* s = static_cast<HttpSession<ConnectionType>*>(session.get());
    s->send_error_and_close(400);
}

} // namespace web_server

#endif // WEB_SERVER_HTTP_FSM_TPP