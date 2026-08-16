#ifndef MAIL_SYSTEM_TCP_SERVER_BASE_H
#define MAIL_SYSTEM_TCP_SERVER_BASE_H

#include "server_base.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace mail_system {

// ================================================================
// TcpServerBase — TCP/SSL 监听服务器模板
//
//   子类只需提供 FSM + Session 类型、实现 make_*_session 工厂。
//   模板参数: TcpSession 和 SslSession 显式传入，避免基类耦合协议类型。
// ================================================================
template <typename TcpSession, typename SslSession>
class TcpServerBase : public ServerBase {
public:
    TcpServerBase(const ServerConfig& config,
                  std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
                  std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
                  std::shared_ptr<DBPool> dbPool = nullptr);
    ~TcpServerBase() override;

    void start() override;

    std::shared_ptr<boost::asio::io_context> get_io_context() const override { return m_ioContext; }
    boost::asio::ssl::context& get_ssl_context() { return m_sslContext; }

protected:
    void stop(ServerState state = ServerState::Pausing) override;

    // 子类实现：如何创建一个 TCP/SSL session
    virtual std::shared_ptr<TcpSession> make_tcp_session(
        std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc) = 0;
    virtual std::shared_ptr<SslSession> make_ssl_session(
        std::unique_ptr<SslConnection> conn, const ListenerConfig& lc) = 0;

public:
    // STARTTLS 升级处理（子类可覆写）
    virtual void handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket,
                                         std::string trace = {});

    void load_certificates(const std::string& cert_file, const std::string& key_file,
                           const std::string& dh_file = "", bool enable_tls1_3 = true);

    // 监听器配置
    std::unordered_map<uint16_t, ListenerConfig> m_listener_configs;

private:
    void start_all_acceptors();
    void do_tcp_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc);
    void do_ssl_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc);

    std::shared_ptr<boost::asio::io_context> m_ioContext;
    boost::asio::ssl::context m_sslContext;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::acceptor>> m_tcp_acceptors;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::acceptor>> m_ssl_acceptors;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> m_workGuard;
    std::thread m_listenerThread;
};

// ================================================================
// 实现
// ================================================================
template <typename TcpSession, typename SslSession>
TcpServerBase<TcpSession, SslSession>::TcpServerBase(
    const ServerConfig& config,
    std::shared_ptr<ThreadPoolBase> ioThreadPool,
    std::shared_ptr<ThreadPoolBase> workerThreadPool,
    std::shared_ptr<DBPool> dbPool)
    : ServerBase(config, ioThreadPool, workerThreadPool, dbPool)
    , m_sslContext(boost::asio::ssl::context::sslv23)
{
    auto cfg = std::atomic_load(&m_config);
    m_ioContext = std::make_shared<boost::asio::io_context>();
    m_workGuard = std::make_unique<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>(m_ioContext->get_executor());

    auto addr = boost::asio::ip::make_address(config.address);

    bool has_ssl = false;
    for (auto& l : config.listeners)
        if (l.type == ListenerType::SSL) { has_ssl = true; break; }

    if (has_ssl) {
        load_certificates(config.certFile, config.keyFile, config.dhFile,
                          config.enable_tls1_3);
    }

    for (auto& l : config.listeners) {
        m_listener_configs[l.port] = l;
        auto acc = std::make_shared<boost::asio::ip::tcp::acceptor>(*m_ioContext);
        acc->open(boost::asio::ip::tcp::v4());
        acc->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acc->bind(boost::asio::ip::tcp::endpoint(addr, l.port));
        acc->listen();
        if (l.type == ListenerType::SSL)
            m_ssl_acceptors.push_back(acc);
        else
            m_tcp_acceptors.push_back(acc);
        LOG_SERVER_INFO("{} acceptor on {}:{}",
                       listener_type_to_string(l.type), config.address, l.port);
    }
}

template <typename TcpSession, typename SslSession>
TcpServerBase<TcpSession, SslSession>::~TcpServerBase() {
    stop();
}

template <typename TcpSession, typename SslSession>
void TcpServerBase<TcpSession, SslSession>::start() {
    if (m_state.load() != ServerState::Stopped) {
        m_state.store(ServerState::Running);
        start_metrics_server();

        m_listenerThread = std::thread([this]() {
            start_all_acceptors();
            m_ioContext->run();
        });

        LOG_SERVER_INFO("TCP server started with {} listener(s)",
                        m_listener_configs.size());
    }
}

template <typename TcpSession, typename SslSession>
void TcpServerBase<TcpSession, SslSession>::stop(ServerState state) {
    auto cur = m_state.load();
    if (cur != ServerState::Running && cur != ServerState::Pausing) return;
    m_state.store(state);

    // 1. 释放 work guard → io_context::run() 可退出
    m_workGuard.reset();

    // 2. 关闭 acceptor
    boost::system::error_code ec;
    for (auto& a : m_ssl_acceptors) a->close(ec);
    for (auto& a : m_tcp_acceptors) a->close(ec);

    // 3. 停 io_context → run() 返回 → join 线程
    if (m_ioContext && !m_ioContext->stopped()) m_ioContext->stop();
    if (m_listenerThread.joinable()) m_listenerThread.join();
    LOG_SERVER_INFO("Listener thread stopped");

    // 4. 基类清理（metrics、线程池等）
    ServerBase::stop(state);
}

template <typename TcpSession, typename SslSession>
void TcpServerBase<TcpSession, SslSession>::start_all_acceptors() {
    for (auto& acc : m_ssl_acceptors) {
        uint16_t port = acc->local_endpoint().port();
        do_ssl_accept(acc, m_listener_configs[port]);
    }
    for (auto& acc : m_tcp_acceptors) {
        uint16_t port = acc->local_endpoint().port();
        do_tcp_accept(acc, m_listener_configs[port]);
    }
}

