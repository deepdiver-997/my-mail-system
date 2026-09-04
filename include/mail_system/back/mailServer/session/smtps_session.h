#ifndef SMTPS_SESSION_H
#define SMTPS_SESSION_H

#include "mail_system/back/common/logger.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "framework/session_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_types.hpp"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/algorithm/line_folder.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/storage/mail_body_writer.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace mail_system {

template <typename ConnectionType>
class SmtpsSession : public SessionBase<ConnectionType> {
    friend class TraditionalSmtpsFsm<ConnectionType>;
    static constexpr size_t MAX_BODY_BYTES = 10 * 1024 * 1024;

public:
    SmtpsSession(
        ServerBase* server,
        std::unique_ptr<ConnectionType> connection,
        std::shared_ptr<TraditionalSmtpsFsm<ConnectionType>> fsm);

    ~SmtpsSession() {
        this->session_authenticated_ = context_.is_authenticated;
    }

    void set_listener_config(const ListenerConfig& lc) { context_.listener_config = lc; }

    static void start(std::shared_ptr<SmtpsSession> self);
    static void start_after_starttls(std::shared_ptr<SmtpsSession> self);

    void handle_read(const std::string& data) override;
    void process_read() override;
    bool has_buffered_input() const override;
    std::string extract_one_line() override;
    std::chrono::milliseconds compute_reply_delay() const override;
    void* get_fsm() const override;
    void* get_context() override;

    void set_current_state(int state) override;
    void set_next_event(int event) override;
    int get_current_state() const override;
    int get_next_event() const override;
    std::string get_last_command_args() const override;

    // ── Mail 实体管理（已从 SessionBase 下放到此）──────────────
    mail* get_mail() { return mail_.get(); }
    std::unique_ptr<mail> get_mail_ptr() { return std::move(mail_); }

    void create_mail_on_data_command();
    persist_storage::SubmitOwnedMailResult submit_mail_to_queue();
    bool check_mail_persist_status();

    // 冲刷剩余缓冲并持久化正文（含 fsync）。返回 false 表示正文未能落盘，
    // 调用方必须回 4xx 而不是 250 —— 否则就是骗发送方 MTA 把邮件从队列里删掉。
    bool commit_body();

    // 异步形状的 commit_body：回调携带 (是否落盘成功, 错误信息)。
    // 本地后端内联执行（与同步版行为一致）；远程后端真异步时回调来自
    // provider 线程 —— 调用方必须已 set_paused(true) 取得 session 独占
    // （SPF/DNS 回调同一约定），在回调里恢复流水线。
    // session 通过 shared_ptr 捕获自持，回调期间对象存活。
    void commit_body_async(std::function<void(bool ok, const storage::IoError& error)> cb);

    void reset_mail_state();
    void discard_current_mail();
    bool has_pending_mail_submission() const;
    persist_storage::PersistStatus get_pending_mail_persist_status() const;
    void cancel_pending_mail_submission();
    void clear_pending_mail_submission();

    void process_message_data(const std::string& data);
    void finalize_attachment_from_context();
    void parse_smtp_command(const std::string& data);

private:
    // 处理写入失败的情况
    // 根据邮件的持久化状态标记为 CANCELLED 或提交删除任务
    void handle_write_failure();

    // 将 DATA 阶段收到的正文数据交给 body_writer_
    // 缓冲、刷盘与偏移推进全部由 MailBodyWriter 负责
    void append_body_data(const char* data, size_t size);

    // 清理邮件相关的所有文件（邮件体和附件）
    // 在邮件持久化失败或需要删除时调用
    void cleanup_mail_files(mail* mail_ptr);

    std::shared_ptr<TraditionalSmtpsFsm<ConnectionType>> fsm_;
    // state_ 跨线程访问：io 线程在 has_buffered_input() 中读取，异步回调线程在
    // set_current_state() 中写入（DNS/DB 回调直接续跑 FSM）。用 atomic 消除竞争。
    std::atomic<int> state_{static_cast<int>(SmtpsState::INIT)};
    SmtpsEvent next_event_;
    bool ignore_current_command_;
    SmtpsContext context_;
    std::string last_command_args_;

    // 正文写入器。DATA 命令时创建，reset_mail_state() 时销毁；
    // 未 commit 就销毁会自动 abort 并删掉半成品文件。
    std::unique_ptr<storage::MailBodyWriter> body_writer_;

    // 正文落盘前的超长行折叠器（跨 TCP 块携带半行，DATA 结束时 flush）。
    // reset_mail_state() 时一并 reset，与 body_writer_ 生命周期对齐。
    algorithm::LineFolder body_line_folder_;

    std::shared_ptr<persist_storage::PersistentQueue> persistent_queue_;
    persist_storage::PersistSubmissionTicket pending_submission_;

    std::unique_ptr<mail> mail_;
};

using TcpSmtpsSession = SmtpsSession<TcpConnection>;
using SslSmtpsSession = SmtpsSession<SslConnection>;

} // namespace mail_system


#endif // SMTPS_SESSION_H
