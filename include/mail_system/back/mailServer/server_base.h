#ifndef MAIL_SYSTEM_SERVER_BASE_H
#define MAIL_SYSTEM_SERVER_BASE_H

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include "server_config.h"

#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/thread_pool/io_thread_pool.h"
#include "mail_system/back/thread_pool/boost_thread_pool.h"

#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/mailServer/session/session_base.h"

namespace mail_system {

enum class ServerState {
    Stopped,    // 未启动或完全关闭
    Running,    // 正常服务
    Pausing,    // 停止接受新连接，但处理存量请求
    Paused      // 完全暂停（无新连接，无请求处理，随时可以启动服务）
};


class ServerBase {
class SessionBase;
public:
    ServerBase(const ServerConfig& config,
         std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
         std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
         std::shared_ptr<DBPool> dbPool = nullptr); //allowing sharing pool with other servers
    virtual ~ServerBase();

    // 启动服务器
    void start();
    // 停止服务器
    void stop(ServerState state = ServerState::Pausing);
    // 是否正在运行
    ServerState get_state() const;
    // 发送异步响应
    void send_async_response(std::weak_ptr<SessionBase> session, const std::string& response);

    // 创建新会话
    virtual void accept_connection();
    // 处理新连接
    virtual void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket >>&& ssl_socket, const boost::system::error_code& error) = 0;

    // 获取IO上下文
    std::shared_ptr<boost::asio::io_context> get_io_context();
    // 获取SSL上下文
    boost::asio::ssl::context& get_ssl_context();
    // 获取接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> get_acceptor();

public:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;
    bool ssl_in_worker;

private:
    // 加载SSL证书
    void load_certificates(const std::string& cert_file, const std::string& key_file, const std::string& dh_file = "");

    boost::asio::ip::tcp::endpoint m_endpoint;
    // IO上下文
    std::shared_ptr<boost::asio::io_context> m_ioContext;
    // SSL上下文
    boost::asio::ssl::context m_sslContext;
    // 接受器
    std::shared_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;
    // 工作守卫
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_workGuard;
    // 是否正在运行
    std::atomic<bool> has_listener_thread;
    // 监听线程
    std::thread m_listenerThread;
    // 服务器状态
    std::atomic<ServerState> m_state;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SERVER_BASE_H