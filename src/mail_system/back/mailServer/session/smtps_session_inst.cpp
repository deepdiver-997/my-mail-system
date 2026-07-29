// 显式模板实例化 — SmtpsSession
#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/mailServer/session/smtps_session.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace mail_system {

template class SmtpsSession<TcpConnection>;
template class SmtpsSession<SslConnection>;

} // namespace mail_system
