// 显式模板实例化 — H2Session（TCP + SSL）
#include "web_server/h2/h2_session.h"
#include "web_server/h2/h2_session.tpp"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"

namespace web_server {
namespace h2 {

template class H2Session<mail_system::TcpConnection>;
template class H2Session<mail_system::SslConnection>;

} // namespace h2
} // namespace web_server