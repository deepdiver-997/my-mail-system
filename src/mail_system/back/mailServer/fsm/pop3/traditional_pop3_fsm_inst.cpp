// 显式模板实例化 — TraditionalPop3Fsm
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.h"
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace mail_system {

template class TraditionalPop3Fsm<TcpConnection>;
template class TraditionalPop3Fsm<SslConnection>;

} // namespace mail_system
