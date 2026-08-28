#ifndef TRADITIONAL_POP3_FSM_TPP
#define TRADITIONAL_POP3_FSM_TPP

#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/algorithm/snow.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "framework/db/db_pool.h"
#include "framework/thread_pool/io_thread_pool.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <utility>

namespace mail_system {

namespace {
inline std::shared_ptr<IDBResult> sq(class IDBConnection* c, const std::string& sql,
                                      const std::vector<std::string>& params) {
    std::shared_ptr<IDBResult> r;
    c->async_query(sql, params, [&r](auto res) { r = std::move(res); });
    return r;
}
inline bool se(class IDBConnection* c, const std::string& sql,
                const std::vector<std::string>& params) {
    bool ok = false;
    c->async_execute(sql, params, [&ok](bool r) { ok = r; });
    return ok;
}
inline bool se(class IDBConnection* c, const std::string& sql) {
    bool ok = false;
    c->async_execute(sql, [&ok](bool r) { ok = r; });
    return ok;
}
} // namespace

template <typename ConnectionType>
Pop3Context* TraditionalPop3Fsm<ConnectionType>::ctx_of(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    return static_cast<Pop3Context*>(session->get_context());
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::send_line(
    std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& line) {
    if (!session) return;
    std::string out = line;
    if (out.empty() || (out.back() != '\n')) out += "\r\n";
    session->trace_append_outbound(out);
    session->do_async_write(out);
}

template <typename ConnectionType>
std::string TraditionalPop3Fsm<ConnectionType>::get_state_name(Pop3State s) {
    switch (s) {
        case Pop3State::INIT: return "INIT";
        case Pop3State::AUTHORIZATION: return "AUTHORIZATION";
        case Pop3State::TRANSACTION: return "TRANSACTION";
        case Pop3State::UPDATE: return "UPDATE";
        case Pop3State::CLOSED: return "CLOSED";
        default: return "UNKNOWN_STATE";
    }
}

template <typename ConnectionType>
std::string TraditionalPop3Fsm<ConnectionType>::get_event_name(Pop3Event e) {
    switch (e) {
        case Pop3Event::CONNECT: return "CONNECT";
        case Pop3Event::CAPA: return "CAPA";
        case Pop3Event::USER: return "USER";
        case Pop3Event::PASS: return "PASS";
        case Pop3Event::STAT: return "STAT";
        case Pop3Event::LIST: return "LIST";
        case Pop3Event::UIDL: return "UIDL";
        case Pop3Event::RETR: return "RETR";
        case Pop3Event::DELE: return "DELE";
        case Pop3Event::NOOP: return "NOOP";
        case Pop3Event::RSET: return "RSET";
        case Pop3Event::QUIT: return "QUIT";
        case Pop3Event::ERROR: return "ERROR";
        case Pop3Event::TIMEOUT: return "TIMEOUT";
        default: return "UNKNOWN_EVENT";
    }
}

// ====================================================================
// 鉴权 + DB helper
// ====================================================================

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::auth_user(
    const std::shared_ptr<router::IShardRouter>& shard_router,
    const std::shared_ptr<AuthCache>& auth_cache,
    const std::string& mail_address,
    const std::string& password,
    uint64_t& out_user_id,
    int& out_shard)
{
    LOG_AUTH_INFO("POP3 AUTH attempt: mail_address=[{}]", mail_address);

    int shard = 0;
    if (shard_router) {
        int r = shard_router->route(mail_address);
        if (r >= 0) shard = r;
    }
    out_shard = shard;

    AuthCacheEntry ce;
    if (auth_cache && auth_cache->lookup(mail_address, ce)) {
        if (ce.status != 1) return false;
        out_shard = ce.shard;
        out_user_id = ce.user_id;
        if (ce.password_hash.size() >= 2 && ce.password_hash[0] == '$' && ce.password_hash[1] == '2')
            return bcrypt_verify(password, ce.password_hash);
        return ce.password_hash == password;
    }

    auto db_pool = shard_router ? shard_router->get_db_pool(static_cast<size_t>(shard)) : nullptr;
    if (!db_pool) {
        LOG_AUTH_ERROR("No database pool for shard {}", shard);
        return false;
    }
    auto conn = db_pool->acquire_connection();
    if (!conn.is_valid()) {
        LOG_AUTH_ERROR("Failed to get database connection for shard {}", shard);
        return false;
    }

    auto result = sq(conn.operator->(), db::sql::build_auth_user_query(), {mail_address});
    if (!result || result->get_row_count() == 0) {
        LOG_AUTH_WARN("User not found: {}", mail_address);
        return false;
    }

    int status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
    if (status != 1) {
        LOG_AUTH_WARN("User account disabled: {}", mail_address);
        return false;
    }

    std::string stored = result->get_value(0, "password");
    uint64_t user_id = safe_stoull(result->get_value(0, "id"));
    if (auth_cache) {
        auth_cache->store(mail_address, {stored, status, user_id, shard});
    }

    bool ok = false;
    if (stored.size() >= 2 && stored[0] == '$' && stored[1] == '2') {
        ok = bcrypt_verify(password, stored);
    } else {
        ok = (stored == password);
        if (ok) LOG_AUTH_WARN("User {} still using plaintext password", mail_address);
    }

    if (ok) {
        out_user_id = user_id;
        se(conn.operator->(), db::sql::build_update_last_login(), {mail_address});
    }
    return ok;
}

template <typename ConnectionType>
uint64_t TraditionalPop3Fsm<ConnectionType>::get_inbox_id(
    class IDBConnection* conn, uint64_t user_id)
{
    if (!conn) return 0;
    auto result = sq(conn, db::sql::build_imap_get_inbox_id(), {std::to_string(user_id)});
    if (!result || result->get_row_count() == 0) return 0;
    return safe_stoull(result->get_value(0, "id"));
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::get_inbox_mails(
    class IDBConnection* conn, uint64_t mailbox_id, uint64_t user_id,
    std::vector<Pop3Message>& out)
{
    out.clear();
    if (!conn) return false;
    auto result = sq(conn, db::sql::build_imap_get_mailbox_mails(),
                     {std::to_string(mailbox_id), std::to_string(user_id)});
    if (!result) return false;
    for (size_t i = 0; i < result->get_row_count(); ++i) {
        Pop3Message m;
        m.mail_id = safe_stoull(result->get_value(i, "id"));
        m.body_path = result->get_value(i, "body_path");
        if (m.mail_id == 0) continue;
        out.push_back(std::move(m));
    }
    return true;
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::acquire_lock(
    class IDBConnection* conn, uint64_t user_id, const std::string& session_id)
{
    if (!conn) return false;
    // INSERT ... ON DUPLICATE KEY UPDATE：同 session_id 续约，异 session_id 保留旧锁。
    // async_execute 只返回 bool（无 affected-rows），且 ON DUPLICATE KEY UPDATE
    // 在冲突时并不报错，所以 upsert 之后再 SELECT 验证所有权。
    const char* upsert =
        "INSERT INTO pop3_session_lock (user_id, session_id, acquired_at, last_heartbeat) "
        "VALUES (?, ?, NOW(), NOW()) "
        "ON DUPLICATE KEY UPDATE "
        "  session_id = IF(session_id = VALUES(session_id), VALUES(session_id), session_id), "
        "  last_heartbeat = IF(session_id = VALUES(session_id), NOW(), last_heartbeat)";
    se(conn, upsert, {std::to_string(user_id), session_id});

    auto r = sq(conn, "SELECT COUNT(*) as cnt FROM pop3_session_lock "
                      "WHERE user_id = ? AND session_id = ?",
                {std::to_string(user_id), session_id});
    return r && r->get_row_count() > 0 && safe_stoull(r->get_value(0, "cnt")) > 0;
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::release_lock(
    class IDBConnection* conn, uint64_t user_id, const std::string& session_id)
{
    if (!conn) return;
    se(conn, "DELETE FROM pop3_session_lock WHERE user_id = ? AND session_id = ?",
       {std::to_string(user_id), session_id});
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::renew_lock_heartbeat(
    const std::shared_ptr<router::IShardRouter>& router,
    uint64_t user_id, const std::string& session_id, int shard)
{
    if (!router) return false;
    auto pool = router->get_db_pool(static_cast<size_t>(shard));
    if (!pool) return false;
    auto conn = pool->acquire_connection();
    if (!conn.is_valid()) return false;

    // 条件续约（区别于抢锁的 upsert）：只在"仍持有这行锁"时刷新心跳。
    // 若行已被 sweeper 回收（心跳断太久）并可能转交新会话，这里不会
    // 反插抢占 —— 用 verify 确认所有权，false = 锁丢了，会话应关闭。
    se(conn.operator->(),
       "UPDATE pop3_session_lock SET last_heartbeat = NOW() "
       "WHERE user_id = ? AND session_id = ?",
       {std::to_string(user_id), session_id});
    auto r = sq(conn.operator->(),
        "SELECT COUNT(*) as cnt FROM pop3_session_lock "
        "WHERE user_id = ? AND session_id = ?",
        {std::to_string(user_id), session_id});
    return r && r->get_row_count() > 0 && safe_stoull(r->get_value(0, "cnt")) > 0;
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::sweep_expired_locks(
    const std::shared_ptr<router::IShardRouter>& router)
{
    if (!router) return false;
    bool all_ok = true;
    for (size_t shard = 0; shard < router->shard_count(); ++shard) {
        auto pool = router->get_db_pool(shard);
        if (!pool) continue;
        auto conn = pool->acquire_connection();
        if (!conn.is_valid()) { all_ok = false; continue; }
        // 心跳断 >5min 视为死锁（硬崩溃的会话不再续约），回收。
        // 与续约周期（60s）之间留足余量，正常 idle 会话不会被误杀。
        if (!se(conn.operator->(),
                "DELETE FROM pop3_session_lock WHERE last_heartbeat < NOW() - INTERVAL 5 MINUTE"))
            all_ok = false;
    }
    return all_ok;
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::start_heartbeat(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    std::shared_ptr<router::IShardRouter> router,
    std::shared_ptr<ThreadPoolBase> worker,
    std::chrono::milliseconds interval)
{
    if (!session || !router || !worker) return;
    auto* ctx = ctx_of(session);
    if (!ctx || !ctx->is_authenticated || ctx->session_id.empty()) return;
    if (ctx->heartbeat_timer) return;   // 幂等：已有心跳

    auto* srv = session->get_server();
    auto io_pool = srv ? std::dynamic_pointer_cast<IOThreadPool>(srv->m_ioThreadPool) : nullptr;
    if (!io_pool) return;
    boost::asio::io_context& io_ctx = io_pool->get_io_context();

    auto timer = std::make_shared<boost::asio::steady_timer>(io_ctx);
    ctx->heartbeat_timer = timer;

    uint64_t user_id = ctx->user_id;
    std::string sid = ctx->session_id;
    int shard = ctx->shard_index;

    // 递归续约任务。session/timer/tick 全部只持 weak 引用，避免
    // session ↔ timer ↔ handler 形成强引用环：session 持 timer/tick（强），
    // handler 只持 weak_self/weak_timer/weak_tick（弱）→ 无环，session 析构
    // 即释放定时器与回调。tick 本体由 session 持有（heartbeat_handler），
    // 每次 re-arm 通过 weak_tick 重新锁定。
    auto weak_self = std::weak_ptr<SessionBase<ConnectionType>>(session);
    auto weak_timer = std::weak_ptr<boost::asio::steady_timer>(timer);
    auto tick = std::make_shared<std::function<void(const boost::system::error_code&)>>();
    ctx->heartbeat_handler = tick;
    auto weak_tick = std::weak_ptr<std::function<void(const boost::system::error_code&)>>(tick);

    *tick = [weak_self, weak_timer, weak_tick, router, worker,
             user_id, sid, shard, interval](const boost::system::error_code& ec) {
        if (ec) return;   // 取消（operation_aborted）或 io 错误 → 停止续约
        // 续约放 worker 线程（DB I/O），不阻塞 io 线程
        worker->post([weak_self, weak_timer, weak_tick, router,
                      user_id, sid, shard, interval]() {
            auto self = weak_self.lock();
            if (!self || self->is_closed()) return;
            bool ok = TraditionalPop3Fsm<ConnectionType>::renew_lock_heartbeat(
                router, user_id, sid, shard);
            self = weak_self.lock();
            if (!self || self->is_closed()) return;
            if (!ok) {
                // 锁已被回收/转交：本会话失去排他性，异常关闭。
                // 无需 release_lock（WHERE 含 session_id，别人拿的锁删不掉）。
                LOG_SESSION_WARN("POP3 heartbeat lost lock for user={}, closing session", user_id);
                self->close();
                return;
            }
            // 续约成功 → 重新排下一次
            auto t = weak_timer.lock();
            auto next = weak_tick.lock();
            if (!t || !next) return;
            t->expires_after(interval);
            t->async_wait(*next);
        });
    };

    timer->expires_after(interval);
    timer->async_wait(*tick);
    LOG_SESSION_INFO("POP3 heartbeat started for user={} ({}ms)", user_id, interval.count());
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::apply_deletions(
    class IDBConnection* conn, uint64_t user_id, uint64_t mailbox_id,
    const std::set<uint64_t>& deleted_mail_ids)
{
    if (!conn || deleted_mail_ids.empty()) return true;
    // 全部数值参数直接拼接（仿 IMAP update_mail_deleted），避开 prepared
    // statement 参数顺序/个数不匹配问题。
    for (auto mid : deleted_mail_ids) {
        std::string sql = "UPDATE mail_mailbox SET is_deleted = 1"
            " WHERE mail_id = " + std::to_string(mid)
            + " AND user_id = " + std::to_string(user_id)
            + " AND mailbox_id = " + std::to_string(mailbox_id);
        se(conn, sql);
    }
    return se(conn, db::sql::build_imap_expunge_delete_mailbox(),
              {std::to_string(mailbox_id), std::to_string(user_id)});
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::init_transition_table() {
    this->add_transition(Pop3State::INIT, Pop3Event::CONNECT, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::USER, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::PASS, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::CAPA, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::NOOP, Pop3State::AUTHORIZATION);
    this->add_transition(Pop3State::AUTHORIZATION, Pop3Event::QUIT, Pop3State::UPDATE);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::STAT, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::LIST, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::UIDL, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::RETR, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::DELE, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::NOOP, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::RSET, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::CAPA, Pop3State::TRANSACTION);
    this->add_transition(Pop3State::TRANSACTION, Pop3Event::QUIT, Pop3State::UPDATE);
    // ERROR = 未知命令/空行：回 -ERR 并留在当前状态（不自闭）
    for (int i = 0; i <= static_cast<int>(Pop3State::UPDATE); ++i) {
        auto s = static_cast<Pop3State>(i);
        this->add_transition(s, Pop3Event::ERROR, s);
    }
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::init_state_handlers() {
    this->add_handler(Pop3State::INIT, Pop3Event::CONNECT,
        [this](auto session) { handle_init_connect(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::USER,
        [this](auto session) { handle_user(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::PASS,
        [this](auto session) { handle_pass(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::CAPA,
        [this](auto session) { handle_capa(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::NOOP,
        [this](auto session) { handle_noop(session); });
    this->add_handler(Pop3State::AUTHORIZATION, Pop3Event::QUIT,
        [this](auto session) { handle_quit(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::STAT,
        [this](auto session) { handle_stat(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::LIST,
        [this](auto session) { handle_list(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::UIDL,
        [this](auto session) { handle_uidl(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::RETR,
        [this](auto session) { handle_retr(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::DELE,
        [this](auto session) { handle_dele(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::NOOP,
        [this](auto session) { handle_noop(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::RSET,
        [this](auto session) { handle_rset(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::CAPA,
        [this](auto session) { handle_capa(session); });
    this->add_handler(Pop3State::TRANSACTION, Pop3Event::QUIT,
        [this](auto session) { handle_quit(session); });
    for (int i = 0; i <= static_cast<int>(Pop3State::UPDATE); ++i) {
        auto s = static_cast<Pop3State>(i);
        this->add_handler(s, Pop3Event::ERROR,
            [this](auto session) { handle_error(session); });
    }
}

template <typename ConnectionType>
bool TraditionalPop3Fsm<ConnectionType>::is_terminal_state(Pop3State s) const {
    return s == Pop3State::CLOSED;
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::on_invalid_transition(
    Pop3State, Pop3Event, std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    send_line(session, "-ERR Command not allowed in current state");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::on_handler_not_found(
    Pop3State, Pop3Event, std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    send_line(session, "-ERR Internal: handler missing");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::process_event(
    std::shared_ptr<SessionBase<ConnectionType>> session, Pop3Event event)
{
    if (!session) return;
    Pop3State cur = static_cast<Pop3State>(session->get_current_state());
    this->dispatch(session, cur, event);
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::auto_process_event(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    if (!session) return;
    process_event(session, static_cast<Pop3Event>(session->get_next_event()));
}

// ====================================================================
// 状态处理器
// ====================================================================

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_init_connect(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    long now = static_cast<long>(time(nullptr));
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld", now);
    send_line(session, std::string("+OK ProtoRelay POP3 server ready <") + ts + "@pop3>");
    session->set_current_state(static_cast<int>(Pop3State::AUTHORIZATION));
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_capa(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    send_line(session, "+OK Capability list follows");
    send_line(session, "USER");
    send_line(session, "UIDL");
    send_line(session, "IMPLEMENTATION ProtoRelay-POP3");
    send_line(session, ".");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_user(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (args.empty()) { send_line(session, "-ERR USER requires a name argument"); return; }
    ctx->clear();
    ctx->username = args;
    send_line(session, "+OK Send PASS");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_pass(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    if (!ctx->is_authenticated && ctx->username.empty()) {
        send_line(session, "-ERR USER first");
        return;
    }
    std::string password = session->get_last_command_args();
    if (password.empty()) {
        send_line(session, "-ERR PASS requires a password");
        return;
    }

    // 切到 worker 线程做 bcrypt + DB 查
    session->set_paused(true);
    auto self = session->shared_from_this();
    auto router = m_shardRouter;
    auto auth_cache = m_authCache;
    auto worker = m_workerThreadPool;
    std::string email = ctx->username;
    std::string pwd = std::move(password);
    std::string new_session_id = std::to_string(
        algorithm::get_snowflake_generator().next_id());
    auto* srv = session->get_server();

    auto finish_fail = [self, srv](const std::string& reply, const std::string& metric_label) {
        if (!self || self->is_closed()) return;
        self->set_paused(false);
        send_line(self, reply);
        if (srv) {
            if (auto m = srv->get_metrics().lock()) {
                m->inc_counter("protorelay_pop3_auth_total",
                               {{"result", metric_label}}, 1);
            }
        }
        self->drain_buffered_commands();
    };

    if (!worker) {
        finish_fail("-ERR Server not ready", "no_worker");
        return;
    }

    worker->post([self, router, auth_cache, email, pwd = std::move(pwd),
                  new_session_id = std::move(new_session_id),
                  srv, finish_fail = std::move(finish_fail),
                  worker, interval = this->heartbeat_interval_]() mutable {
        uint64_t uid = 0;
        int shard = 0;
        bool ok = TraditionalPop3Fsm<ConnectionType>::auth_user(
            router, auth_cache, email, pwd, uid, shard);
        if (!ok) {
            // 失败计数只在"真正鉴权失败"时计数（与 SMTP/IMAP 一致）
            if (self && !self->is_closed() && self->record_auth_failure_and_check()) {
                self->set_paused(false);
                send_line(self, "-ERR Too many auth failures, closing connection");
                if (srv) {
                    if (auto m = srv->get_metrics().lock()) {
                        m->inc_counter("protorelay_pop3_auth_total",
                                       {{"result", "fail_too_many"}}, 1);
                    }
                }
                self->close();
                return;
            }
            finish_fail("-ERR Authentication failed", "wrong_pass");
            return;
        }

        auto pool = router ? router->get_db_pool(static_cast<size_t>(shard)) : nullptr;
        if (!pool) {
            finish_fail("-ERR Server configuration error", "no_pool");
            return;
        }
        auto conn = pool->acquire_connection();
        if (!conn.is_valid()) {
            finish_fail("-ERR Server database unavailable", "no_conn");
            return;
        }
        uint64_t inbox_id = TraditionalPop3Fsm<ConnectionType>::get_inbox_id(
            conn.operator->(), uid);
        if (inbox_id == 0) {
            finish_fail("-ERR No INBOX for user", "no_inbox");
            return;
        }

        if (!TraditionalPop3Fsm<ConnectionType>::acquire_lock(
                conn.operator->(), uid, new_session_id)) {
            if (srv) {
                if (auto m = srv->get_metrics().lock()) {
                    m->inc_counter("protorelay_pop3_lock_conflict_total", {}, 1);
                }
            }
            finish_fail("-ERR [IN-USE] Mailbox lock busy, try later", "lock_conflict");
            return;
        }

        // 拉邮件列表 + 每封 size（用 storage provider）
        std::vector<Pop3Message> mails;
        TraditionalPop3Fsm<ConnectionType>::get_inbox_mails(
            conn.operator->(), inbox_id, uid, mails);

        std::shared_ptr<storage::IStorageProvider> provider;
        if (router) provider = router->get_storage(static_cast<size_t>(shard));
        if (provider) {
            for (auto& mm : mails) {
                storage::IoError err;
                uint64_t sz = 0;
                if (provider->object_size(mm.body_path, sz, err)) {
                    mm.size = sz;
                }
            }
        }

        // 写回 ctx
        if (!self || self->is_closed()) return;
        auto* c = static_cast<Pop3Context*>(self->get_context());
        if (c) {
            c->is_authenticated = true;
            c->user_id = uid;
            c->mailbox_id = inbox_id;
            c->shard_index = shard;
            c->messages = std::move(mails);
            c->deleted.clear();
            c->session_id = new_session_id;
        }

        // 锁拿到后启动心跳续约（v2）：防会话硬崩溃后锁泄漏死锁
        start_heartbeat(self, router, worker, interval);

        self->set_paused(false);
        size_t n = c ? c->messages.size() : 0;
        uint64_t total = 0;
        if (c) for (auto& mm : c->messages) total += mm.size;
        send_line(self, "+OK Mailbox locked and loaded, " + std::to_string(n) +
                        " messages (" + std::to_string(total) + " octets)");
        self->set_current_state(static_cast<int>(Pop3State::TRANSACTION));
        if (srv) {
            if (auto m = srv->get_metrics().lock()) {
                m->inc_counter("protorelay_pop3_auth_total",
                               {{"result", "ok"}}, 1);
            }
        }
        self->drain_buffered_commands();
    });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_stat(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    uint64_t total = 0;
    size_t count = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        total += m.size;
        ++count;
    }
    send_line(session, "+OK " + std::to_string(count) + " " + std::to_string(total));
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_list(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (!args.empty()) {
        uint64_t n = safe_stoull(args);
        if (n < 1 || n > ctx->messages.size()) {
            send_line(session, "-ERR no such message");
            return;
        }
        size_t seen = 0;
        for (auto& m : ctx->messages) {
            if (ctx->deleted.count(m.mail_id)) continue;
            if (++seen == n) {
                send_line(session, "+OK " + std::to_string(n) + " " + std::to_string(m.size));
                return;
            }
        }
        send_line(session, "-ERR no such message");
        return;
    }
    uint64_t total = 0;
    size_t count = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        total += m.size; ++count;
    }
    send_line(session, "+OK " + std::to_string(count) + " messages (" +
                       std::to_string(total) + " octets)");
    size_t n = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        send_line(session, std::to_string(++n) + " " + std::to_string(m.size));
    }
    send_line(session, ".");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_uidl(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (!args.empty()) {
        uint64_t n = safe_stoull(args);
        if (n < 1 || n > ctx->messages.size()) {
            send_line(session, "-ERR no such message");
            return;
        }
        size_t seen = 0;
        for (auto& m : ctx->messages) {
            if (ctx->deleted.count(m.mail_id)) continue;
            if (++seen == n) {
                send_line(session, "+OK " + std::to_string(n) + " " + std::to_string(m.mail_id));
                return;
            }
        }
        send_line(session, "-ERR no such message");
        return;
    }
    send_line(session, "+OK Unique-ID listing follows");
    size_t n = 0;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        send_line(session, std::to_string(++n) + " " + std::to_string(m.mail_id));
    }
    send_line(session, ".");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_retr(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (args.empty()) { send_line(session, "-ERR RETR requires a message number"); return; }
    uint64_t n = safe_stoull(args);
    if (n < 1 || n > ctx->messages.size()) {
        send_line(session, "-ERR no such message");
        return;
    }
    size_t seen = 0;
    Pop3Message target;
    bool found = false;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        if (++seen == n) { target = m; found = true; break; }
    }
    if (!found) { send_line(session, "-ERR no such message"); return; }

    // 在 worker 线程读 body + dot-stuffing（避免 io 线程被大邮件卡住）
    session->set_paused(true);
    auto self = session->shared_from_this();
    auto router = m_shardRouter;
    auto worker = m_workerThreadPool;
    int shard = ctx->shard_index;
    auto* srv = session->get_server();
    if (!worker) {
        session->set_paused(false);
        send_line(session, "-ERR Server not ready");
        return;
    }
    worker->post([self, router, shard, target, srv]() {
        std::string body;
        bool read_ok = false;
        if (router) {
            if (auto provider = router->get_storage(static_cast<size_t>(shard))) {
                storage::IoError err;
                read_ok = provider->read_all(target.body_path, body, err);
            }
        }
        if (!read_ok) {
            if (self && !self->is_closed()) {
                self->set_paused(false);
                send_line(self, "-ERR Failed to read message body");
                self->drain_buffered_commands();
            }
            return;
        }
        // RFC 1939 §3.3 dot-stuffing：每行以 "." 开头则前缀 "."
        // 逐行转 CRLF；结尾的 '\n' 不产生额外空行（否则终止符前会多一行）
        std::string out;
        out.reserve(body.size() + 64);
        size_t start = 0;
        while (start < body.size()) {
            size_t eol = body.find('\n', start);
            size_t line_end = (eol == std::string::npos) ? body.size() : eol;
            std::string line = body.substr(start, line_end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '.') line.insert(line.begin(), '.');
            out += line;
            out += "\r\n";
            if (eol == std::string::npos) break;
            start = eol + 1;
        }
        if (!self || self->is_closed()) return;
        send_line(self, "+OK " + std::to_string(target.size) + " octets");
        self->do_async_write(out);
        send_line(self, ".");
        if (srv) {
            if (auto m = srv->get_metrics().lock()) {
                m->inc_counter("protorelay_pop3_retr_total", {}, 1);
            }
        }
        self->set_paused(false);
        self->drain_buffered_commands();
    });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_dele(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (!ctx) { send_line(session, "-ERR Internal: no context"); return; }
    std::string args = session->get_last_command_args();
    if (args.empty()) { send_line(session, "-ERR DELE requires a message number"); return; }
    uint64_t n = safe_stoull(args);
    if (n < 1 || n > ctx->messages.size()) {
        send_line(session, "-ERR no such message");
        return;
    }
    size_t seen = 0;
    uint64_t target_id = 0;
    bool found = false;
    for (auto& m : ctx->messages) {
        if (ctx->deleted.count(m.mail_id)) continue;
        if (++seen == n) { target_id = m.mail_id; found = true; break; }
    }
    if (!found) { send_line(session, "-ERR no such message"); return; }
    if (ctx->deleted.count(target_id)) {
        send_line(session, "-ERR message already deleted");
        return;
    }
    ctx->deleted.insert(target_id);
    send_line(session, "+OK message " + std::to_string(n) + " deleted");
    if (auto* srv = session->get_server()) {
        if (auto m = srv->get_metrics().lock()) {
            m->inc_counter("protorelay_pop3_dele_total", {}, 1);
        }
    }
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_noop(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    send_line(session, "+OK");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_rset(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    auto* ctx = ctx_of(session);
    if (ctx) ctx->deleted.clear();
    send_line(session, "+OK");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_quit(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session) return;
    session->set_trace_clean_close();   // 正常 QUIT → 连接追踪丢弃
    auto* ctx = ctx_of(session);
    if (!ctx || !ctx->is_authenticated) {
        // AUTHORIZATION 阶段 QUIT：直接 bye
        send_line(session, "+OK Bye");
        session->set_current_state(static_cast<int>(Pop3State::UPDATE));
        session->close();
        return;
    }

    // TRANSACTION 阶段 QUIT：worker 线程 apply deletions + release lock
    session->set_paused(true);
    auto self = session->shared_from_this();
    auto router = m_shardRouter;
    auto worker = m_workerThreadPool;
    uint64_t user_id = ctx->user_id;
    uint64_t mailbox_id = ctx->mailbox_id;
    int shard = ctx->shard_index;
    std::string sid = ctx->session_id;
    std::set<uint64_t> del = ctx->deleted;
    if (!worker) {
        send_line(session, "+OK Bye");
        session->set_current_state(static_cast<int>(Pop3State::UPDATE));
        session->close();
        return;
    }
    worker->post([self, router, user_id, mailbox_id, shard, sid, del]() {
        if (router) {
            auto pool = router->get_db_pool(static_cast<size_t>(shard));
            if (pool) {
                auto conn = pool->acquire_connection();
                if (conn.is_valid()) {
                    TraditionalPop3Fsm<ConnectionType>::apply_deletions(
                        conn.operator->(), user_id, mailbox_id, del);
                    TraditionalPop3Fsm<ConnectionType>::release_lock(
                        conn.operator->(), user_id, sid);
                }
            }
        }
        if (!self || self->is_closed()) return;
        self->set_paused(false);
        send_line(self, "+OK Bye");
        self->set_current_state(static_cast<int>(Pop3State::UPDATE));
        self->close();
    });
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_error(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    // 未知命令/空行 → 回 -ERR，连接保持（用户在 AUTHORIZATION 可重试）
    send_line(session, "-ERR Unknown command");
}

template <typename ConnectionType>
void TraditionalPop3Fsm<ConnectionType>::handle_timeout(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    if (!session || session->is_closed()) return;
    // 异常结束：释放 mailbox 锁再关（abrupt disconnect 的锁泄漏由 v2 sweeper 兜底）
    auto* ctx = ctx_of(session);
    if (ctx && ctx->is_authenticated && !ctx->session_id.empty() && m_shardRouter) {
        auto pool = m_shardRouter->get_db_pool(
            static_cast<size_t>(ctx->shard_index));
        if (pool) {
            auto conn = pool->acquire_connection();
            if (conn.is_valid()) {
                release_lock(conn.operator->(), ctx->user_id, ctx->session_id);
            }
        }
    }
    session->close();
}

} // namespace mail_system

#endif // TRADITIONAL_POP3_FSM_TPP