template <typename TcpSession, typename SslSession>
void TcpServerBase<TcpSession, SslSession>::do_tcp_accept(
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc)
{
    if (get_state() != ServerState::Running) return;

    // 从 IO 线程池获取 io_context 做负载分散，避免所有连接挤在主线程的 io_context 上
    auto& pool_io = static_cast<IOThreadPool*>(m_ioThreadPool.get())->get_io_context();
    auto sock = std::make_unique<boost::asio::ip::tcp::socket>(pool_io);
    // 预先取裸指针：lambda 捕获时 sock 被 move 进闭包，取地址发生在捕获之前，
    // 否则某些平台/编译器会先移动 sock 再取地址导致 psock 指向已移动的对象
    auto* psock = sock.get();

    acceptor->async_accept(*psock, [this, acceptor, lc,
        sock = std::move(sock)](const boost::system::error_code& ec) mutable {
        if (!ec) {
            auto ip = sock->remote_endpoint().address().to_string();
            if (is_ip_banned(ip)) {
                LOG_NETWORK_WARN("Banned IP {} rejected at accept", ip);
                increment_connections_rejected();
                boost::system::error_code ign;
                sock->close(ign);
            } else {
                std::string reason;
                if (should_reject_connection(reason, ip)) {
                    LOG_NETWORK_WARN("Rejecting TCP connection from {}: {}", ip, reason);
                    increment_connections_rejected();
                    boost::system::error_code ign;
                    sock->close(ign);
                } else {
                    LOG_NETWORK_INFO("New TCP connection accepted from {}", ip);
                    increment_connections_total();
                    auto conn = std::make_unique<TcpConnection>(std::move(sock));
                    auto session = make_tcp_session(std::move(conn), lc);
                    if (session) {
                        increment_connection_count();
                        TcpSession::start(session);
                    }
                }
            }
        }
        do_tcp_accept(acceptor, lc);
    });
}

template <typename TcpSession, typename SslSession>
void TcpServerBase<TcpSession, SslSession>::do_ssl_accept(
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor, const ListenerConfig& lc)
{
    if (get_state() != ServerState::Running) return;

    // 从 IO 线程池获取 io_context 做负载分散（同上）
    auto& pool_io = static_cast<IOThreadPool*>(m_ioThreadPool.get())->get_io_context();
    auto sock = std::make_unique<boost::asio::ip::tcp::socket>(pool_io);
    auto ssl_sock = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
        std::move(*sock), get_ssl_context());
    auto& lowest = ssl_sock->next_layer();

    acceptor->async_accept(lowest, [this, acceptor, lc,
        ssl_sock = std::move(ssl_sock)](const boost::system::error_code& ec) mutable {
        if (!ec) {
            auto ip = ssl_sock->next_layer().remote_endpoint().address().to_string();
            if (is_ip_banned(ip)) {
                increment_connections_rejected();
                boost::system::error_code ign;
                ssl_sock->next_layer().close(ign);
            } else {
                std::string reason;
                if (should_reject_connection(reason, ip)) {
                    increment_connections_rejected();
                    boost::system::error_code ign;
                    ssl_sock->next_layer().close(ign);
                } else {
                    increment_connections_total();
                    auto conn = std::make_unique<SslConnection>(std::move(ssl_sock));
                    auto session = make_ssl_session(std::move(conn), lc);
                    if (session) {
                        increment_connection_count();
                        SslSession::start(session);
                    }
                }
            }
        }
        do_ssl_accept(acceptor, lc);
    });
}

template <typename TcpSession, typename SslSession>
void TcpServerBase<TcpSession, SslSession>::handoff_starttls_socket(
    std::unique_ptr<boost::asio::ip::tcp::socket>&& socket, std::string trace)
{
    if (!socket) return;
    auto ssl_stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
        std::move(*socket), get_ssl_context());
    auto conn = std::make_unique<SslConnection>(std::move(ssl_stream));
    auto session = make_ssl_session(std::move(conn), ListenerConfig{});
    if (session) {
        increment_connection_count();
        if (!trace.empty()) session->set_trace_buffer(std::move(trace));  // 延续旧会话的对话记录
        SslSession::start_after_starttls(session);
    }
}

template <typename TcpSession, typename SslSession>
void TcpServerBase<TcpSession, SslSession>::load_certificates(
    const std::string& cert_file, const std::string& key_file, const std::string& dh_file,
    bool enable_tls1_3)
{
    try {
        if (!std::ifstream(cert_file.c_str()).good())
            throw std::runtime_error("Certificate file not found: " + cert_file);
        if (!std::ifstream(key_file.c_str()).good())
            throw std::runtime_error("Private key file not found: " + key_file);
        m_sslContext.use_certificate_chain_file(cert_file);
        m_sslContext.use_private_key_file(key_file, boost::asio::ssl::context::pem);
        // Gmail 用 TLS 1.3 ClientHello 且无回退机制：若服务器强制 TLS 1.2，
        // Gmail 在 STARTTLS 阶段直接 RST（TLS Negotiation failed, error 104）。
        // 默认启用 TLS 1.3，可通过配置 enable_tls1_3=false 强制 TLS 1.2 回退。
        if (!enable_tls1_3) {
            m_sslContext.set_options(boost::asio::ssl::context::no_tlsv1_3);
        }
        if (!dh_file.empty() && std::ifstream(dh_file.c_str()).good()) {
            m_sslContext.use_tmp_dh_file(dh_file);
        }
        LOG_SERVER_INFO("SSL certificates loaded successfully");
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error loading SSL certificates: {}", e.what());
        throw;
    }
}

} // namespace mail_system

#endif // MAIL_SYSTEM_TCP_SERVER_BASE_H
