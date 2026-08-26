// 显式模板实例化 — Pop3Session
#include "mail_system/back/mailServer/session/pop3_session.h"
#include "mail_system/back/mailServer/session/pop3_session.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace mail_system {

template class Pop3Session<TcpConnection>;
template class Pop3Session<SslConnection>;

} // namespace mail_system
