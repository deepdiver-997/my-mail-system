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
    // 邮箱统计后台刷新的在途 key 集合：同一 key 只允许一个刷新任务在跑，避免雪崩
    std::set<std::string> m_statsRefreshInFlight;
    std::mutex m_statsRefreshMutex;

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

    ScopedConnection acquire_connection(int shard) {
        auto pool = m_shardRouter->get_db_pool(static_cast<size_t>(shard));
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

    // ========== 数据库操作 ==========

    // 用户认证（复用和 SMTP 相同的 users 表）
    bool auth_user(SessionBase<ConnectionType>* session,
                   const std::string& mail_address,
                   const std::string& password,
                   uint64_t& out_user_id,
                   int& out_shard);

    bool get_mailboxes(uint64_t user_id,
                       std::vector<std::tuple<uint64_t, std::string, int>>& mailboxes);

    uint64_t find_mailbox_id(uint64_t user_id, const std::string& mailbox_name);

    bool get_mailbox_mails(uint64_t mailbox_id, uint64_t user_id,
                           std::vector<MailboxMailInfo>& mails);

    bool get_mail_info(uint64_t mail_id, MailboxMailInfo& info);

    std::string get_mail_sender(uint64_t mail_id);

    std::vector<std::string> get_mail_recipients(uint64_t mail_id);

    std::string get_user_email(uint64_t user_id);

    bool update_mail_seen(uint64_t mail_id, const std::string& recipient, bool seen);

    bool update_mail_deleted(uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id, bool deleted);

    bool update_mail_flagged(uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id, bool flagged);

    // 邮件持久化
    uint64_t create_mail(const std::string& subject, const std::string& body_content,
                         std::string& out_body_path, std::string& error);

    bool link_mail_to_mailbox(uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id,
                              const std::string& sender, const std::string& recipient,
                              int status);

    // IMAP-UTF-7 解码
    static std::string decode_imap_utf7(const std::string& imap7);

    // 从存储读取邮件内容
    std::string read_mail_body(const std::string& body_path);

    // 缓存感知的邮箱统计
    MailboxCacheEntry get_mailbox_stats_cached(
        uint64_t user_id, uint64_t mailbox_id,
        bool& from_cache_out, bool& stale_out);

    size_t get_mailbox_count(uint64_t mailbox_id, uint64_t user_id);

    size_t get_mailbox_unseen_count(uint64_t mailbox_id, uint64_t user_id);

    uint64_t get_mailbox_uidnext(uint64_t mailbox_id, uint64_t user_id);

    void expunge_mailbox(uint64_t mailbox_id, uint64_t user_id);

    std::vector<uint64_t> get_expunged_ids(uint64_t mailbox_id, uint64_t user_id);

    // 通用 IMAP 响应写回
    void send_untagged(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& data);
    void send_tagged(std::shared_ptr<SessionBase<ConnectionType>> session,
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

    // ========== 工具方法 ==========
    static std::string imap_timestamp(time_t t);
    static std::string quote_string(const std::string& s);
    static std::string encode_mailbox_name(const std::string& name);
    static std::string decode_mailbox_name(const std::string& imap7);
    static std::string build_flags_string(int status, bool starred, bool deleted, bool important);
    static std::string build_envelope_string(
        const std::string& date_str,
        const std::string& subject,
        const std::string& from,
        const std::string& sender,
        const std::string& reply_to,
        const std::string& to,
        const std::string& cc,
        const std::string& bcc,
        const std::string& in_reply_to,
        const std::string& message_id);
    static std::string build_fetch_body_response(
        const std::string& body_content,
        size_t octets);
public:
    // 以下纯静态工具方法对外暴露以便单元测试（不依赖实例状态）
    static std::string build_bodystructure(const std::string& raw);
    static std::string build_bodystructure_tree(const MimePart& mp);
    static std::string extract_part_content(const std::string& raw, const MimePart& part);
private:
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
