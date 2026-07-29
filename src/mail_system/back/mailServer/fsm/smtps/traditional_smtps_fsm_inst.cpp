// 显式模板实例化 — TraditionalSmtpsFsm
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"

namespace mail_system {

template class TraditionalSmtpsFsm<TcpConnection>;
template class TraditionalSmtpsFsm<SslConnection>;

} // namespace mail_system
