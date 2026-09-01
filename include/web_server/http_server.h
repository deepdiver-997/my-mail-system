#ifndef WEB_SERVER_HTTP_SERVER_H
#define WEB_SERVER_HTTP_SERVER_H
// ──────────────────────────────────────────────────────────────────
// HttpServer — HTTP/1.1 静态文件服务器装配
//
// 直接复用 TcpServerBase（acceptor+session 工厂），对比 smtps_server 只少
// 了 DB/队列/DNS 依赖：HttpFsm 不需要线程池，纯读+静态文件发即可。
// 唯一新增：doc_root（静态根目录），在工厂里注入到每个 session。
// ──────────────────────────────────────────────────────────────────
#include "framework/tcp_server_base.h"
#include "web_server/http_types.hpp"
#include "web_server/http_fsm.h"
#include "web_server/http_session.h"
#include <memory>
#include <string>

namespace web_server {

class HttpServer : public mail_system::TcpServerBase<TcpHttpSession, SslHttpSession> {
public:
    HttpServer(const mail_system::ServerConfig& config,
               std::string doc_root = ".",
               std::shared_ptr<mail_system::ThreadPoolBase> ioThreadPool = nullptr,
               std::shared_ptr<mail_system::ThreadPoolBase> workerThreadPool = nullptr,
               std::shared_ptr<mail_system::DBPool> dbPool = nullptr);
    ~HttpServer() override = default;

    const std::string& doc_root() const { return doc_root_; }

protected:
    std::shared_ptr<TcpHttpSession> make_tcp_session(
        std::unique_ptr<mail_system::TcpConnection> conn,
        const mail_system::ListenerConfig& lc) override;
    std::shared_ptr<SslHttpSession> make_ssl_session(
        std::unique_ptr<mail_system::SslConnection> conn,
        const mail_system::ListenerConfig& lc) override;
    bool should_reject_connection(std::string& reason,
        const std::string& client_ip = "") const override;

private:
    std::shared_ptr<HttpFsm<mail_system::TcpConnection>> m_tcp_fsm;
    std::shared_ptr<HttpFsm<mail_system::SslConnection>> m_ssl_fsm;
    std::string doc_root_;
};

} // namespace web_server

#endif // WEB_SERVER_HTTP_SERVER_H