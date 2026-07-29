// 显式模板实例化 — SessionBase
// 避免每个翻译单元重复编译 session_base.tpp（278 行）
#include "framework/session_base.h"
#include "framework/session_base.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace mail_system {

template class SessionBase<TcpConnection>;
template class SessionBase<SslConnection>;

} // namespace mail_system
