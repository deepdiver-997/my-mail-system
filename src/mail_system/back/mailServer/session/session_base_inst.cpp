// 显式模板实例化 — SessionBase
// 避免每个翻译单元重复编译 session_base.tpp（278 行）
#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/mailServer/session/session_base.tpp"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"

namespace mail_system {

template class SessionBase<TcpConnection>;
template class SessionBase<SslConnection>;

} // namespace mail_system
