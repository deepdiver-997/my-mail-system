#ifndef TRADITIONAL_SMTPS_FSM_H
#define TRADITIONAL_SMTPS_FSM_H

#include "framework/session_base.h"
#include "framework/fsm_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/sql_queries.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_types.hpp"
#include "mail_system/back/router/i_shard_router.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mail_system {

template <typename ConnectionType>
class SmtpsSession;  // 前向声明

template <typename ConnectionType>
class TraditionalSmtpsFsm : public FsmBase<ConnectionType, SmtpsState, SmtpsEvent> {
protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<IMailboxCache> m_mailboxCache;
    std::shared_ptr<mail_system::persist_storage::PersistentQueue> m_persistentQueue;

public:
    std::shared_ptr<AuthCache> m_authCache = std::make_shared<AuthCache>();
    // RCPT TO 校验缓存（独立于认证缓存，缓存用户存在性 status：1=存在 0=不存在）
    std::shared_ptr<AuthCache> m_recipientCache = std::make_shared<AuthCache>();

    TraditionalSmtpsFsm(
        std::shared_ptr<ThreadPoolBase> io_thread_pool,
        std::shared_ptr<ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<persist_storage::PersistentQueue> persistent_queue,
        std::shared_ptr<router::IShardRouter> shard_router
    ) : m_ioThreadPool(io_thread_pool),
        m_workerThreadPool(worker_thread_pool),
        m_shardRouter(std::move(shard_router)),
        m_persistentQueue(persistent_queue) {
        init_transition_table();
        init_state_handlers();
    }

    ~TraditionalSmtpsFsm() override = default;

    void set_mailbox_cache(std::shared_ptr<IMailboxCache> cache) { m_mailboxCache = cache; }
    std::shared_ptr<IMailboxCache> get_mailbox_cache() const { return m_mailboxCache; }

    ScopedConnection acquire_connection(int shard) {
        auto pool = m_shardRouter->get_db_pool(static_cast<size_t>(shard));
        return pool->acquire_connection();
    }

    // 处理事件
    void process_event(std::shared_ptr<SessionBase<ConnectionType>> session, SmtpsEvent event);
    void auto_process_event(std::shared_ptr<SessionBase<ConnectionType>> session);

    // 获取状态名称
    static std::string get_state_name(SmtpsState state);

    // 获取事件名称
    static std::string get_event_name(SmtpsEvent event);

    using AuthCallback = std::function<void(bool ok, int shard)>;
    // session 用 shared_ptr 保活：异步 DB 回调（真异步 worker 线程）期间 session 不会提前析构
    void auth_user_async(std::shared_ptr<SessionBase<ConnectionType>> session,
                         const std::string& mail_address,
                         const std::string& password, AuthCallback cb);

    // RCPT TO 校验：异步检查本地收件人是否存在（回调 exists=true 表示存在且 status=1）
    void user_exists_async(std::shared_ptr<SessionBase<ConnectionType>> session,
                           const std::string& mail_address,
                           std::function<void(bool exists)> cb);

    // 保存邮件元数据到数据库（异步），返回future用于跟踪操作结果
    std::future<bool> save_mail_metadata_async(mail* data, const std::string& file_path_prefix);

    // 保存附件元数据到数据库（异步），返回 future
    std::future<bool> save_attachment_metadata_async(const attachment& att, size_t mail_id);

    // 根据文件路径删除邮件元数据 假定数据库操作一定成功
    void remove_metadata_by_file_path(const std::vector<std::string>& file_paths);

    // 保存邮件正文到本地文件
    bool save_mail_body_to_file(mail* data, const std::string& file_path);

    bool save_attachment_to_file(attachment& att, const std::string& file_path);

    // 检查异步操作结果，失败则删除对应文件
    void cleanup_failed_saves(std::vector<std::future<bool>>& futures, const std::vector<std::string>& file_paths);

private:
    static void cleanup_streamed_attachments(SmtpsContext* ctx);
    static void cleanup_mail_files(mail* mail);

    bool persist_mails_sync(SmtpsSession<ConnectionType>* session, std::string& error);
    bool persist_and_reply(std::shared_ptr<SessionBase<ConnectionType>> session);

    void init_transition_table();
    void init_state_handlers();

    bool is_terminal_state(SmtpsState s) const override;
    void on_invalid_transition(SmtpsState s, SmtpsEvent e,
        std::shared_ptr<SessionBase<ConnectionType>> session) override;

    void handle_init_connect(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_greeting_ehlo(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_auth_starttls(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_auth_auth(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_auth_auth_login(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_auth_username(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_auth_password(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_auth_mail_from(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_mail_from_mail_from(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_rcpt_to_rcpt_to(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_data_data(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_in_message_data(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_in_message_data_end(std::shared_ptr<SessionBase<ConnectionType>> session);

    // DATA_END 在 commit_body_async 回调（本地内联 / 远程 provider 线程）之后的
    // 收尾：MIME 预解析 → 入站校验 → 入队 + 250/451。刻意 static：
    // 回调里无 this 可用，全部依赖经 session 参数推导。
    static void finish_data_end_after_commit(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_wait_quit_quit(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_timeout(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_error(std::shared_ptr<SessionBase<ConnectionType>> session);
};

} // namespace mail_system


#endif // TRADITIONAL_SMTPS_FSM_H
