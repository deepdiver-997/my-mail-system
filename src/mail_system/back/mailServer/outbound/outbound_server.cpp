// ================================================================
// OutboundServer — 出站投递调度器实现
//
// 实现从 include/mail_system/back/mailServer/outbound_server.h 拆出
// （参考 src/framework/server_base.cpp 与 include/framework/server_base.h
//  的 .h+.cpp 范式）
//
// 拆分原因：原 outbound_server.h 404 行所有实现都 inline 在 class 内
//   加重 include 链传播时间、且与同模块其它文件（outbound_smtp_session.h
//   / outbound_smtp_fsm.h 模板类）混淆 .h/.cpp 范式
// 拆后：.h 仅声明 + 成员，.cpp 实现 21 个函数
// 模板类（OutboundSmtpSession / OutboundSmtpFsm）保持 .h 现状
// ================================================================

#include "mail_system/back/mailServer/outbound_server.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "mail_system/back/common/logger.h"

namespace mail_system {
namespace outbound {

// ── 构造 / 析构 ────────────────────────────────────────────
OutboundServer::OutboundServer(ServerBase* server,
                               int max_per_mx,
                               int idle_timeout)
    : server_(server)
    , max_sessions_per_mx_(max_per_mx)
    , idle_timeout_sec_(idle_timeout)
    , worker_id_("ob-" + std::to_string(reinterpret_cast<uintptr_t>(this)))
{}

OutboundServer::~OutboundServer() { stop(); }

// ── 生命周期 ────────────────────────────────────────────────
void OutboundServer::start() {
    if (running_.exchange(true)) return;

    completion_cb_ = [this](uint64_t record_id, bool success) {
        // DB 写入 post 到 worker 池，不阻塞 IO 线程
        auto db = server_->m_shardRouter->get_db_pool(0);
        if (db) {
            server_->m_workerThreadPool->post([this, db, record_id, success]() {
                auto conn = db->acquire_connection();
                if (success)
                    repo_.mark_sent(*db, record_id, "250 OK");
                else
                    repo_.mark_dead(*db, record_id, "500 permanent failure");
            });
        }

        // 2026-08-27: 投递结果 metrics。domain label 在此 lambda 拿不到（task
        // 已 reset），仅用 {} 标签。counts 数本身比按域分桶价值高（聚合告警阈值）。
        if (auto m = m_metrics.lock()) {
            m->inc_counter(success ? "protorelay_outbound_delivered_total"
                                   : "protorelay_outbound_bounced_total",
                           {}, 1);
        }

        // 计数递减和 try_pull 立即可执行（不依赖 DB 写入结果）
        int64_t prev = pending_count_.fetch_sub(1) - 1;
        if (prev < LOW_WATERMARK) try_pull();
    };

    // 初始拉取：post 到 worker 池，不占常驻线程
    server_->m_workerThreadPool->post([this]() { try_pull(); });

    // 空闲回收定时器
    auto& io_ctx = static_cast<IOThreadPool*>(
        server_->m_ioThreadPool.get())->get_io_context();
    evict_timer_ = std::make_unique<boost::asio::steady_timer>(io_ctx);
    schedule_evict();

    LOG_SMTP_INFO("OutboundServer started: worker={}", worker_id_);
}

void OutboundServer::stop() {
    if (!running_.exchange(false)) return;

    // 取消定时器
    if (evict_timer_) evict_timer_->cancel();
    if (retry_timer_) retry_timer_->cancel();

    std::unique_lock<std::shared_mutex> lk(mutex_);
    for (auto& [mx, sessions] : mx_sessions_)
        for (auto& s : sessions)
            if (s && s->is_connected()) s->close();
    mx_sessions_.clear();
    LOG_SMTP_INFO("OutboundServer stopped");
}

// ── 投递入口 ────────────────────────────────────────────────
void OutboundServer::submit(std::unique_ptr<MailDeliveryTask> task, int port) {
    std::string domain = extract_domain(task->recipient);
    if (domain.empty()) return;

    // 2026-08-27: 外部 submit 入口的 attempts 计数（on_claim_complete 是内部
    // 拉取路径，那里再 push 一次，外部 submit 直接跳过外层路由）。
    if (auto m = m_metrics.lock()) {
        m->inc_counter("protorelay_outbound_attempts_total", {{"domain", domain}}, 1);
    }

    // 端口解析顺序：static_routes[domain] > ports[0] > 25
    int target_port = (port > 0) ? port : resolve_port(domain);
    // target_host: static_routes 命中时是 IP/host（跳过 DNS），否则是 domain
    std::string target_host = resolve_target_host(domain);
    pending_count_.fetch_add(1);
    auto session = acquire_session(target_host, target_port);
    if (session) {
        session->submit(std::move(task));
        try_pull();  // 事件驱动：新任务到达后检查是否需要拉取
    } else {
        pending_count_.fetch_sub(1);
    }
}

// ── 拉取入口（水位检查 + CAS 抢占） ─────────────────────────
void OutboundServer::try_pull() {
    // 快速路径：水位够高直接返回，无 CAS 开销
    if (pending_count_.load(std::memory_order_relaxed) >= LOW_WATERMARK)
        return;

    // CAS 抢占拉取周期
    bool expected = false;
    if (!is_pulling_.compare_exchange_strong(expected, true))
        return;

    // 预占水位：claim 返回后修正
    pending_count_.fetch_add(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
    do_claim_batch();
}

// ── 空闲回收 ────────────────────────────────────────────────
void OutboundServer::evict_idle() {
    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lk(mutex_);

    for (auto it = mx_sessions_.begin(); it != mx_sessions_.end();) {
        auto& sessions = it->second;
        sessions.erase(
            std::remove_if(sessions.begin(), sessions.end(),
                [](const SessionPtr& s) { return !s || !s->is_connected(); }),
            sessions.end());

        bool all_idle = true;
        for (auto& s : sessions) {
            if (s->queue_size() > 0 || s->has_active_task()) {
                all_idle = false;
                last_active_[it->first] = now;
                break;
            }
        }

        if (all_idle && sessions.empty()) {
            last_active_.erase(it->first);
            it = mx_sessions_.erase(it);
        } else if (all_idle) {
            auto li = last_active_.find(it->first);
            auto last = (li != last_active_.end()) ? li->second : now;
            if (now - last > std::chrono::seconds(idle_timeout_sec_)) {
                for (auto& s : sessions) {
                    if (s->is_connected()) s->close();
                }
                sessions.clear();
                last_active_.erase(it->first);
                it = mx_sessions_.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }

    while (mx_sessions_.size() > static_cast<size_t>(DEFAULT_MAX_MX_ENTRIES)) {
        auto oldest = mx_sessions_.begin();
        auto oldest_t = now;
        for (auto mi = mx_sessions_.begin(); mi != mx_sessions_.end(); ++mi) {
            auto li = last_active_.find(mi->first);
            auto t = (li != last_active_.end()) ? li->second
                : std::chrono::steady_clock::time_point::min();
            if (t < oldest_t) { oldest_t = t; oldest = mi; }
        }
        for (auto& s : oldest->second) {
            if (s->is_connected()) s->close();
        }
        last_active_.erase(oldest->first);
        mx_sessions_.erase(oldest);
    }
}

// ── 健康检查 ────────────────────────────────────────────────
size_t OutboundServer::total_sessions() const {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    size_t n = 0;
    for (auto& [mx, sessions] : mx_sessions_) n += sessions.size();
    return n;
}

bool OutboundServer::is_running() const { return running_.load(); }

// ── 配置注入 ────────────────────────────────────────────────
void OutboundServer::set_config(OutboundConfig c) { config_ = std::move(c); }
const OutboundConfig& OutboundServer::config() const { return config_; }

// ── 私有：端口/host 解析 ────────────────────────────────────
int OutboundServer::resolve_port(const std::string& domain) const {
    auto it = config_.static_routes.find(domain);
    if (it != config_.static_routes.end() && it->second.port > 0)
        return it->second.port;
    if (!config_.ports.empty()) return config_.ports[0];
    return 25;
}

std::string OutboundServer::resolve_target_host(const std::string& domain) const {
    auto it = config_.static_routes.find(domain);
    if (it != config_.static_routes.end() && !it->second.host.empty())
        return it->second.host;
    return domain;
}

// ── 私有：DB 拉取（发送到 worker 线程池） ────────────────────
void OutboundServer::do_claim_batch() {
    auto db = server_->m_shardRouter->get_db_pool(0);
    if (!db) {
        // DB 不可用，退还预占并放弃本次拉取周期
        pending_count_.fetch_sub(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
        is_pulling_.store(false);
        return;
    }

    server_->m_workerThreadPool->post([this, db]() {
        auto records = repo_.claim_batch(*db, worker_id_, DEFAULT_BATCH_SIZE, 120);
        on_claim_complete(std::move(records));
    });
}

// ── 私有：claim 回调 ────────────────────────────────────────
void OutboundServer::on_claim_complete(std::vector<OutboxRecord> records) {
    int pulled = static_cast<int>(records.size());
    LOG_SMTP_INFO("Outbound: on_claim_complete pulled={}", pulled);

    // 修正预占水位（只会少不会多，over >= 0）
    int over = DEFAULT_BATCH_SIZE - pulled;
    if (over > 0)
        pending_count_.fetch_sub(over, std::memory_order_relaxed);

    // 无记录：退避重试，不释放 is_pulling_
    if (records.empty()) {
        schedule_retry();
        return;
    }

    // 有数据，重置退避延迟
    retry_delay_sec_ = MIN_RETRY_DELAY_SEC;

    // 加载邮件并分发到 session
    auto db = server_->m_shardRouter->get_db_pool(0);
    if (!db) {
        is_pulling_.store(false);
        return;
    }

    for (auto& rec : records) {
        auto mail_ptr = repo_.load_mail(*db, rec.mail_id);
        auto task = std::make_unique<MailDeliveryTask>();
        task->mail_id = rec.mail_id;
        task->record_id = rec.id;
        task->sender = rec.sender;
        task->recipient = rec.recipient;
        task->mail_ptr = std::move(mail_ptr);
        task->attempt_count = rec.attempt_count;
        task->max_attempts = rec.max_attempts;

        // 2026-08-27: 内部 claim 路径的 attempts 计数。submit() 也 push 一次
        // （外部入站用），这里是 DB outbox 拉取的入站用，两条路径互斥。
        std::string domain = extract_domain(task->recipient);
        if (auto m = m_metrics.lock()) {
            m->inc_counter("protorelay_outbound_attempts_total",
                           {{"domain", domain}}, 1);
        }

        // 直接路由到 session，不通过 public submit() 避免重复计数
        int target_port = resolve_port(domain);
        // target_host: static_routes 命中时是 IP/host（跳过 DNS），否则是 domain
        std::string target_host = resolve_target_host(domain);
        LOG_SMTP_INFO("Outbound: dispatching mail_id={} recipient={} target_host={} port={}",
                      rec.mail_id, rec.recipient, target_host, target_port);
        auto session = acquire_session(target_host, target_port);
        if (session) session->submit(std::move(task));
    }

    // 水位仍低 → 链式续拉，不释放 is_pulling_
    if (pending_count_.load(std::memory_order_relaxed) < LOW_WATERMARK) {
        pending_count_.fetch_add(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
        do_claim_batch();
    } else {
        is_pulling_.store(false);
    }
}

// ── 私有：退避重试（claim 返回空时） ─────────────────────────
void OutboundServer::schedule_retry() {
    retry_delay_sec_ = std::min(retry_delay_sec_ * 2, MAX_RETRY_DELAY_SEC);

    auto& io_ctx = static_cast<IOThreadPool*>(
        server_->m_ioThreadPool.get())->get_io_context();
    retry_timer_ = std::make_unique<boost::asio::steady_timer>(io_ctx);
    retry_timer_->expires_after(std::chrono::seconds(retry_delay_sec_));
    retry_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec || !running_.load(std::memory_order_relaxed)) return;
        // is_pulling_ 仍为 true，直接重新预占并拉取
        pending_count_.fetch_add(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
        do_claim_batch();
    });
}

// ── 私有：空闲回收定时器（30 秒周期） ───────────────────────
void OutboundServer::schedule_evict() {
    if (!running_.load(std::memory_order_relaxed)) return;
    evict_timer_->expires_after(std::chrono::seconds(30));
    evict_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec || !running_.load(std::memory_order_relaxed)) return;
        evict_idle();
        schedule_evict();  // 重新武装
    });
}

// ── 私有：session 池管理 ────────────────────────────────────
OutboundServer::SessionPtr OutboundServer::acquire_session(const std::string& mx, int port) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    last_active_[mx] = std::chrono::steady_clock::now();

