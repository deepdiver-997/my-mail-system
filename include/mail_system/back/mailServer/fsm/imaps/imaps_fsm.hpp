#ifndef IMAPS_FSM_H
#define IMAPS_FSM_H

#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/router/i_shard_router.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/algorithm/snow.h"
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <fstream>

namespace mail_system {

// stoull/stoi/stoll 安全包装 — 非数字输入返回 0
inline uint64_t safe_stoull(const std::string& s) {
    try { return std::stoull(s); }
    catch (const std::invalid_argument&) { return 0; }
    catch (const std::out_of_range&)     { return 0; }
}

// ====================================================================
// IMAP 状态枚举 (RFC 3501 §3 基本状态)
// ====================================================================
enum class ImapState {
    INIT = 0,                // 连接刚建立
    NOT_AUTHENTICATED = 1,   // 等待 LOGIN / AUTHENTICATE
    AUTHENTICATED = 2,       // 已登录，无选中的邮箱
    SELECTED = 3,            // 已选中一个邮箱
    LOGOUT = 4,              // LOGOUT 发送完毕
    CLOSED = 5               // 连接关闭
};

// ====================================================================
// IMAP 事件枚举 —— 等价于命令名映射
// ====================================================================
enum class ImapEvent {
    CONNECT = 0,
    CAPABILITY = 1,
    LOGIN = 2,
    AUTHENTICATE = 3,
    LOGOUT = 4,
    SELECT = 5,
    EXAMINE = 6,
    CREATE = 7,
    DELETE = 8,
    RENAME = 9,
    SUBSCRIBE = 10,
    UNSUBSCRIBE = 11,
    LIST = 12,
    LSUB = 13,
    IMAP_STATUS = 14,   // 避免与 MySQL headers 中的 STATUS 宏冲突
    APPEND = 15,
    CHECK = 16,
    CLOSE = 17,
    EXPUNGE = 18,
    SEARCH = 19,
    FETCH = 20,
    STORE = 21,
    COPY = 22,
    MOVE = 23,
    UID = 24,
    NOOP = 25,
    IDLE = 26,
    DONE = 27,
    STARTTLS = 28,
    ERROR = 29,
    TIMEOUT = 30
};

// ====================================================================
// IMAP 上下文 —— 存储每次会话的状态
// ====================================================================
struct ImapContext {
    // 认证
    bool is_authenticated = false;
    std::string username;                     // 登录名（邮箱地址）
    uint64_t user_id = 0;                     // users.id
    int shard_index = 0;                     // 由 shard router 在认证时分配

    // 命令标签
    std::string current_tag;                  // 当前命令的 tag，响应时回显

    // 选中的邮箱
    bool mailbox_selected = false;
    std::string selected_mailbox_name;        // 邮箱名称，如 "INBOX"
    uint64_t selected_mailbox_id = 0;         // mailboxes.id
    uint64_t uid_validity = 0;               // UIDVALIDITY 值

    // 读写模式
    bool read_only = false;                   // true = EXAMINE, false = SELECT

    // IDLE
    bool idle_mode = false;                   // 是否处于 IDLE 状态

    // UID 命令标记（handle_uid 设置，handle_fetch/search 读取）
    bool is_uid_command = false;
    std::string uid_overridden_args;          // UID→seq 转换后的命令参数

    // APPEND 文字量等待
    bool awaiting_literal = false;            // 等待 APPEND 文字量数据
    size_t literal_size = 0;                  // 期望的文字量字节数
    std::string literal_buffer;               // 已接收的文字量数据
    std::string pending_append_mailbox;       // APPEND 的目标邮箱
    std::string pending_append_flags;         // APPEND 的 flags
    std::string pending_append_internaldate;  // APPEND 的 InternalDate
    std::string pending_append_preamble;      // APPEND 原始参数（literal 前的部分）

