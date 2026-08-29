#ifndef TRADITIONAL_IMAPS_FSM_H
#define TRADITIONAL_IMAPS_FSM_H

#include "framework/session_base.h"
#include "framework/fsm_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/sql_queries.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_types.hpp"
#include "mail_system/back/mailServer/fsm/imaps/imap_utils.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/lru_cache.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/router/i_shard_router.h"
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <regex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <iomanip>

namespace mail_system {

template <typename ConnectionType>
class TraditionalImapsFsm : public FsmBase<ConnectionType, ImapState, ImapEvent> {
public:
    using MailboxStatsCache = LruCache<std::string, MailboxCacheEntry>;
    std::shared_ptr<AuthCache> m_authCache = std::make_shared<AuthCache>();

protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<MailboxStatsCache> m_mailboxStatsCache;
    // SELECT/STATUS 统计的 single-flight：同一 (user,mailbox) 只允许一个回源查询链
    // 在途，其余并发 SELECT 挂到等待列表上（owner 完成后统一通知），防缓存
    // miss/stale 时 N 个连接各自查库打爆数据库。
    struct StatsFlight {
        std::vector<std::function<void(MailboxCacheEntry, bool, bool)>> waiters;
    };
    std::unordered_map<std::string, std::shared_ptr<StatsFlight>> m_statsFlights;
    std::mutex m_statsFlightsMutex;

public:
    TraditionalImapsFsm(
        std::shared_ptr<ThreadPoolBase> io_thread_pool,
        std::shared_ptr<ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<router::IShardRouter> shard_router
    ) : m_ioThreadPool(io_thread_pool),
        m_workerThreadPool(worker_thread_pool),
        m_shardRouter(std::move(shard_router)) {
        init_transition_table();
        init_state_handlers();
    }

    ~TraditionalImapsFsm() override = default;

    void set_mailbox_stats_cache(std::shared_ptr<MailboxStatsCache> cache) { m_mailboxStatsCache = cache; }
    std::shared_ptr<MailboxStatsCache> get_mailbox_stats_cache() const { return m_mailboxStatsCache; }

    std::shared_ptr<ScopedConnection> acquire_connection(int shard) {
        auto pool = m_shardRouter ? m_shardRouter->get_db_pool(static_cast<size_t>(shard)) : nullptr;
        if (!pool) {
            LOG_DATABASE_ERROR("No DB pool for shard {}", shard);
            return ScopedConnection::invalid();
        }
        return pool->acquire_connection();
    }

    std::shared_ptr<storage::IStorageProvider> get_storage(int shard) {
        return m_shardRouter->get_storage(static_cast<size_t>(shard));
    }

    // 事件处理入口
    void process_event(
        std::shared_ptr<SessionBase<ConnectionType>> session,
        ImapEvent event,
        const std::string& tag);

    // 自动处理（从 session 获取当前 pending 事件）
    void auto_process_event(std::shared_ptr<SessionBase<ConnectionType>> session);

    // ========== 状态/事件名称查询 ==========
    static std::string get_state_name(ImapState state) {
        static const std::unordered_map<ImapState, std::string> names = {
            {ImapState::INIT, "INIT"},
            {ImapState::NOT_AUTHENTICATED, "NOT_AUTHENTICATED"},
            {ImapState::AUTHENTICATED, "AUTHENTICATED"},
            {ImapState::SELECTED, "SELECTED"},
            {ImapState::LOGOUT, "LOGOUT"},
            {ImapState::CLOSED, "CLOSED"}
        };
        auto it = names.find(state);
        return it != names.end() ? it->second : "UNKNOWN_STATE";
    }

