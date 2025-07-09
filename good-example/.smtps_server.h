#ifndef MAIL_SYSTEM_SMTPS_SERVER_H
#define MAIL_SYSTEM_SMTPS_SERVER_H

#include "../db_pool.h"
#include "../thread_pool/thread_pool_base.h"
#include "../thread_pool/boost_thread_pool.h"
#include "../thread_pool/io_thread_pool.h"
#include "fsm/SmtpsFsm.h"
#include "fsm/TraditionalSmtpsFsm.h"
#include "fsm/BoostMsmSmtpsFsm.h"
#include <vmime/vmime.hpp>
#include <vmime/platforms/posix/posixHandler.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <unordered_map>
#include <atomic>

namespace mail_system {

// SMTP服务器配置
struct SMTPConfig {
    std::string address;
    int port;
    std::string certFile;
    std::string keyFile;
    size_t maxMessageSize;
    size_t maxRecipients;
    bool requireAuth;
    bool useMSM;  // 是否使用Boost.MSM状态机
    
    // 线程池配置
    size_t io_thread_count = 2;     // IO线程池线程数
    size_t worker_thread_count = 4; // 工作线程池线程数
};

// 客户端会话
struct ClientSession {
    std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> socket;  // SSL套接字
    SMTPS::SmtpsContext context;  // 使用新的状态机上下文
    
    // 默认构造函数
    ClientSession() = default;
    
    // 移动构造函数
    ClientSession(std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> &&s) 
        : socket(std::move(s)) {}
        
    // 复制构造函数
    ClientSession(const ClientSession&) = delete;
    
    // 移动赋值操作符
    ClientSession& operator=(ClientSession&&) = default;
    
    // 复制赋值操作符
    ClientSession& operator=(const ClientSession&) = delete;
};

class SMTPSServer {
public:
    SMTPSServer(const SMTPConfig& config, std::shared_ptr<DBPool> dbPool);
    ~SMTPSServer();
    
    void start();
    void stop();
    
    // 获取线程池
    std::shared_ptr<IOThreadPool> getIOThreadPool() const { return m_ioThreadPool; }
    std::shared_ptr<BoostThreadPool> getWorkerThreadPool() const { return m_workerThreadPool; }

private:
    // 命令处理器类型
    using CommandHandler = std::function<void(ClientSession&, const std::string&)>;
    
    // 成员变量
    SMTPConfig m_config;
    std::shared_ptr<DBPool> m_dbPool;
    std::atomic<bool> m_running;
    
    // 线程池
    std::shared_ptr<IOThreadPool> m_ioThreadPool;
    std::shared_ptr<BoostThreadPool> m_workerThreadPool;
    
    // 新增：状态机
    std::unique_ptr<SMTPS::SmtpsFsm> m_stateMachine;
    
    // Boost.ASIO相关
    std::shared_ptr<boost::asio::io_context> m_ioContext;
    boost::asio::ssl::context m_sslContext;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;
    
    // 监听线程
    std::thread m_listenerThread;
    
    // 私有方法
    void listener_thread_func();
    void accept_connection();
    void handle_client(ClientSession& session);
    void read_command(ClientSession& session, std::shared_ptr<boost::asio::streambuf> buffer);
    void send_async_response(const ClientSession& session, int code, const std::string& message);
    void initCommandHandlers();
    
    // 辅助方法
    bool upgradeToTLS(ClientSession& session);
    bool authenticateUser(const std::string& username, const std::string& password);
    void saveMailToDB(const ClientSession& session);
    void resetSession(ClientSession& session);
    bool isValidEmailAddress(const std::string& email);
    
    // 新增：解析SMTP命令
    void parse_smtp_command(const std::string& command, ClientSession& session);
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SMTPS_SERVER_H