    auto it = mx_sessions_.find(mx);
    if (it != mx_sessions_.end()) {
        auto& sessions = it->second;
        sessions.erase(
            std::remove_if(sessions.begin(), sessions.end(),
                [](const SessionPtr& s) { return !s || !s->is_connected(); }),
            sessions.end());

        SessionPtr best;
        size_t min_q = SIZE_MAX;
        for (auto& s : sessions) {
            size_t qs = s->queue_size();
            if (qs < min_q) { min_q = qs; best = s; }
        }
        if (best && min_q < 100) return best;

        if (sessions.size() < static_cast<size_t>(max_sessions_per_mx_)) {
            auto new_s = create_session(mx, port);
            sessions.push_back(new_s);
            return new_s;
        }
        return best;
    }

    auto new_s = create_session(mx, port);
    mx_sessions_[mx] = {new_s};
    return new_s;
}

OutboundServer::SessionPtr OutboundServer::create_session(const std::string& mx, int port) {
    auto s = std::make_shared<OutboundSmtpSession<TcpConnection>>(server_, mx, port);
    if (completion_cb_) s->set_completion_cb(completion_cb_);
    return s;
}

std::string OutboundServer::extract_domain(const std::string& addr) {
    auto at = addr.find('@');
    if (at != std::string::npos) return addr.substr(at + 1);
    return addr;
}

} // namespace outbound
} // namespace mail_system
