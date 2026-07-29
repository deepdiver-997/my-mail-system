// 显式模板实例化 — TraditionalImapsFsm
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace mail_system {

template class TraditionalImapsFsm<TcpConnection>;
template class TraditionalImapsFsm<SslConnection>;

} // namespace mail_system
