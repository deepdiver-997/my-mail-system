#include "mail_system/back/mailServer/pop3_server.h"
#include "framework/connection/ssl_connection.h"
#include "framework/connection/tcp_connection.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "mail_system/back/common/logger.h"
#include <chrono>
#include <memory>

namespace mail_system {

namespace {
// 锁清扫周期：每 5min 回收心跳过期(>5min)的锁。与续约周期(60s)留足余量。
constexpr int kLockSweepIntervalSeconds = 300;
} // namespace

Pop3Server::Pop3Server(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> workerThreadPool,
       std::shared_ptr<DBPool> dbPool)
        : TcpServerBase(config, ioThreadPool, workerThreadPool, dbPool) {
    m_tcp_fsm = std::make_shared<TraditionalPop3Fsm<TcpConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    m_ssl_fsm = std::make_shared<TraditionalPop3Fsm<SslConnection>>(
        m_ioThreadPool, m_workerThreadPool, m_shardRouter);
    LOG_SERVER_INFO("POP3 server initialized, SSL fsm={}, TCP fsm={}",
                  m_ssl_fsm ? "ready" : "null",
                  m_tcp_fsm ? "ready" : "null");
}

Pop3Server::~Pop3Server() {
    stop();
}

void Pop3Server::start() {
    TcpServerBase<Pop3Session<TcpConnection>, Pop3Session<SslConnection>>::start();
    start_lock_sweeper();
}

void Pop3Server::stop(ServerState state) {
    // 先取消 sweeper 定时器再停 io/worker 池（TcpServerBase::stop 会停池）
    if (m_sweep_timer) {
        m_sweep_timer->cancel();
        m_sweep_timer.reset();
    }
    m_sweep_tick = {};
    TcpServerBase<Pop3Session<TcpConnection>, Pop3Session<SslConnection>>::stop(state);
}

void Pop3Server::start_lock_sweeper() {
    if (m_sweep_timer) return;
    auto io_pool = std::dynamic_pointer_cast<IOThreadPool>(m_ioThreadPool);
    if (!io_pool) return;
    boost::asio::io_context& io_ctx = io_pool->get_io_context();
    m_sweep_timer = std::make_shared<boost::asio::steady_timer>(io_ctx);

    // 递归重排：m_sweep_tick 是成员（生命周期=server），handler 引用成员而非
    // 局部变量，避免悬垂。this 裸指针安全：定时器是成员，server 析构即释放。
    m_sweep_tick = [this](const boost::system::error_code& ec) {
        if (ec) return;
        if (!m_sweep_timer) return;
        // 先重排下一次，再清扫（清扫放 worker，不阻塞 io 线程）
        m_sweep_timer->expires_after(std::chrono::seconds(kLockSweepIntervalSeconds));
        m_sweep_timer->async_wait(m_sweep_tick);
        auto router = m_shardRouter;
        if (router && m_workerThreadPool) {
            m_workerThreadPool->post([router]() {
                TraditionalPop3Fsm<TcpConnection>::sweep_expired_locks(router);
            });
        }
    };

    m_sweep_timer->expires_after(std::chrono::seconds(kLockSweepIntervalSeconds));
    m_sweep_timer->async_wait(m_sweep_tick);
    LOG_SERVER_INFO("POP3 lock sweeper started (every {}s)", kLockSweepIntervalSeconds);
}

std::shared_ptr<Pop3Session<TcpConnection>> Pop3Server::make_tcp_session(
    std::unique_ptr<TcpConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<Pop3Session<TcpConnection>>(
        this, std::move(conn), m_tcp_fsm);
}

std::shared_ptr<Pop3Session<SslConnection>> Pop3Server::make_ssl_session(
    std::unique_ptr<SslConnection> conn, const ListenerConfig& lc)
{
    (void)lc;
    return std::make_shared<Pop3Session<SslConnection>>(
        this, std::move(conn), m_ssl_fsm);
}

bool Pop3Server::should_reject_connection(std::string& reason, const std::string&) const {
    auto cfg = std::atomic_load(&m_config);
    if (cfg->maxConnections > 0 &&
        active_connections_.load(std::memory_order_relaxed) >= cfg->maxConnections) {
        reason = "max connections reached";
        return true;
    }
    return false;
}

} // namespace mail_system
