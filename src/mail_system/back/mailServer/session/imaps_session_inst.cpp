// 显式模板实例化 — ImapsSession
#include "mail_system/back/mailServer/session/imaps_session.h"
#include "mail_system/back/mailServer/session/imaps_session.tpp"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"

namespace mail_system {

template class ImapsSession<TcpConnection>;
template class ImapsSession<SslConnection>;

} // namespace mail_system
