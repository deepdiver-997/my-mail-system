#ifndef SMTPS_SESSION_H
#define SMTPS_SESSION_H

#include "mail_system/back/common/logger.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "framework/session_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/entities/usr.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_types.hpp"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
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
    static constexpr size_t INITIAL_BUFFER_SIZE = 8192;
    static constexpr size_t MAX_BUFFER_SIZE = 1048576;
    static constexpr size_t BUFFER_GROWTH_FACTOR = 2;

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
    usr* get_usr() { return usr_.get(); }

    void create_mail_on_data_command();
    persist_storage::SubmitOwnedMailResult submit_mail_to_queue();
    bool check_mail_persist_status();
    void transfer_mail_ownership_to_outbound();
    void flush_body_and_wait();
    void reset_mail_state();
    void discard_current_mail();
    bool has_pending_mail_submission() const;
    persist_storage::PersistStatus get_pending_mail_persist_status() const;
    void cancel_pending_mail_submission();
    void clear_pending_mail_submission();

    void process_message_data(const std::string& data);
    void handle_multipart_line_and_write_attachment(const std::string& line);
    void finalize_attachment_from_context();
    void parse_smtp_command(const std::string& data);

private:
    // 扩展邮件体缓冲区，当缓冲区空间不足时调用
    // 扩展会触发异步刷盘，最多扩展 MAX_BUFFER_EXPAND_COUNT 次
    void expand_buffer();

    // 将邮件体缓冲区的内容同步写入磁盘
    // 在邮件传输完成或缓冲区满时调用
    void flush_buffer_to_disk();

    // 异步将邮件体缓冲区的内容写入磁盘
    // 提交到线程池执行，避免阻塞网络IO线程
    void async_flush_buffer_to_disk();

    // 处理写入失败的情况
    // 根据邮件的持久化状态标记为 CANCELLED 或提交删除任务
    void handle_write_failure();

    // 将数据追加到邮件体缓冲区
    // 当缓冲区空间不足时会触发扩容或刷盘
    void append_to_buffer(const char* data, size_t size);

    // 清理邮件相关的所有文件（邮件体和附件）
    // 在邮件持久化失败或需要删除时调用
    void cleanup_mail_files(mail* mail_ptr);

    // 扩展附件缓冲区，当附件数据量大时调用
    // 扩展逻辑与邮件体缓冲区类似
    void expand_attachment_buffer();

    // 将附件缓冲区的内容同步写入磁盘
    // 在附件接收完成时调用
    void flush_attachment_buffer_to_disk();

    // 异步将附件缓冲区的内容写入磁盘
    // 提交到线程池执行，避免阻塞网络IO线程
    void async_flush_attachment_buffer_to_disk();

    // 将数据追加到附件缓冲区
    // 当缓冲区空间不足时会触发扩容或刷盘
    void append_to_attachment_buffer(const char* data, size_t size);

    void wait_for_async_writes();

    std::shared_ptr<TraditionalSmtpsFsm<ConnectionType>> fsm_;
    SmtpsState state_;
    SmtpsEvent next_event_;
    bool ignore_current_command_;
    SmtpsContext context_;
    std::string last_command_args_;

    size_t buffer_size_;
    std::unique_ptr<char[]> buffer_;
    size_t buffer_used_;
    size_t buffer_expand_count_;
    static constexpr size_t MAX_BUFFER_EXPAND_COUNT = 3;

    std::vector<std::future<bool>> async_write_futures_;
    std::shared_ptr<persist_storage::PersistentQueue> persistent_queue_;
    persist_storage::PersistSubmissionTicket pending_submission_;

    std::unique_ptr<mail> mail_;
    std::unique_ptr<usr> usr_;
};

using TcpSmtpsSession = SmtpsSession<TcpConnection>;
using SslSmtpsSession = SmtpsSession<SslConnection>;

} // namespace mail_system


#endif // SMTPS_SESSION_H