    void clear() {
        is_authenticated = false;
        username.clear();
        user_id = 0;
        shard_index = 0;
        current_tag.clear();
        mailbox_selected = false;
        selected_mailbox_name.clear();
        selected_mailbox_id = 0;
        uid_validity = 0;
        read_only = false;
        idle_mode = false;
        awaiting_literal = false;
        literal_size = 0;
        literal_buffer.clear();
        pending_append_mailbox.clear();
        pending_append_flags.clear();
        pending_append_internaldate.clear();
        pending_append_preamble.clear();
    }
};

// 前向声明
template <typename ConnectionType>
class SessionBase;

// 状态处理函数类型定义（同 SMTP 风格）
template <typename ConnectionType>
using ImapStateHandler = std::function<void(std::shared_ptr<SessionBase<ConnectionType>>)>;

// ====================================================================
// IMAP 状态机基类 —— 封装 DB 操作和公用方法
// ====================================================================
template <typename ConnectionType>
class ImapsFsm {
public:
    using MailboxStatsCache = LruCache<std::string, MailboxCacheEntry>;
    std::shared_ptr<AuthCache> m_authCache = std::make_shared<AuthCache>();
protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<MailboxStatsCache> m_mailboxStatsCache;


public:
    ImapsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
             std::shared_ptr<ThreadPoolBase> worker_thread_pool,
             std::shared_ptr<router::IShardRouter> shard_router)
        : m_ioThreadPool(io_thread_pool),
          m_workerThreadPool(worker_thread_pool),
          m_shardRouter(std::move(shard_router)) {}

    virtual ~ImapsFsm() = default;

    void set_mailbox_stats_cache(std::shared_ptr<MailboxStatsCache> cache) { m_mailboxStatsCache = cache; }
    std::shared_ptr<MailboxStatsCache> get_mailbox_stats_cache() const { return m_mailboxStatsCache; }

    ScopedConnection acquire_connection(int shard) {
        auto pool = m_shardRouter->get_db_pool(static_cast<size_t>(shard));
        return pool->acquire_connection();
    }

    std::shared_ptr<storage::IStorageProvider> get_storage(int shard) {
        return m_shardRouter->get_storage(static_cast<size_t>(shard));
    }

    // 处理事件 —— 纯虚接口
    virtual void process_event(std::shared_ptr<SessionBase<ConnectionType>> session,
                                ImapEvent event,
                                const std::string& tag) = 0;

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
    // out_shard 返回该用户所在的 shard 索引，供 session 存储
    bool auth_user(SessionBase<ConnectionType>* session,
                   const std::string& mail_address,
                   const std::string& password,
                   uint64_t& out_user_id,
                   int& out_shard) {
        LOG_AUTH_INFO("IMAP AUTH attempt: mail_address=[{}]", mail_address);

        if (!session) {
            LOG_AUTH_ERROR("Session is null in auth_user");
            return false;
        }

        // 通过 shard router 确定用户所在 shard
        int shard = 0;
        if (m_shardRouter) {
            int r = m_shardRouter->route(mail_address);
            if (r >= 0) shard = r;
        }
        out_shard = shard;

        // 查缓存
        AuthCacheEntry ce;
        if (m_authCache->lookup(mail_address, ce)) {
            if (ce.status != 1) return false;
            out_shard = ce.shard;
            out_user_id = ce.user_id;
            if (ce.password_hash.size() >= 2 && ce.password_hash[0] == '$' && ce.password_hash[1] == '2')
                return bcrypt_verify(password, ce.password_hash);
            return ce.password_hash == password;
        }

        auto conn = acquire_connection(shard);
        if (!conn.is_valid()) {
            LOG_AUTH_ERROR("Failed to get database connection for shard {}", shard);
            return false;
        }

        std::string sql = db::sql::build_auth_user_query();
        auto result = conn->query(sql, {mail_address});
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
        m_authCache->store(mail_address, {stored, status, user_id, shard});

        bool ok = false;
        if (stored.size() >= 2 && stored[0] == '$' && stored[1] == '2') {
            ok = bcrypt_verify(password, stored);
        } else {
            ok = (stored == password);
            if (ok) {
                LOG_AUTH_WARN("User {} still using plaintext password", mail_address);
            }
        }

        if (ok) {
            out_user_id = user_id;
            conn->execute(db::sql::build_update_last_login(), {mail_address});
        }
        return ok;
    }