    static std::string get_event_name(ImapEvent event) {
        static const std::unordered_map<ImapEvent, std::string> names = {
            {ImapEvent::CONNECT, "CONNECT"},
            {ImapEvent::CAPABILITY, "CAPABILITY"},
            {ImapEvent::LOGIN, "LOGIN"},
            {ImapEvent::AUTHENTICATE, "AUTHENTICATE"},
            {ImapEvent::LOGOUT, "LOGOUT"},
            {ImapEvent::SELECT, "SELECT"},
            {ImapEvent::EXAMINE, "EXAMINE"},
            {ImapEvent::CREATE, "CREATE"},
            {ImapEvent::DELETE, "DELETE"},
            {ImapEvent::RENAME, "RENAME"},
            {ImapEvent::SUBSCRIBE, "SUBSCRIBE"},
            {ImapEvent::UNSUBSCRIBE, "UNSUBSCRIBE"},
            {ImapEvent::LIST, "LIST"},
            {ImapEvent::LSUB, "LSUB"},
            {ImapEvent::IMAP_STATUS, "STATUS"},
            {ImapEvent::APPEND, "APPEND"},
            {ImapEvent::CHECK, "CHECK"},
            {ImapEvent::CLOSE, "CLOSE"},
            {ImapEvent::EXPUNGE, "EXPUNGE"},
            {ImapEvent::SEARCH, "SEARCH"},
            {ImapEvent::FETCH, "FETCH"},
            {ImapEvent::STORE, "STORE"},
            {ImapEvent::COPY, "COPY"},
            {ImapEvent::MOVE, "MOVE"},
            {ImapEvent::UID, "UID"},
            {ImapEvent::NOOP, "NOOP"},
            {ImapEvent::IDLE, "IDLE"},
            {ImapEvent::DONE, "DONE"},
            {ImapEvent::STARTTLS, "STARTTLS"},
            {ImapEvent::ERROR, "ERROR"},
            {ImapEvent::TIMEOUT, "TIMEOUT"}
        };
        auto it = names.find(event);
        return it != names.end() ? it->second : "UNKNOWN_EVENT";
    }

    // ========== 数据库操作（CPS：async_query/async_execute 链） ==========
    // 全部 static：可在 worker/异步回调里调用，依赖显式传入。
    // 底层 MySQL async_* 目前是默认同步包装（回调同步触发），但调用方结构
    // 已异步就绪；将来接真异步 DB 时这些调用方无需改动。conn 由调用方持有的
    // shared ScopedConnection 保活，链中每个回调都捕获它避免悬垂。

    // 用户认证（复用和 SMTP 相同的 users 表）
    // 登录认证（查 DB + bcrypt）。bcrypt 几十~几百 ms 纯 CPU，调用方
    // 必须在 worker 线程发起本函数（见 handle_login）。
    static void auth_user_async(const std::shared_ptr<router::IShardRouter>& shard_router,
                                const std::shared_ptr<AuthCache>& auth_cache,
                                const std::string& mail_address,
                                const std::string& password,
                                std::function<void(bool ok, uint64_t user_id, int shard)> cb);

    // 邮箱列表（LIST/LSUB）。失败 cb(false, {})。
    static void get_mailboxes_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                    std::function<void(bool,
                                        std::vector<std::tuple<uint64_t, std::string, int>>)> cb);

