#ifndef OUTBOUND_SERVER_H
#define OUTBOUND_SERVER_H

#include "framework/server_base.h"
#include "mail_system/back/outbound/outbound_smtp_session.h"
#include "mail_system/back/outbound/outbound_types.hpp"
#include "mail_system/back/common/lru_cache.h"
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
//   管理 MX → session 映射，复用 TCP 连接投递多封邮件。
//   LRU 淘汰冷门 MX 的闲置连接。
// ================================================================
class OutboundServer {
public:
    static constexpr int DEFAULT_MAX_SESSIONS_PER_MX = 4;
    static constexpr int DEFAULT_IDLE_TIMEOUT_SEC = 120;

    OutboundServer(ServerBase* server,
                   int max_per_mx = DEFAULT_MAX_SESSIONS_PER_MX,
                   int idle_timeout = DEFAULT_IDLE_TIMEOUT_SEC)
        : server_(server)
        , max_sessions_per_mx_(max_per_mx)
        , idle_timeout_sec_(idle_timeout)
    {}

    // ── 投递入口 ──────────────────────────────────────────────
    // MX 已在 task 中确定（由 router 在上层解析）
    void submit(std::unique_ptr<MailDeliveryTask> task, int mx_port = 25) {
        const std::string& mx = task->recipient;  // recipient 的域名即 MX
        // 提取域名部分
        std::string domain = extract_domain(task->recipient);
        if (domain.empty()) return;

        auto session = acquire_session(domain, mx_port);
        if (session) {
            session->submit(std::move(task));
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

        // 1. 查找已有连接组
        auto it = mx_sessions_.find(mx);
        if (it != mx_sessions_.end()) {
            // 清理已断开的 session
            auto& sessions = it->second;
            sessions.erase(
                std::remove_if(sessions.begin(), sessions.end(),
                    [](const SessionPtr& s) { return !s || !s->is_connected(); }),
                sessions.end());

            // 找负载最轻的 session
            SessionPtr best;
            size_t min_q = SIZE_MAX;
            for (auto& s : sessions) {
                size_t qs = s->queue_size();
                if (qs < min_q) { min_q = qs; best = s; }
            }
            if (best && min_q < 100) {  // 队列未爆满
                return best;
            }

            // 已满 → 创建新 session（如果未超上限）
            if (sessions.size() < static_cast<size_t>(max_sessions_per_mx_)) {
                auto new_s = create_session(mx, port);
                sessions.push_back(new_s);
                return new_s;
            }
            // 超上限 → 负载最低的那个（即使队列长）
            return best;
        }

        // 2. 无此 MX → 创建首个 session
        auto new_s = create_session(mx, port);
        mx_sessions_[mx] = {new_s};
        return new_s;
    }

    SessionPtr create_session(const std::string& mx, int port) {
        return std::make_shared<OutboundSmtpSession<TcpConnection>>(server_, mx, port);
    }

    static std::string extract_domain(const std::string& addr) {
        auto at = addr.find('@');
        if (at != std::string::npos) return addr.substr(at + 1);
        return addr;  // 已经是裸域名
    }

    // ── 成员 ──────────────────────────────────────────────────
    ServerBase* server_;
    int max_sessions_per_mx_;
    int idle_timeout_sec_;

    mutable std::shared_mutex mutex_;
    std::map<std::string, SessionList> mx_sessions_;
};

} // namespace outbound
} // namespace mail_system

#endif // OUTBOUND_SERVER_H
