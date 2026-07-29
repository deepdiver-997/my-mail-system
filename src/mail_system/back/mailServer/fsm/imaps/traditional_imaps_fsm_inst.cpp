// 显式模板实例化 — TraditionalImapsFsm
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"

namespace mail_system {

template class TraditionalImapsFsm<TcpConnection>;
template class TraditionalImapsFsm<SslConnection>;

} // namespace mail_system
