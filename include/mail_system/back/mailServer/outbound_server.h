#ifndef MAIL_SYSTEM_OUTBOUND_SERVER_H
#define MAIL_SYSTEM_OUTBOUND_SERVER_H

#include "framework/server_base.h"
#include "mail_system/back/mailServer/session/outbound_smtp_session.h"
#include "mail_system/back/mailServer/outbound/outbound_types.hpp"
#include "mail_system/back/mailServer/outbound/outbox_repository.h"
#include <boost/asio/steady_timer.hpp>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <chrono>
#include <atomic>

namespace mail_system {
namespace outbound {

// ================================================================
// OutboundServer — 出站投递调度器（事件驱动）
//
//   水位预占 + CAS 单拉取周期：
//   - try_pull() 先判水位，再 CAS 抢占拉取周期
//   - 抢占后预占 BATCH_SIZE，claim 回调中修正
//   - completion_cb / submit() 事件驱动触发 try_pull()
//   - claim 返回空时指数退避重试，不释放 is_pulling_
//   - 空闲回收由独立 steady_timer 驱动
//   - 不再常驻占用 worker 线程
//
//   实现：见 src/mail_system/back/mailServer/outbound/outbound_server.cpp
//   (non-template 类，仅声明保留在头文件)
// ================================================================
class OutboundServer {
public:
    static constexpr int DEFAULT_MAX_SESSIONS_PER_MX = 4;
    static constexpr int DEFAULT_IDLE_TIMEOUT_SEC = 120;
    static constexpr int DEFAULT_MAX_MX_ENTRIES = 256;
    static constexpr int DEFAULT_BATCH_SIZE = 32;
    static constexpr int64_t LOW_WATERMARK = 16;
    static constexpr int MIN_RETRY_DELAY_SEC = 5;
    static constexpr int MAX_RETRY_DELAY_SEC = 60;

    OutboundServer(ServerBase* server,
                   int max_per_mx = DEFAULT_MAX_SESSIONS_PER_MX,
                   int idle_timeout = DEFAULT_IDLE_TIMEOUT_SEC);

    ~OutboundServer();

    // ── 生命周期 ──────────────────────────────────────────────
    void start();
    void stop();

    // ── 投递入口 ──────────────────────────────────────────────
    void submit(std::unique_ptr<MailDeliveryTask> task, int port = -1);

    // ── 拉取入口（水位检查 + CAS 抢占） ───────────────────────
    void try_pull();

    // ── 空闲回收 ──────────────────────────────────────────────
    void evict_idle();

    // ── 健康检查 ──────────────────────────────────────────────
    size_t total_sessions() const;
    bool is_running() const;

    // ── 配置注入 ──────────────────────────────────────────────
    // 必须在 start() 之前调用。注入后 OutboundServer 持有副本，
    // 不再依赖外层 server_config。default 构造给出合理兜底。
    void set_config(OutboundConfig c);
    const OutboundConfig& config() const;

private:
    // 端口解析：static_routes[domain] > ports[0] > 25
    int resolve_port(const std::string& domain) const;

    // 路由目标 host 解析：static_routes[domain] 命中则用静态 host（跳过 DNS）；
    // 未命中返 domain 字符串（让 OutboundSmtpSession 走 DNS 解析）
    std::string resolve_target_host(const std::string& domain) const;

    using SessionPtr = std::shared_ptr<OutboundSmtpSession<TcpConnection>>;
    using SessionList = std::vector<SessionPtr>;
    using CompletionCb = OutboundSmtpSession<TcpConnection>::CompletionCb;

    // ── DB 拉取（发送到 worker 线程池） ───────────────────────
    void do_claim_batch();

    // ── claim 回调：修正水位 + 分发 + 续拉 / 退避重试 ────────
    void on_claim_complete(std::vector<OutboxRecord> records);

    // ── 退避重试（claim 返回空时） ────────────────────────────
    void schedule_retry();

    // ── 空闲回收定时器（30 秒周期） ───────────────────────────
    void schedule_evict();

    SessionPtr acquire_session(const std::string& mx, int port);
    SessionPtr create_session(const std::string& mx, int port);

    static std::string extract_domain(const std::string& addr);

    // ── 成员 ──────────────────────────────────────────────────
    ServerBase* server_;
    int max_sessions_per_mx_;
    int idle_timeout_sec_;
    std::string worker_id_;
    OutboxRepository repo_;
    CompletionCb completion_cb_;
    OutboundConfig config_;   // set_config() 注入；submit() 派生 port 用

    std::atomic<int64_t> pending_count_{0};
    std::atomic<bool> is_pulling_{false};
    int retry_delay_sec_ = MIN_RETRY_DELAY_SEC;

    mutable std::shared_mutex mutex_;
    std::map<std::string, SessionList> mx_sessions_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_active_;

    std::atomic<bool> running_{false};
    std::unique_ptr<boost::asio::steady_timer> evict_timer_;
    std::unique_ptr<boost::asio::steady_timer> retry_timer_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_OUTBOUND_SERVER_H
