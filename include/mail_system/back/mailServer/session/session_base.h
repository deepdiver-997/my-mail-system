#ifndef SESSION_BASE_H
#define SESSION_BASE_H

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <mail_system/back/entities/mail.h>
#include <mail_system/back/entities/usr.h>
#include <mail_system/back/mailServer/server_base.h>

// #define _LIBCPP_STD_VER 17

namespace mail_system {
class ServerBase;
class SessionBase : public std::enable_shared_from_this<SessionBase> {
public:
    // 构造函数
    SessionBase(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> &&, ServerBase*);

    // 虚析构函数
    virtual ~SessionBase();

    // 启动会话
    virtual void start() = 0;

    // 关闭会话
    virtual void close();

    // 获取客户端地址
    std::string get_client_ip() const;

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& get_ssl_socket();

    mail* get_mail();
    
    // 设置服务器引用
    void set_server(ServerBase* server);

    // 执行SSL握手
    void do_handshake(std::function<void(std::weak_ptr<SessionBase> session, const boost::system::error_code&)> callback);


    // 异步读取数据
    void async_read(std::function<void(const boost::system::error_code&, std::size_t)> callback = nullptr);

    // 异步写入数据
    void async_write(const std::string& data, std::function<void(const boost::system::error_code&)> callback = nullptr);

    // 处理接收到的数据（由派生类实现）
    virtual void handle_read(const std::string& data) = 0;

    // 处理错误
    virtual void handle_error(const boost::system::error_code& error);

    // 会话是否已关闭
    bool is_closed() const;

protected:

    // // IO上下文引用
    // boost::asio::io_context& m_io_context;
    
    // // SSL上下文引用
    // boost::asio::ssl::context& m_ssl_context;
    
    // SSL流
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> m_socket;

    // 读取缓冲区
    std::vector<char> read_buffer_, use_buffer_;

    // 客户端地址
    mutable std::string client_address_;

    std::unique_ptr<mail> mail_;

    std::unique_ptr<usr> usr_;

    // 会话是否已关闭
    bool closed_;
    public:
    // 指向服务器的指针，用于访问IO线程池
    ServerBase* m_server;
};

} // namespace mail_system

#endif // SESSION_BASE_H