    // 按名称找邮箱 id（含 INBOX 兜底）。失败 cb(0)。
    static void find_mailbox_id_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                      const std::string& mailbox_name,
                                      std::function<void(uint64_t)> cb);

    // 邮箱内邮件列表（FETCH/SEARCH/STORE/EXPUNGE/COPY/MOVE 用）。
    static void get_mailbox_mails_async(std::shared_ptr<ScopedConnection> conn,
                                        uint64_t mailbox_id, uint64_t user_id,
                                        std::function<void(bool, std::vector<MailboxMailInfo>)> cb);

    static void get_mail_info_async(std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
                                    std::function<void(bool, MailboxMailInfo)> cb);

    static void get_mail_sender_async(std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
                                      std::function<void(std::string)> cb);

    static void get_mail_recipients_async(std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
                                          std::function<void(std::vector<std::string>)> cb);

    static void get_user_email_async(std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
                                     std::function<void(std::string)> cb);

    // 邮件持久化（APPEND）。storage 写入 + DB 插入；调用方应在 worker 线程发起
    // （storage append 是阻塞 I/O，不许在 io 线程）。cb(mail_id, body_path, error)，
    // 失败 mail_id=0。
    static void create_mail_async(const std::shared_ptr<storage::IStorageProvider>& storage,
                                  std::shared_ptr<ScopedConnection> conn,
                                  const std::string& subject, const std::string& body_content,
                                  std::function<void(uint64_t, std::string, std::string)> cb);

    static void link_mail_to_mailbox_async(std::shared_ptr<ScopedConnection> conn,
                                           uint64_t mail_id, uint64_t user_id,
                                           uint64_t mailbox_id,
                                           const std::string& sender,
                                           const std::string& recipient,
                                           int status, std::function<void(bool)> cb);

    // 缓存感知的邮箱统计（CPS）。缓存命中且未过期时 cb 同步触发。
    // 本函数在 miss/stale 时自行回源并写回缓存，无需调用方再刷。
    void get_mailbox_stats_cached_async(std::shared_ptr<ScopedConnection> conn,
                                        uint64_t user_id, uint64_t mailbox_id,
                                        std::function<void(MailboxCacheEntry, bool, bool)> cb);

    static void get_mailbox_count_async(std::shared_ptr<ScopedConnection> conn,
                                        uint64_t mailbox_id, uint64_t user_id,
                                        std::function<void(size_t)> cb);

    static void get_mailbox_unseen_count_async(std::shared_ptr<ScopedConnection> conn,
                                               uint64_t mailbox_id, uint64_t user_id,
                                               std::function<void(size_t)> cb);

    static void get_mailbox_uidnext_async(std::shared_ptr<ScopedConnection> conn,
                                          uint64_t mailbox_id, uint64_t user_id,
                                          std::function<void(uint64_t)> cb);

    // 批量 flag 更新（STORE）：单条 IN 列表 SQL 拍平 N 循环。
    static void batch_mark_seen_async(std::shared_ptr<ScopedConnection> conn,
                                      const std::vector<uint64_t>& mail_ids,
                                      const std::string& recipient, int status,
                                      std::function<void(bool)> cb);
    static void batch_mark_deleted_async(std::shared_ptr<ScopedConnection> conn,
                                         const std::vector<uint64_t>& mail_ids,
                                         uint64_t user_id, uint64_t mailbox_id, int deleted,
                                         std::function<void(bool)> cb);
    static void batch_mark_flagged_async(std::shared_ptr<ScopedConnection> conn,
                                         const std::vector<uint64_t>& mail_ids,
                                         uint64_t user_id, uint64_t mailbox_id, int flagged,
                                         std::function<void(bool)> cb);

    // EXPUNGE：物理删除已标记 \Deleted 的行。
    static void expunge_mailbox_async(std::shared_ptr<ScopedConnection> conn,
                                      uint64_t mailbox_id, uint64_t user_id,
                                      std::function<void(bool)> cb);

    // COPY/MOVE 批量插入：INSERT IGNORE ... VALUES (...),(...)（mail_mailbox 有
    // UNIQUE KEY uk_mail_box_user 自动去重 + id AUTO_INCREMENT）。cb 报告实际插入数
    //（通过 SELECT ROW_COUNT()，dup 被 IGNORE 的不计入）。
    static void batch_insert_mailbox_async(std::shared_ptr<ScopedConnection> conn,
                                           const std::vector<uint64_t>& mail_ids,
                                           uint64_t target_id, uint64_t user_id,
                                           std::function<void(size_t)> cb);

    // MOVE 源邮箱删除：单条 UPDATE ... IN（仅标记 is_deleted，expunge 时物理删）。
    static void batch_mark_move_deleted_async(std::shared_ptr<ScopedConnection> conn,
                                              const std::vector<uint64_t>& mail_ids,
                                              uint64_t user_id, uint64_t source_mailbox_id,
                                              std::function<void(bool)> cb);

    // 通用 IMAP 响应写回
    void send_untagged(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& data);
    // static：只触碰 session，可在无 this 的回调（worker/异步续作）里调用
    static void send_tagged(std::shared_ptr<SessionBase<ConnectionType>> session,
                            const std::string& tag,
                            const std::string& status,
                            const std::string& message);
    void send_continuation(std::shared_ptr<SessionBase<ConnectionType>> session,
                           const std::string& message);