    // 获取用户的邮箱列表
    bool get_mailboxes(uint64_t user_id,
                       std::vector<std::tuple<uint64_t, std::string, int>>& mailboxes) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            LOG_DATABASE_ERROR("Failed to get DB connection in get_mailboxes");
            return false;
        }

        auto result = conn->query(
            db::sql::build_imap_list_mailboxes(),
            {std::to_string(user_id)});
        if (!result) {
            return false;
        }

        for (size_t i = 0; i < result->get_row_count(); ++i) {
            uint64_t id = safe_stoull(result->get_value(i, "id"));
            std::string name = result->get_value(i, "name");
            int box_type = static_cast<int>(safe_stoull(result->get_value(i, "box_type")));
            mailboxes.emplace_back(id, name, box_type);
        }
        return true;
    }

    // 根据邮箱名称查找邮箱 ID
    uint64_t find_mailbox_id(uint64_t user_id, const std::string& mailbox_name) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return 0;
        }

        // 先把 IMAP-UTF-7 编码的名称解码为 UTF-8（客户端发来的可能是编码后的）
        std::string name_utf8 = mailbox_name;
        if (mailbox_name.find('&') != std::string::npos) {
            std::string decoded = decode_imap_utf7(mailbox_name);
            if (!decoded.empty()) name_utf8 = decoded;
        }

        // Try direct name match first (handles Chinese names like "收件箱")
        auto result = conn->query(
            db::sql::build_imap_get_mailbox_by_name(),
            {std::to_string(user_id), name_utf8});
        if (result && result->get_row_count() > 0) {
            return safe_stoull(result->get_value(0, "id"));
        }

        // IMAP: "INBOX" → find the inbox mailbox (box_type=1)
        std::string upper = name_utf8;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        if (upper == "INBOX") {
            result = conn->query(
                db::sql::build_imap_get_inbox_id(),
                {std::to_string(user_id)});
            if (result && result->get_row_count() > 0) {
                return safe_stoull(result->get_value(0, "id"));
            }
        }

        return 0;
    }

    // 获取邮箱中的邮件列表（用于 FETCH / SEARCH）
    struct MailboxMailInfo {
        uint64_t mail_id;
        std::string sender;
        std::string recipient;
        std::string subject;
        std::string body_path;
        bool is_starred;
        bool is_deleted;
        bool is_important;
        int status;        // 0=read, 1=unread
        time_t send_time;
    };

    bool get_mailbox_mails(uint64_t mailbox_id, uint64_t user_id,
                           std::vector<MailboxMailInfo>& mails) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return false;
        }

        std::string sql = db::sql::build_imap_get_mailbox_mails();

        auto result = conn->query(sql, {
            std::to_string(mailbox_id),
            std::to_string(user_id)
        });

        if (!result) {
            return false;
        }

        for (size_t i = 0; i < result->get_row_count(); ++i) {
            MailboxMailInfo info;
            info.mail_id = safe_stoull(result->get_value(i, "id"));
            info.sender = result->get_value(i, "sender");
            info.recipient = result->get_value(i, "recipient");
            info.subject = result->get_value(i, "subject");
            info.body_path = result->get_value(i, "body_path");
            info.is_starred = result->get_value(i, "is_starred") == "1";
            info.is_deleted = result->get_value(i, "is_deleted") == "1";
            info.is_important = result->get_value(i, "is_important") == "1";
            info.status = result->get_value(i, "status").empty() ? 0 : static_cast<int>(safe_stoull(result->get_value(i, "status")));
            info.send_time = static_cast<time_t>(safe_stoull(result->get_value(i, "send_time")));
            mails.push_back(std::move(info));
        }
        return true;
    }

    // 获取单封邮件信息
    bool get_mail_info(uint64_t mail_id,
                       MailboxMailInfo& info) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return false;
        }

        auto result = conn->query(
            "SELECT id, subject, body_path, UNIX_TIMESTAMP(send_time) AS send_time FROM mails WHERE id = ?",
            {std::to_string(mail_id)});

        if (!result || result->get_row_count() == 0) {
            return false;
        }

        info.mail_id = safe_stoull(result->get_value(0, "id"));
        info.subject = result->get_value(0, "subject");
        info.body_path = result->get_value(0, "body_path");
        info.send_time = static_cast<time_t>(safe_stoull(result->get_value(0, "send_time")));
        return true;
    }

    // 获取发件人
    std::string get_mail_sender(uint64_t mail_id) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return "";
        }
        auto result = conn->query(
            "SELECT sender FROM mail_recipients WHERE mail_id = ? LIMIT 1",
            {std::to_string(mail_id)});
        if (result && result->get_row_count() > 0) {
            return result->get_value(0, "sender");
        }
        return "";
    }

    // 获取收件人列表
    std::vector<std::string> get_mail_recipients(uint64_t mail_id) {
        std::vector<std::string> recipients;
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return recipients;
        }
        auto result = conn->query(
            "SELECT recipient FROM mail_recipients WHERE mail_id = ?",
            {std::to_string(mail_id)});
        if (result) {
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                recipients.push_back(result->get_value(i, "recipient"));
            }
        }
        return recipients;
    }

    // 获取用户邮箱地址
    std::string get_user_email(uint64_t user_id) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return "";
        }
        auto result = conn->query(
            "SELECT mail_address FROM users WHERE id = ?",
            {std::to_string(user_id)});
        if (result && result->get_row_count() > 0) {
            return result->get_value(0, "mail_address");
        }
        return "";
    }

    // 更新邮件已读/未读状态
    bool update_mail_seen(uint64_t mail_id, const std::string& recipient, bool seen) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return false;
        }
        int new_status = seen ? 0 : 1; // 0=read, 1=unread
        return conn->execute(
            "UPDATE mail_recipients SET status = ? WHERE mail_id = ? AND recipient = ?",
            {std::to_string(new_status), std::to_string(mail_id), recipient});
    }

    // 更新已删除标记
    bool update_mail_deleted(uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id, bool deleted) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return false;
        }
        return conn->execute(
            db::sql::build_imap_update_mail_flag_deleted(),
            {deleted ? "1" : "0", std::to_string(mail_id), std::to_string(user_id), std::to_string(mailbox_id)});
    }

    // 更新标星标记
    bool update_mail_flagged(uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id, bool flagged) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return false;
        }
        return conn->execute(
            db::sql::build_imap_update_mail_flag_starred(),
            {flagged ? "1" : "0", std::to_string(mail_id), std::to_string(user_id), std::to_string(mailbox_id)});
    }

    // ========== 邮件持久化 ==========

    // 创建一封邮件记录：生成 mail_id + 保存正文 + 写入 mails 表
    // 返回 mail_id（0 表示失败），body_path 为存储路径供后续 DB 操作使用
    uint64_t create_mail(const std::string& subject, const std::string& body_content,
                         std::string& out_body_path, std::string& error) {
        int64_t mail_id = algorithm::get_snowflake_generator().next_id();
        out_body_path.clear();

        // 保存正文
        std::string storage_key = get_storage(0)
            ? get_storage(0)->build_mail_body_key(static_cast<uint64_t>(mail_id))
            : "";
        if (!storage_key.empty()) {
            std::string err;
            if (!get_storage(0)->append_binary(storage_key, body_content.data(),
                                                  body_content.size(), err)) {
                error = "Storage error: " + err;
                return 0;
            }
            out_body_path = storage_key;
        } else {
            std::string base = "mail/";
            std::string fp = base + std::to_string(mail_id);
            std::ofstream out(fp, std::ios::binary);
            if (!out) { error = "Cannot write " + fp; return 0; }
            out.write(body_content.data(), static_cast<std::streamsize>(body_content.size()));
            if (!out) { error = "Write failed " + fp; return 0; }
            out_body_path = fp;
        }

        // 插入 mails 表
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) { error = "DB connection failed"; return 0; }
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (!conn->execute(
                "INSERT INTO mails (id, subject, body_path, send_time) VALUES (?, ?, ?, ?)",
                {std::to_string(mail_id),
                 subject.empty() ? "(无主题)" : subject,
                 out_body_path, std::to_string(ts)})) {
            error = "Insert mail record failed";
            return 0;
        }
        return static_cast<uint64_t>(mail_id);
    }

    // 添加邮件到邮箱（mail_recipients + mail_mailbox）
    bool link_mail_to_mailbox(uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id,
                              const std::string& sender, const std::string& recipient,
                              int status) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) return false;

        int64_t rid = algorithm::get_snowflake_generator().next_id();
        bool ok = conn->execute(
            "INSERT INTO mail_recipients (id, mail_id, sender, recipient, status) "
            "VALUES (?, ?, ?, ?, ?)",
            {std::to_string(rid), std::to_string(mail_id),
             sender, recipient, std::to_string(status)});
        if (!ok) return false;

        return conn->execute(
            "INSERT INTO mail_mailbox (id, mail_id, mailbox_id, user_id, is_starred, "
            "is_important, is_deleted, add_time) VALUES (?, ?, ?, ?, 0, 0, 0, NOW())",
            {std::to_string(algorithm::get_snowflake_generator().next_id()),
             std::to_string(mail_id), std::to_string(mailbox_id),
             std::to_string(user_id)});
    }

    // ========== IMAP-UTF-7 解码（RFC 3501 §5.1.3）==========
    // 将客户端发来的 IMAP-UTF-7 邮箱名转为 UTF-8，用于 DB 查询
    static std::string decode_imap_utf7(const std::string& imap7) {
        // modified Base64 反向表
        static const int rev[128] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        };

        std::string result;
        size_t i = 0;
        while (i < imap7.size()) {
            char c = imap7[i];
            if (c == '&') {
                if (i + 1 < imap7.size() && imap7[i + 1] == '-') {
                    result += '&';
                    i += 2;
                } else {
                    size_t end = imap7.find('-', i + 1);
                    if (end == std::string::npos) { result += c; i++; continue; }
                    std::string b64 = imap7.substr(i + 1, end - i - 1);
                    i = end + 1;

                    std::vector<uint8_t> bytes;
                    uint32_t acc = 0;
                    int bits = 0;
                    for (char bc : b64) {
                        if (bc < 0 || bc >= 128) continue;
                        int v = rev[static_cast<int>(bc)];
                        if (v < 0) continue;
                        acc = (acc << 6) | static_cast<uint32_t>(v);
                        bits += 6;
                        if (bits >= 8) {
                            bits -= 8;
                            bytes.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
                        }
                    }

                    for (size_t j = 0; j + 1 < bytes.size(); j += 2) {
                        uint16_t unit = (static_cast<uint16_t>(bytes[j]) << 8) | bytes[j + 1];
                        if (unit >= 0xD800 && unit <= 0xDBFF && j + 3 < bytes.size()) {
                            uint16_t low = (static_cast<uint16_t>(bytes[j + 2]) << 8) | bytes[j + 3];
                            uint32_t cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                            result += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                            j += 2;
                        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                            continue;
                        } else if (unit < 0x80) {
                            result += static_cast<char>(unit);
                        } else if (unit < 0x800) {
                            result += static_cast<char>(0xC0 | ((unit >> 6) & 0x1F));
                            result += static_cast<char>(0x80 | (unit & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | ((unit >> 12) & 0x0F));
                            result += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (unit & 0x3F));
                        }
                    }
                }
            } else {
                result += c;
                i++;
            }
        }
        return result;
    }

    // 从存储读取邮件内容
    // 邮件内容存储在本地文件系统（body_path 是绝对/相对路径）
    std::string read_mail_body(const std::string& body_path) {
        if (body_path.empty()) {
            return "";
        }

        std::ifstream in(body_path, std::ios::binary);
        if (!in.is_open()) {
            LOG_FILE_IO_ERROR("Failed to open mail body: {}", body_path);
            return "";
        }
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        return content;
    }

    // ========== 缓存感知的邮箱统计 ==========

    // 获取邮箱摘要（优先缓存，stale-while-revalidate）
    // from_cache_out: true 表示返回值来自缓存
    // stale_out: true 表示缓存 TTL 已过（值仍可用，建议异步刷新）
    MailboxCacheEntry get_mailbox_stats_cached(
        uint64_t user_id, uint64_t mailbox_id,
        bool& from_cache_out, bool& stale_out) {

        from_cache_out = false;
        stale_out = false;

        // 尝试缓存（仅新鲜命中时返回；过期则不返回避免客户端看到旧数据）
        if (m_mailboxStatsCache) {
            std::string key = mbox_cache_key(user_id, mailbox_id);
            MailboxCacheEntry cached;
            if (m_mailboxStatsCache->get(key, cached, stale_out)) {
                from_cache_out = true;
                if (!stale_out) {
                    return cached;  // 新鲜命中
                }
                // stale=true：缓存过时，不回源，直接查 DB 更新缓存
            }
        }

        // 回源 DB
        MailboxCacheEntry entry;
        entry.exists = get_mailbox_count(mailbox_id, user_id);
        entry.unseen = get_mailbox_unseen_count(mailbox_id, user_id);
        entry.uidnext = get_mailbox_uidnext(mailbox_id, user_id);
        entry.uidvalidity = mailbox_id;

        // 写入缓存
        if (m_mailboxStatsCache) {
            m_mailboxStatsCache->put(mbox_cache_key(user_id, mailbox_id), entry);
        }

        return entry;
    }

    // 获取邮箱的总邮件数
    size_t get_mailbox_count(uint64_t mailbox_id, uint64_t user_id) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return 0;
        }
        auto result = conn->query(
            db::sql::build_imap_select_status_total(),
            {std::to_string(mailbox_id), std::to_string(user_id)});
        if (result && result->get_row_count() > 0) {
            return static_cast<size_t>(safe_stoull(result->get_value(0, "cnt")));
        }
        return 0;
    }

    // 获取邮箱的未读邮件数
    size_t get_mailbox_unseen_count(uint64_t mailbox_id, uint64_t user_id) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return 0;
        }
        // 通过 mail_recipients.status=1 (unread) 统计
        auto result = conn->query(
            db::sql::build_imap_mailbox_unseen_count(),
            {std::to_string(user_id), std::to_string(mailbox_id), std::to_string(user_id)});
        if (result && result->get_row_count() > 0) {
            return static_cast<size_t>(safe_stoull(result->get_value(0, "cnt")));
        }
        return 0;
    }

    // 获取邮箱的最近邮件 UID（这里直接用 mail_id 充当 UID）
    uint64_t get_mailbox_uidnext(uint64_t mailbox_id, uint64_t user_id) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return 1;
        }
        auto result = conn->query(
            db::sql::build_imap_mailbox_uidnext(),
            {std::to_string(mailbox_id), std::to_string(user_id)});
        if (result && result->get_row_count() > 0) {
            return safe_stoull(result->get_value(0, "uidnext"));
        }
        return 1;
    }

    // EXPUNGE：真正从邮箱删除打了 \Deleted 标记的邮件
    void expunge_mailbox(uint64_t mailbox_id, uint64_t user_id) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return;
        }
        // 删除 mail_mailbox 中 is_deleted=1 且在此邮箱的记录
        conn->execute(
            db::sql::build_imap_expunge_delete_mailbox(),
            {std::to_string(mailbox_id), std::to_string(user_id)});
    }

    // 获取 EXPUNGE 序列（返回被删邮件的 mail_id 列表）
    std::vector<uint64_t> get_expunged_ids(uint64_t mailbox_id, uint64_t user_id) {
        std::vector<uint64_t> ids;
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            return ids;
        }
        auto result = conn->query(
            db::sql::build_imap_expunge_select_ids(),
            {std::to_string(mailbox_id), std::to_string(user_id)});
        if (result) {
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                ids.push_back(safe_stoull(result->get_value(i, "mail_id")));
            }
        }
        return ids;
    }
};

} // namespace mail_system

#endif // IMAPS_FSM_H
