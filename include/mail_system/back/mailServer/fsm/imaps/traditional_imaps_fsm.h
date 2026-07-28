#ifndef TRADITIONAL_IMAPS_FSM_H
#define TRADITIONAL_IMAPS_FSM_H

#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/mailServer/fsm/fsm_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imaps_fsm.hpp"
#include "mail_system/back/common/logger.h"
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
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <iomanip>

namespace mail_system {

template <typename ConnectionType>
class TraditionalImapsFsm : public ImapsFsm<ConnectionType>, public FsmBase<ConnectionType, ImapState, ImapEvent> {
public:
    TraditionalImapsFsm(
        std::shared_ptr<ThreadPoolBase> io_thread_pool,
        std::shared_ptr<ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<router::IShardRouter> shard_router
    ) : ImapsFsm<ConnectionType>(io_thread_pool, worker_thread_pool, std::move(shard_router)) {
        init_transition_table();
        init_state_handlers();
    }

    ~TraditionalImapsFsm() override = default;

    // 事件处理入口
    void process_event(
        std::shared_ptr<SessionBase<ConnectionType>> session,
        ImapEvent event,
        const std::string& tag,
        const std::string& args
    ) override;

    // 自动处理（从 session 获取当前 pending 事件）
    void auto_process_event(std::shared_ptr<SessionBase<ConnectionType>> session);

private:
    // 初始化
    void init_transition_table();
    void init_state_handlers();

    // FsmBase hooks
    using BaseFsm = FsmBase<ConnectionType, ImapState, ImapEvent>;
    using Handler = typename BaseFsm::Handler;
    bool is_terminal_state(ImapState s) const override;
    void on_invalid_transition(ImapState s, ImapEvent e,
        std::shared_ptr<SessionBase<ConnectionType>> session,
        const std::string& args) override;
    void on_handler_not_found(ImapState s, ImapEvent e,
        std::shared_ptr<SessionBase<ConnectionType>> session,
        const std::string& args) override;
    void invoke_handler(Handler& h,
        std::shared_ptr<SessionBase<ConnectionType>> session,
        const std::string& args) override;

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
    static std::string build_bodystructure(const std::string& raw);

public:
    // 通用的 IMAP 响应写回（session 需要访问）
    void send_untagged(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& data);
    void send_tagged(std::shared_ptr<SessionBase<ConnectionType>> session,
                     const std::string& tag,
                     const std::string& status,
                     const std::string& message);
    void send_continuation(std::shared_ptr<SessionBase<ConnectionType>> session,
                           const std::string& message);

private:
    // ========== 状态处理器 ==========
    void handle_init_connect(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_capability(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_login(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_authenticate(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_logout(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_select(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_examine(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_list(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_status(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_fetch(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args,
                      bool is_uid = false);
    void handle_store(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_expunge(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_close(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_noop(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_check(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_create(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_delete(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_rename(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_subscribe(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_unsubscribe(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_lsub(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_append(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_search(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args,
                       bool is_uid = false);
    void handle_uid(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_starttls(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_copy(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_move(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_copy_move(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args, bool is_move);
    void handle_idle(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_done(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_error(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);
    void handle_timeout(std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& args);

    // 辅助函数
    static bool parse_seq_set(const std::string& seq_set, uint64_t& start, uint64_t& end, size_t total);
};

} // namespace mail_system

#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"

#endif // TRADITIONAL_IMAPS_FSM_H
