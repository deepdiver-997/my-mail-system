#ifndef OUTBOUND_SERVER_H
#define OUTBOUND_SERVER_H

#include "framework/server_base.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "mail_system/back/outbound/outbound_smtp_session.h"
#include "mail_system/back/outbound/outbound_types.hpp"
#include "mail_system/back/outbound/outbox_repository.h"
#include <boost/asio/steady_timer.hpp>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
                   int idle_timeout = DEFAULT_IDLE_TIMEOUT_SEC)
        : server_(server)
        , max_sessions_per_mx_(max_per_mx)
        , idle_timeout_sec_(idle_timeout)
        , worker_id_("ob-" + std::to_string(reinterpret_cast<uintptr_t>(this)))
    {}

    ~OutboundServer() { stop(); }

    // ── 生命周期 ──────────────────────────────────────────────
    void start() {
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

    void stop() {
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

    // ── 投递入口 ──────────────────────────────────────────────
    void submit(std::unique_ptr<MailDeliveryTask> task, int port = 25) {
        std::string domain = extract_domain(task->recipient);
        if (domain.empty()) return;

        pending_count_.fetch_add(1);
        auto session = acquire_session(domain, port);
        if (session) {
            session->submit(std::move(task));
            try_pull();  // 事件驱动：新任务到达后检查是否需要拉取
        } else {
            pending_count_.fetch_sub(1);
        }
    }

    // ── 拉取入口（水位检查 + CAS 抢占） ───────────────────────
    void try_pull() {
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

    // ── 空闲回收 ──────────────────────────────────────────────
    void evict_idle() {
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

    // ── 健康检查 ──────────────────────────────────────────────
    size_t total_sessions() const {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        size_t n = 0;
        for (auto& [mx, sessions] : mx_sessions_) n += sessions.size();
        return n;
    }
    bool is_running() const { return running_.load(); }

private:
    using SessionPtr = std::shared_ptr<OutboundSmtpSession<TcpConnection>>;
    using SessionList = std::vector<SessionPtr>;
    using CompletionCb = OutboundSmtpSession<TcpConnection>::CompletionCb;

    // ── DB 拉取（发送到 worker 线程池） ───────────────────────
    void do_claim_batch() {
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

    // ── claim 回调：修正水位 + 分发 + 续拉 / 退避重试 ────────
    void on_claim_complete(std::vector<OutboxRecord> records) {
        int pulled = static_cast<int>(records.size());

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

            // 直接路由到 session，不通过 public submit() 避免重复计数
            std::string domain = extract_domain(task->recipient);
            auto session = acquire_session(domain, 25);
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

    // ── 退避重试（claim 返回空时） ────────────────────────────
    void schedule_retry() {
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

    // ── 空闲回收定时器（30 秒周期） ───────────────────────────
    void schedule_evict() {
        if (!running_.load(std::memory_order_relaxed)) return;
        evict_timer_->expires_after(std::chrono::seconds(30));
        evict_timer_->async_wait([this](const boost::system::error_code& ec) {
            if (ec || !running_.load(std::memory_order_relaxed)) return;
            evict_idle();
            schedule_evict();  // 重新武装
        });
    }

    SessionPtr acquire_session(const std::string& mx, int port) {
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

    SessionPtr create_session(const std::string& mx, int port) {
        auto s = std::make_shared<OutboundSmtpSession<TcpConnection>>(server_, mx, port);
        if (completion_cb_) s->set_completion_cb(completion_cb_);
        return s;
    }

    static std::string extract_domain(const std::string& addr) {
        auto at = addr.find('@');
        if (at != std::string::npos) return addr.substr(at + 1);
        return addr;
    }

    // ── 成员 ──────────────────────────────────────────────────
    ServerBase* server_;
    int max_sessions_per_mx_;
    int idle_timeout_sec_;
    std::string worker_id_;
    OutboxRepository repo_;
    CompletionCb completion_cb_;

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

#endif // OUTBOUND_SERVER_H
