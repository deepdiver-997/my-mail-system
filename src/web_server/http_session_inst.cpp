// 显式模板实例化 — HttpSession（TCP + SSL）
#include "web_server/http_session.h"
#include "web_server/http_session.tpp"
#include "web_server/http_fsm.h"
#include "web_server/http_fsm.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace web_server {

template class HttpSession<mail_system::TcpConnection>;
template class HttpSession<mail_system::SslConnection>;

} // namespace web_server