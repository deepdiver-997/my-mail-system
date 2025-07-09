#ifndef MAIL_SYSTEM_SERVER_BASE_H
#define MAIL_SYSTEM_SERVER_BASE_H

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include "server_config.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/mailServer/session/session_base.h"

namespace mail_system {

class ServerBase {
public:
    ServerBase(const ServerConfig& config);
    virtual ~ServerBase();

    // 启动服务器
    void start();
    // 停止服务器
    void stop();
    // 是否正在运行
    bool is_running() const;
    // 发送异步响应
    void send_async_response(std::weak_ptr<SessionBase> session, const std::string& response);

protected:
    // 创建新会话
    virtual void accept_connection();
    // 处理新连接
    virtual void handle_accept(std::shared_ptr<SessionBase> session, const boost::system::error_code& error) = 0;

    // 获取IO上下文
    std::shared_ptr<boost::asio::io_context> get_io_context();
    // 获取SSL上下文
    boost::asio::ssl::context& get_ssl_context();
    // 获取接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> get_acceptor();

    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;

private:
    // 加载SSL证书
    void load_certificates(const std::string& cert_file, const std::string& key_file);

    // IO上下文
    std::shared_ptr<boost::asio::io_context> m_ioContext;
    // SSL上下文
    boost::asio::ssl::context m_sslContext;
    // 接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;
    // 是否正在运行
    std::atomic<bool> m_running;
    // 监听线程
    std::thread m_listenerThread;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SERVER_BASE_H