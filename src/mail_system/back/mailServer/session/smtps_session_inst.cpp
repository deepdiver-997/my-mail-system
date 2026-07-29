// 显式模板实例化 — SmtpsSession
#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/mailServer/session/smtps_session.tpp"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"

namespace mail_system {

template class SmtpsSession<TcpConnection>;
template class SmtpsSession<SslConnection>;

} // namespace mail_system
