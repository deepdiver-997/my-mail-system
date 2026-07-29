#ifndef OUTBOUND_SERVER_H
#define OUTBOUND_SERVER_H

#include "framework/server_base.h"
#include "mail_system/back/outbound/outbound_smtp_session.h"
#include "mail_system/back/outbound/outbound_types.hpp"
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <chrono>

namespace mail_system {
namespace outbound {

// ================================================================
// OutboundServer — 出站投递调度器
//
//   管理 MX → session 映射，复用 TCP 连接流水线投递。
//   LRU 淘汰冷门 MX 的闲置连接。
// ================================================================
class OutboundServer {
public:
    static constexpr int DEFAULT_MAX_SESSIONS_PER_MX = 4;
    static constexpr int DEFAULT_IDLE_TIMEOUT_SEC = 120;
    static constexpr int DEFAULT_MAX_MX_ENTRIES = 256;

    OutboundServer(ServerBase* server,
                   int max_per_mx = DEFAULT_MAX_SESSIONS_PER_MX,
                   int idle_timeout = DEFAULT_IDLE_TIMEOUT_SEC)
        : server_(server)
        , max_sessions_per_mx_(max_per_mx)
        , idle_timeout_sec_(idle_timeout)
    {}

    // ── 完成回调 ──────────────────────────────────────────────
    // 设置后自动转发给所有新创建的 session
    using CompletionCb = OutboundSmtpSession<TcpConnection>::CompletionCb;
    void set_completion_cb(CompletionCb cb) { completion_cb_ = std::move(cb); }

    // ── 投递入口 ──────────────────────────────────────────────
    void submit(std::unique_ptr<MailDeliveryTask> task, int mx_port = 25) {
        std::string domain = extract_domain(task->recipient);
        if (domain.empty()) return;

        auto session = acquire_session(domain, mx_port);
        if (session) {
            session->submit(std::move(task));
        }
    }

    // ── 空闲连接回收（可被定时器定期调用） ────────────────────
    void evict_idle() {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock<std::shared_mutex> lk(mutex_);

        for (auto it = mx_sessions_.begin(); it != mx_sessions_.end();) {
            auto& sessions = it->second;
            // 移除已断开的 session
            sessions.erase(
                std::remove_if(sessions.begin(), sessions.end(),
                    [](const SessionPtr& s) { return !s || !s->is_connected(); }),
                sessions.end());

            // 该 MX 的所有 session 是否都空闲（队列空 + 无正在投递的邮件）？
            bool all_idle = true;
            for (auto& s : sessions) {
                if (s->queue_size() > 0 || s->has_active_task()) {
                    all_idle = false;
                    last_active_[it->first] = now;
                    break;
                }
            }

            if (all_idle && sessions.empty()) {
                // 无 session → 移除 MX 条目
                last_active_.erase(it->first);
                it = mx_sessions_.erase(it);
            } else if (all_idle) {
                // 检查空闲超时
                auto last_it = last_active_.find(it->first);
                auto last = (last_it != last_active_.end())
                    ? last_it->second : now;
                if (now - last > std::chrono::seconds(idle_timeout_sec_)) {
                    // 关闭所有空闲连接，移除 MX 条目
                    for (auto& s : sessions) {
                        if (s->is_connected()) s->close();
                    }
                    sessions.clear();
                    last_active_.erase(it->first);
                    it = mx_sessions_.erase(it);
                    LOG_SMTP_INFO("Outbound: evicted idle MX {}", it->first);
                } else {
                    ++it;
                }
            } else {
                // 有活跃 session → 保留
                ++it;
            }
        }

        // LRU 容量保护：如果 MX 条目过多，淘汰最久未活跃的
        while (mx_sessions_.size() > DEFAULT_MAX_MX_ENTRIES) {
            auto oldest_it = mx_sessions_.begin();
            auto oldest_time = now;
            for (auto mit = mx_sessions_.begin(); mit != mx_sessions_.end(); ++mit) {
                auto lit = last_active_.find(mit->first);
                auto t = (lit != last_active_.end()) ? lit->second : std::chrono::steady_clock::time_point::min();
                if (t < oldest_time) { oldest_time = t; oldest_it = mit; }
            }
            for (auto& s : oldest_it->second) {
                if (s->is_connected()) s->close();
            }
            LOG_SMTP_INFO("Outbound: LRU evict MX {}", oldest_it->first);
            last_active_.erase(oldest_it->first);
            mx_sessions_.erase(oldest_it);
        }
    }

    // ── 健康检查 ──────────────────────────────────────────────
    size_t total_sessions() const {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        size_t n = 0;
        for (auto& [mx, sessions] : mx_sessions_) n += sessions.size();
        return n;
    }

    size_t mx_count() const {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        return mx_sessions_.size();
    }

private:
    using SessionPtr = std::shared_ptr<OutboundSmtpSession<TcpConnection>>;
    using SessionList = std::vector<SessionPtr>;

    SessionPtr acquire_session(const std::string& mx, int port) {
        std::unique_lock<std::shared_mutex> lk(mutex_);

        last_active_[mx] = std::chrono::steady_clock::now();

        auto it = mx_sessions_.find(mx);
        if (it != mx_sessions_.end()) {
            auto& sessions = it->second;
            // 清理已断开
            sessions.erase(
                std::remove_if(sessions.begin(), sessions.end(),
                    [](const SessionPtr& s) { return !s || !s->is_connected(); }),
                sessions.end());

            // 找负载最轻的
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
    CompletionCb completion_cb_;

    mutable std::shared_mutex mutex_;
    std::map<std::string, SessionList> mx_sessions_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_active_;
};

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_SERVER_H
