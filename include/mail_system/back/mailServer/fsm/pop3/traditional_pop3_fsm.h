#ifndef TRADITIONAL_POP3_FSM_H
#define TRADITIONAL_POP3_FSM_H

#include "framework/session_base.h"
#include "framework/fsm_base.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/mailServer/fsm/pop3/pop3_types.hpp"
#include <chrono>
#include <memory>
#include <string>

namespace mail_system {

template <typename ConnectionType>
class TraditionalPop3Fsm : public FsmBase<ConnectionType, Pop3State, Pop3Event> {
public:
    std::shared_ptr<AuthCache> m_authCache = std::make_shared<AuthCache>();
    // 锁心跳续约周期（默认 60s）。测试可改短以在有限时间内观察续约/失锁。
    std::chrono::milliseconds heartbeat_interval_{std::chrono::seconds(60)};

protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    // 每会话上下文。Pop3Context 是 per-session 状态，由 session 拥有；
    // FSM 通过 session->get_context() 获取裸指针。
    // 这里不存，避免所有权歧义。

public:
    TraditionalPop3Fsm(
        std::shared_ptr<ThreadPoolBase> io_thread_pool,
        std::shared_ptr<ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<router::IShardRouter> shard_router
    ) : m_ioThreadPool(io_thread_pool),
        m_workerThreadPool(worker_thread_pool),
        m_shardRouter(std::move(shard_router)) {
        init_transition_table();
        init_state_handlers();
    }

    ~TraditionalPop3Fsm() override = default;

    // ========== 事件派发入口（session 调） ==========
    void process_event(
        std::shared_ptr<SessionBase<ConnectionType>> session,
        Pop3Event event);

    // 从 session 的 next_event_ 取事件后派发（仿 SMTP auto_process_event）
    void auto_process_event(
        std::shared_ptr<SessionBase<ConnectionType>> session);

    // 静态：USER/PASS 走 worker 线程做 bcrypt + DB 查
    // 仿 IMAP auth_user (imaps_fsm.tpp:2547-2619)
    static bool auth_user(const std::shared_ptr<router::IShardRouter>& shard_router,
                          const std::shared_ptr<AuthCache>& auth_cache,
                          const std::string& mail_address,
                          const std::string& password,
                          uint64_t& out_user_id,
                          int& out_shard);

    // 取 INBOX id（box_type=1）。失败返回 0。
    static uint64_t get_inbox_id(class IDBConnection* conn, uint64_t user_id);

    // 取 INBOX 全部未删邮件的 (mail_id, body_path) 列表。
    // 排除 mm.is_deleted = 1（POP3 视角是"已 DELE 但未 QUIT"的仍可见，
    // 真正的"已 DELE 后 QUIT 提交"由 DELE 走 UPDATE 时直接 SQL 删除）。
    static bool get_inbox_mails(class IDBConnection* conn, uint64_t mailbox_id, uint64_t user_id,
                                std::vector<Pop3Message>& out);

    // 取 shard 的 storage provider（仿 imaps_fsm.tpp get_storage）
    std::shared_ptr<storage::IStorageProvider> get_storage(int shard) {
        return m_shardRouter ? m_shardRouter->get_storage(static_cast<size_t>(shard)) : nullptr;
    }

    // 申请 mailbox 锁。true = 成功；false = 已被他人持有。
    static bool acquire_lock(class IDBConnection* conn,
                             uint64_t user_id, const std::string& session_id);

    // 释放 mailbox 锁。幂等。
    static void release_lock(class IDBConnection* conn,
                             uint64_t user_id, const std::string& session_id);

    // 续约锁心跳（v2）：条件 UPDATE last_heartbeat + verify 所有权。
    // 返回 false = 锁已被回收/转交（行不存在），调用方应关闭会话。
    static bool renew_lock_heartbeat(const std::shared_ptr<router::IShardRouter>& router,
                                     uint64_t user_id, const std::string& session_id, int shard);

    // 清扫过期锁（v2 sweeper 周期调用）：删除心跳超过 5min 的锁。
    static bool sweep_expired_locks(const std::shared_ptr<router::IShardRouter>& router);

    // 提交 DELE 标记：把 deleted 里所有 mail_id 对应行 is_deleted=1
    // 然后 expunge（DELETE FROM mail_mailbox）。
    static bool apply_deletions(class IDBConnection* conn,
                                uint64_t user_id, uint64_t mailbox_id,
                                const std::set<uint64_t>& deleted_mail_ids);

    // 写一行（带 CRLF）到 session，跨线程安全（通过 session->do_async_write）。
    // 协议规范：`+OK text\r\n` / `-ERR text\r\n`。
    static void send_line(std::shared_ptr<SessionBase<ConnectionType>> session,
                          const std::string& line);

    // 取 session 的 Pop3Context 裸指针（每个 session 自带 context_ 成员，
    // 通过 get_context() 拿到 SessionBase 的协议层 context —— 但 SessionBase
    // 接口是 void*，本 FSM 强转为 Pop3Context*）。
    static Pop3Context* ctx_of(std::shared_ptr<SessionBase<ConnectionType>> session);

    // ========== 状态/事件名（调试 / 日志） ==========
    static std::string get_state_name(Pop3State s);
    static std::string get_event_name(Pop3Event e);

private:
    void init_transition_table();
    void init_state_handlers();

    // FsmBase hooks
    using BaseFsm = FsmBase<ConnectionType, Pop3State, Pop3Event>;
    using Handler = typename BaseFsm::Handler;
    bool is_terminal_state(Pop3State s) const override;
    void on_invalid_transition(Pop3State s, Pop3Event e,
        std::shared_ptr<SessionBase<ConnectionType>> session) override;
    void on_handler_not_found(Pop3State s, Pop3Event e,
        std::shared_ptr<SessionBase<ConnectionType>> session) override;

    // ========== 锁心跳（v2） ==========
    // 在 io_context 上启动递归续约定时器；handler 只捕获 weak 引用打破自环，
    // 会话 close() 时取消。静态：不依赖 FSM 实例（router/worker/interval 显式传入）。
    static void start_heartbeat(std::shared_ptr<SessionBase<ConnectionType>> session,
                                std::shared_ptr<router::IShardRouter> router,
                                std::shared_ptr<ThreadPoolBase> worker,
                                std::chrono::milliseconds interval);

    // ========== 状态处理器 ==========
    void handle_init_connect(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_capa(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_user(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_pass(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_stat(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_list(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_uidl(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_retr(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_dele(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_noop(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_rset(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_quit(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_error(std::shared_ptr<SessionBase<ConnectionType>> session);
    void handle_timeout(std::shared_ptr<SessionBase<ConnectionType>> session);
};

} // namespace mail_system

#endif // TRADITIONAL_POP3_FSM_H
