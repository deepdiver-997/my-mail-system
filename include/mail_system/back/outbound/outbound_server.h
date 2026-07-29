#ifndef OUTBOUND_SERVER_H
#define OUTBOUND_SERVER_H

#include "framework/server_base.h"
#include "mail_system/back/outbound/outbound_smtp_session.h"
#include "mail_system/back/outbound/outbound_types.hpp"
#include "mail_system/back/outbound/outbox_repository.h"
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace mail_system {
namespace outbound {

// ================================================================
// OutboundServer — 出站投递调度器
//
//   原子计数器 + CAS 单拉取者模式：
//   - submit() 增加 pending，通知 poll 线程
//   - 完成回调递减 pending，低于阈值时触发 try_pull()
//   - is_pulling CAS 保证同一时刻只有一个线程拉 DB
// ================================================================
class OutboundServer {
public:
    static constexpr int DEFAULT_MAX_SESSIONS_PER_MX = 4;
    static constexpr int DEFAULT_IDLE_TIMEOUT_SEC = 120;
    static constexpr int DEFAULT_BATCH_SIZE = 32;
    static constexpr int64_t LOW_WATERMARK = 16;

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

        // 投递完成回调：递减计数器 + 按需拉取
        completion_cb_ = [this](uint64_t record_id, bool success) {
            auto db = server_->m_shardRouter->get_db_pool(0);
            if (!db) return;
            auto conn = db->acquire_connection();
            if (success)
                repo_.mark_sent(*db, record_id, "250 OK");
            else
                repo_.mark_dead(*db, record_id, "500 permanent failure");

            // 递减计数器，低于水位时尝试拉取
            int64_t prev = pending_count_.fetch_sub(1) - 1;
            if (prev < LOW_WATERMARK) try_pull();
        };

        // poll 线程：处理空闲回收和初始拉取
        server_->m_workerThreadPool->post([this]() { poll_loop(); });

        LOG_SMTP_INFO("OutboundServer started: worker={}", worker_id_);
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        // 简单等待 poll 退出（实际项目中可改用 joinable thread + join）
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
        } else {
            pending_count_.fetch_sub(1);  // 回退
        }
    }

    // ── 拉取（CAS 保护，仅一个线程执行） ──────────────────────
    void try_pull() {
        bool expected = false;
        if (!is_pulling_.compare_exchange_strong(expected, true))
            return;  // 另一个线程已经在拉取

        auto db = server_->m_shardRouter->get_db_pool(0);
        if (!db) {
            is_pulling_.store(false);
            return;
        }

        auto records = repo_.claim_batch(*db, worker_id_, DEFAULT_BATCH_SIZE, 120);
        int pulled = static_cast<int>(records.size());
        if (pulled == 0) { is_pulling_.store(false); return; }

        pending_count_.fetch_add(pulled);

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

        is_pulling_.store(false);

        // 拉满了 → 可能还有更多
        if (pulled >= DEFAULT_BATCH_SIZE) try_pull();
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

    // ── poll 循环 ─────────────────────────────────────────────
    void poll_loop() {
        try_pull();  // 初始拉取

        while (running_.load(std::memory_order_relaxed)) {
            evict_idle();

            // 如果 pending 低于水位，再拉一批
            if (pending_count_.load() < LOW_WATERMARK) try_pull();

            // 空闲回收间隔：30 秒
            std::unique_lock<std::mutex> lk(cv_mu_);
            cv_.wait_for(lk, std::chrono::seconds(30),
                         [this]() { return !running_.load(); });
        }
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

    mutable std::shared_mutex mutex_;
    std::map<std::string, SessionList> mx_sessions_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_active_;

    std::mutex cv_mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_SERVER_H