private:
    // 初始化
    void init_transition_table();
    void init_state_handlers();

    // FsmBase hooks
    using BaseFsm = FsmBase<ConnectionType, ImapState, ImapEvent>;
    using Handler = typename BaseFsm::Handler;
    bool is_terminal_state(ImapState s) const override;
    void on_invalid_transition(ImapState s, ImapEvent e,
        std::shared_ptr<SessionBase<ConnectionType>> session) override;
    void on_handler_not_found(ImapState s, ImapEvent e,
        std::shared_ptr<SessionBase<ConnectionType>> session) override;
    void invoke_handler(Handler& h,
        std::shared_ptr<SessionBase<ConnectionType>> session) override;

    // ========== 状态处理器 ==========
    void handle_init_connect(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_capability(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_login(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_authenticate(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_logout(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_select(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_examine(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_list(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_status(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_fetch(std::shared_ptr<SessionBase<ConnectionType>> session,
                      bool is_uid = false);

    // FETCH 的续作链状态与驱动器（定义在 .tpp）。读取走 provider 的
    // async 接口：本地内联，远程后端由装饰器投递到 worker —— 逐封
    // size/正文读取不再阻塞 io 线程。static：回调里无 this。
    // 续作链迭代化（不递归）：每封一个 shared atomic alive，size/body 两个异步读
    // 共享。完成本封的最后一步走 fetch_continue——inline 回调（外层 fetch_drive
    // 帧仍在）→ 外层循环 continue；deferred 回调（外层已返回）→ 由回调驱动下一封。
    // 避免本地 storage 内联回调导致的逐封栈递归（大邮箱 >~200 封爆栈）。
    struct FetchContext;
    static void fetch_drive(std::shared_ptr<SessionBase<ConnectionType>> session,
                            std::shared_ptr<FetchContext> ctx);
    // 完成本封剩余 item 后的续作：inline 时外层循环续，deferred 时驱动下一封
    static void fetch_continue(std::shared_ptr<SessionBase<ConnectionType>> session,
                               std::shared_ptr<FetchContext> ctx,
                               std::shared_ptr<std::atomic<bool>> alive);
    // 本封的 envelope + 正文阶段。返回 true = 已发起异步正文读取（回调里 fetch_continue）；
    // false = 正文已同步就绪并完成本封。
    static bool fetch_after_size(std::shared_ptr<SessionBase<ConnectionType>> session,
                                 std::shared_ptr<FetchContext> ctx,
                                 std::shared_ptr<std::atomic<bool>> alive);
    // 用已就绪的正文完成当前邮件剩余 item（纯数据推进，不触碰 session）
    static void fetch_complete_mail_with_body(FetchContext& ctx,
                                              std::string body_content);
    static void fetch_finalize(std::shared_ptr<SessionBase<ConnectionType>> session,
                               std::shared_ptr<FetchContext> ctx);
    void handle_store(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_expunge(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_close(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_noop(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_check(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_create(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_delete(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_rename(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_subscribe(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_unsubscribe(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_lsub(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_append(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_search(std::shared_ptr<SessionBase<ConnectionType>> session,
                       bool is_uid = false);
    void handle_uid(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_starttls(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_copy(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_move(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_copy_move(std::shared_ptr<SessionBase<ConnectionType>> session, bool is_move);
    void handle_idle(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_done(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_error(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_timeout(std::shared_ptr<SessionBase<ConnectionType>> session);

    // 辅助函数
};

} // namespace mail_system


#endif // TRADITIONAL_IMAPS_FSM_H
