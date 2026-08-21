#pragma once

#include "mail_system/back/mailServer/smtps_server.h"

namespace mail_system {

template <typename ConnectionType>
SmtpsSession<ConnectionType>::SmtpsSession(
    ServerBase* server,
    std::unique_ptr<ConnectionType> connection,
    std::shared_ptr<TraditionalSmtpsFsm<ConnectionType>> fsm
) : SessionBase<ConnectionType>(std::move(connection), server)
    , fsm_(std::move(fsm))
    , next_event_(SmtpsEvent::CONNECT)
    , ignore_current_command_(false)
    , context_()
    // TODO: `static_cast<SmtpsServer*>(server)` 假定 server 一定是 SmtpsServer。
    // 传非 SmtpsServer（如测试里的 ServerBase 派生类）会在 server+0x228 处越界读堆垃圾，
    // 构造野 shared_ptr 导致间歇性 SIGBUS/SIGSEGV（smtps_fsm_test 曾 ~70% 崩溃）。
    // 加固选项（任选，勿再裸转型）：
    //   1) dynamic_cast<SmtpsServer*>(server) + null 兜底（需 RTTI，本项目已启用）
    //   2) ServerBase 加虚访问器 get_persistent_queue()，SmtpsServer 覆写返回 m_persistentQueue
    //      （会向传输无关基类引入 SMTP 专属语义，抽象污染）
    //   3) 构造函数显式接收 shared_ptr<PersistentQueue> 参数（最纯，但需改 make_tcp_session 调用点）
    , persistent_queue_(static_cast<SmtpsServer*>(server)->m_persistentQueue) {}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::start(std::shared_ptr<SmtpsSession> self) {
    SessionBase<ConnectionType>::do_handshake(
        self,
        boost::asio::ssl::stream_base::server,
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) { LOG_SESSION_ERROR("Handshake failed: {}", ec.message()); return; }
            auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(s->get_fsm());
            fsm->process_event(s, SmtpsEvent::CONNECT);
        }
    );
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::start_after_starttls(std::shared_ptr<SmtpsSession> self) {
    SessionBase<ConnectionType>::do_handshake(
        self,
        boost::asio::ssl::stream_base::server,
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) { LOG_SESSION_ERROR("STARTTLS handshake failed: {}", ec.message()); return; }
            s->set_current_state(static_cast<int>(SmtpsState::WAIT_EHLO));
            s->set_next_event(static_cast<int>(SmtpsEvent::TIMEOUT));
            LOG_SMTP_DETAIL_INFO("STARTTLS handshake complete, waiting for EHLO on TLS session");
            s->do_async_read();
        }
    );
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::handle_read(const std::string& data) {
    parse_smtp_command(data);
}

template <typename ConnectionType>
bool SmtpsSession<ConnectionType>::has_buffered_input() const {
    // SMTP 保持对裸 \n 的兼容性（部分客户端不遵守 CRLF 规范）
    if (static_cast<SmtpsState>(state_.load(std::memory_order_acquire)) == SmtpsState::IN_MESSAGE)
        return !this->command_read_buffer_.empty();
    return this->command_read_buffer_.find('\n') != std::string::npos;
}

template <typename ConnectionType>
std::string SmtpsSession<ConnectionType>::extract_one_line() {
    // IN_MESSAGE 状态下返回全部缓冲数据（body 按块处理）
    if (static_cast<SmtpsState>(state_.load(std::memory_order_acquire)) == SmtpsState::IN_MESSAGE)
        return this->take_buffered_input();

    auto pos = this->command_read_buffer_.find('\n');
    if (pos == std::string::npos) return {};
    std::string line = this->command_read_buffer_.substr(0, pos + 1);
    this->command_read_buffer_.erase(0, pos + 1);
    return line;
}

template <typename ConnectionType>
std::chrono::milliseconds SmtpsSession<ConnectionType>::compute_reply_delay() const {
    return std::chrono::milliseconds(0);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::process_read() {
    if (ignore_current_command_) {
        ignore_current_command_ = false;
        this->do_async_read();
        return;
    }
    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(this->get_fsm());
    fsm->auto_process_event(this->shared_from_this());
}

template <typename ConnectionType>
void* SmtpsSession<ConnectionType>::get_fsm() const {
    return fsm_.get();
}

template <typename ConnectionType>
void* SmtpsSession<ConnectionType>::get_context() {
    return &context_;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::set_current_state(int state) {
    state_.store(state, std::memory_order_release);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::set_next_event(int event) {
    next_event_ = static_cast<SmtpsEvent>(event);
}

template <typename ConnectionType>
int SmtpsSession<ConnectionType>::get_current_state() const {
    return state_.load(std::memory_order_acquire);
}

template <typename ConnectionType>
int SmtpsSession<ConnectionType>::get_next_event() const {
    return int(next_event_);
}

template <typename ConnectionType>
std::string SmtpsSession<ConnectionType>::get_last_command_args() const {
    return last_command_args_;
}

template <typename ConnectionType>
bool SmtpsSession<ConnectionType>::commit_body() {
    if (!this->get_mail()) {
        // 压根没有邮件（例如 DATA 之前就被拒），无正文可提交
        return true;
    }
    if (!body_writer_) {
        // 有邮件却没有写入器 = 打开写入流时就失败了，正文从未落盘。
        // 这里绝不能返回 true，否则会放一封空正文的邮件过去并回 250。
        LOG_SESSION_ERROR("Mail {} has no body write stream, cannot commit",
                          this->get_mail()->id);
        handle_write_failure();
        return false;
    }

    std::string error;
    if (!body_writer_->commit(error)) {
        LOG_SESSION_ERROR("Failed to commit mail body for mail {}: {}",
                          this->get_mail() ? this->get_mail()->id : 0,
                          error);
        handle_write_failure();
        return false;
    }

    LOG_SESSION_INFO("Committed mail body, {} bytes", body_writer_->bytes_total());
    return true;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::handle_write_failure() {
    if (!this->get_mail()) {
        return;
    }

    if (this->get_mail()->persist_status == persist_storage::PersistStatus::PENDING) {
        this->get_mail()->persist_status = persist_storage::PersistStatus::CANCELLED;
        LOG_SESSION_WARN("Mail {} write failed, marked as CANCELLED", this->get_mail()->id);
    } else {
        LOG_SESSION_WARN("Mail {} write failed after processing started, submitting delete task", this->get_mail()->id);
        if (persistent_queue_) {
            auto mail_ptr = this->get_mail();
            this->m_server->m_workerThreadPool->submit([this, mail_ptr]() {
                persistent_queue_->delete_task(mail_ptr);
            });
        }
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::append_body_data(const char* data, size_t size) {
    if (!body_writer_) {
        LOG_SESSION_WARN("Body data arrived without an open write stream, dropping {} bytes", size);
        return;
    }

    // 首次失败已通过 handle_write_failure 标记 CANCELLED / 提交清理任务；
    // 之后每个 chunk 都只是丢弃，避免重复提交 delete_task 刷爆工作线程池。
    if (body_writer_->failed()) {
        return;
    }

    std::string error;
    if (!body_writer_->write(data, size, error)) {
        LOG_SESSION_ERROR("Failed to write mail body for mail {}: {}",
                          this->get_mail() ? this->get_mail()->id : 0,
                          error);
        handle_write_failure();
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::create_mail_on_data_command() {
    if (this->get_mail()) {
        LOG_SESSION_WARN("Mail already created, skipping");
        return;
    }

    LOG_SESSION_INFO("Creating new mail object on DATA command");
    this->mail_ = std::make_unique<mail>();
    auto& gen = algorithm::get_snowflake_generator();
    this->get_mail()->from = context_.sender_address;
    this->get_mail()->to = context_.recipient_addresses;
    this->mail_->id = gen.next_id();
    for (size_t i = 0; i < this->get_mail()->to.size(); ++i) {
        this->mail_->ids.push_back(gen.next_id());
    }
    this->get_mail()->send_time = std::time(nullptr);
    this->get_mail()->status = 1;
    this->get_mail()->box_id = 1;
    this->get_mail()->subject = "(无主题)";
    this->get_mail()->persist_status = persist_storage::PersistStatus::PENDING;

    auto storage_provider = this->m_server->m_shardRouter->get_storage(
        static_cast<size_t>(this->context_.shard_index));
    if (storage_provider) {
        this->get_mail()->body_path = storage_provider->build_mail_body_key(this->get_mail()->id);
    } else {
        auto cfg = std::atomic_load(&this->m_server->m_config);
        std::string body_path = cfg->mail_storage_path;
        if (!body_path.empty() && body_path.back() != '/' && body_path.back() != '\\') {
            body_path.push_back('/');
        }
        body_path += std::to_string(this->get_mail()->id);
        this->get_mail()->body_path = body_path;
    }

    // 整个 DATA 阶段只打开一次写入句柄，写入位置由 MailBodyWriter 用显式 offset 管理。
    // 没有 storage provider 时直接用本地文件流，不再各处重复手写 ofstream。
    std::string open_error;
    std::unique_ptr<storage::IWriteStream> stream;
    if (storage_provider) {
        stream = storage_provider->open_write(this->get_mail()->body_path, open_error);
    } else {
        stream = storage::LocalFileWriteStream::open(this->get_mail()->body_path, open_error);
    }
    if (!stream) {
        LOG_SESSION_ERROR("Failed to open mail body write stream for {}: {}",
                          this->get_mail()->body_path, open_error);
        handle_write_failure();
        return;
    }
    body_writer_ = std::make_unique<storage::MailBodyWriter>(std::move(stream));

    LOG_SESSION_INFO("Created mail {} on DATA command, from: {}, recipients: {}",
        this->get_mail()->id, this->get_mail()->from, this->get_mail()->to.size());
}

template <typename ConnectionType>
persist_storage::SubmitOwnedMailResult SmtpsSession<ConnectionType>::submit_mail_to_queue() {
    persist_storage::SubmitOwnedMailResult result;
    if (!this->get_mail()) {
        result.error = "no current mail";
        LOG_SESSION_WARN("No mail to submit");
        return result;
    }

    if (!context_.parsed_subject.empty()) {
        this->get_mail()->subject = context_.parsed_subject;
    }
    if (!context_.source_message_id.empty()) {
        this->get_mail()->source_message_id = context_.source_message_id;
    }
    this->get_mail()->header = context_.header_buffer;

    for (auto& att : context_.streamed_attachments) {
        this->get_mail()->attachments.push_back(std::move(att));
    }

    if (persistent_queue_) {
        auto owned_mail = this->get_mail_ptr();
        result = persistent_queue_->submit_owned_mail(std::move(owned_mail));
        if (!result.accepted) {
            this->mail_ = std::move(result.rejected_mail);
            LOG_SESSION_ERROR("Failed to submit mail {} to persistent queue: {}",
                              this->get_mail() ? this->get_mail()->id : 0,
                              result.error);
            return result;
        }
        pending_submission_ = result.ticket;
        LOG_SESSION_INFO("Submitted mail {} to persistent queue", pending_submission_.mail_id);
    } else {
        // 压测 / 无 DB 模式：直接标记接受
        result.accepted = true;
        result.ticket.mail_id = this->get_mail()->id;
        pending_submission_ = result.ticket;
        LOG_SESSION_INFO("Mail {} accepted without persistence (null queue)", this->get_mail()->id);
    }

    return result;
}

template <typename ConnectionType>
bool SmtpsSession<ConnectionType>::check_mail_persist_status() {
    if (!has_pending_mail_submission()) {
        LOG_SESSION_WARN("No pending mail submission to check persist status");
        return false;
    }

    auto status = get_pending_mail_persist_status();

    if (status == persist_storage::PersistStatus::SUCCESS) {
        LOG_SESSION_INFO("Mail {} persisted successfully", pending_submission_.mail_id);
        return true;
    }

    if (status == persist_storage::PersistStatus::PENDING ||
        status == persist_storage::PersistStatus::PROCESSING) {
        // 仍在处理中，调用方应继续等待
        LOG_SESSION_DEBUG("Mail {} still processing (status {})",
                          pending_submission_.mail_id,
                          static_cast<int>(status));
        return false;
    }

    LOG_SESSION_ERROR("Mail {} persist failed with status {}",
        pending_submission_.mail_id, static_cast<int>(status));
    return false;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::discard_current_mail() {
    if (!this->get_mail()) {
        return;
    }
    cleanup_mail_files(this->get_mail());
    reset_mail_state();
}

template <typename ConnectionType>
bool SmtpsSession<ConnectionType>::has_pending_mail_submission() const {
    return pending_submission_.valid();
}

template <typename ConnectionType>
persist_storage::PersistStatus SmtpsSession<ConnectionType>::get_pending_mail_persist_status() const {
    if (!pending_submission_.valid()) {
        return persist_storage::PersistStatus::CANCELLED;
    }
    return pending_submission_.status.load();
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::cancel_pending_mail_submission() {
    if (pending_submission_.valid()) {
        pending_submission_.request_cancel();
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::clear_pending_mail_submission() {
    pending_submission_ = persist_storage::PersistSubmissionTicket{};
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::cleanup_mail_files(mail* mail_ptr) {
    if (!mail_ptr) {
        return;
    }

    if (!mail_ptr->body_path.empty()) {
        if (this->m_server->m_shardRouter->get_storage(static_cast<size_t>(this->context_.shard_index))) {
            std::string error;
            if (this->m_server->m_shardRouter->get_storage(static_cast<size_t>(this->context_.shard_index))->remove_object(mail_ptr->body_path, error)) {
                LOG_SESSION_INFO("Deleted mail body file: {}", mail_ptr->body_path);
            } else {
                LOG_SESSION_WARN("Failed to delete mail body file: {}, error={}", mail_ptr->body_path, error);
            }
        } else if (std::remove(mail_ptr->body_path.c_str()) == 0) {
            LOG_SESSION_INFO("Deleted mail body file: {}", mail_ptr->body_path);
        } else {
            LOG_SESSION_WARN("Failed to delete mail body file: {}", mail_ptr->body_path);
        }
    }

    for (const auto& att : mail_ptr->attachments) {
        if (!att.filepath.empty()) {
            if (this->m_server->m_shardRouter->get_storage(static_cast<size_t>(this->context_.shard_index))) {
                std::string error;
                if (this->m_server->m_shardRouter->get_storage(static_cast<size_t>(this->context_.shard_index))->remove_object(att.filepath, error)) {
                    LOG_SESSION_INFO("Deleted attachment file: {}", att.filepath);
                } else {
                    LOG_SESSION_WARN("Failed to delete attachment file: {}, error={}", att.filepath, error);
                }
            } else if (std::remove(att.filepath.c_str()) == 0) {
                LOG_SESSION_INFO("Deleted attachment file: {}", att.filepath);
            } else {
                LOG_SESSION_WARN("Failed to delete attachment file: {}", att.filepath);
            }
        }
    }

    for (const auto& att : context_.streamed_attachments) {
        if (!att.filepath.empty()) {
            if (std::remove(att.filepath.c_str()) == 0) {
                LOG_SESSION_INFO("Deleted streamed attachment file: {}", att.filepath);
            } else {
                LOG_SESSION_WARN("Failed to delete streamed attachment file: {}", att.filepath);
            }
        }
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::reset_mail_state() {
    this->mail_ = nullptr;
    // 未 commit 就销毁 → MailBodyWriter 析构自动 abort，半成品文件不会留在盘上
    body_writer_.reset();
    clear_pending_mail_submission();

    context_.mail_data.clear();
    context_.header_buffer.clear();
    context_.parsed_subject.clear();
    context_.source_message_id.clear();
    context_.text_body_buffer.clear();
    context_.line_buffer.clear();
    context_.body_limit_exceeded = false;
    context_.streaming_enabled = false;
    context_.multipart = false;
    context_.boundary.clear();
    context_.sender_address.clear();
    context_.recipient_addresses.clear();
    context_.streamed_attachments.clear();

    LOG_SESSION_DEBUG("Mail state reset");
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::process_message_data(const std::string& data) {
    algorithm::process_message_data(context_, data);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::finalize_attachment_from_context() {
    if (context_.current_part_is_attachment && !context_.current_attachment_filename.empty()) {
        // 附件字节已包含在 body 文件中（DATA 阶段整体落盘），此处仅记录元数据
        attachment att;
        att.filename = context_.current_attachment_filename;
        att.filepath = context_.current_attachment_path;
        att.mime_type = context_.current_part_mime.empty() ? "application/octet-stream" : context_.current_part_mime;
        att.file_size = context_.current_attachment_size;
        att.upload_time = std::time(nullptr);

        context_.streamed_attachments.push_back(std::move(att));
        LOG_SESSION_DEBUG("Attachment finalized and added, total count={}", context_.streamed_attachments.size());
    }

    context_.current_attachment_filename.clear();
    context_.current_attachment_path.clear();
    context_.current_attachment_size = 0;
    context_.current_part_headers.clear();
    context_.current_part_mime.clear();
    context_.current_part_encoding.clear();
    context_.current_part_is_attachment = false;
    context_.base64_remainder.clear();
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::parse_smtp_command(const std::string& data) {
    std::string trimmed;

    if (static_cast<SmtpsState>(state_.load(std::memory_order_acquire)) != SmtpsState::IN_MESSAGE) {
        // extract_one_line 已返回一条完整命令（含 \n），直接 trim 即可
        trimmed = algorithm::trim(data);

        if (trimmed.empty()) {
            next_event_ = SmtpsEvent::TIMEOUT;
            last_command_args_.clear();
            return;
        }

        LOG_SESSION_DEBUG("Handling data: {}", data);
    } else {
        trimmed = algorithm::trim(data);
        LOG_SESSION_DEBUG("Handling data: {}", data);
    }

    if (static_cast<SmtpsState>(state_.load(std::memory_order_acquire)) == SmtpsState::IN_MESSAGE) {
        process_message_data(data);

        bool data_end_seen = (trimmed == ".") || (data.find("\r\n.\r\n") != std::string::npos);
        std::string write_chunk = data;
        if (data_end_seen) {
            // 去掉终止符"\r\n.\r\n"部分避免写入
            size_t pos = write_chunk.find("\r\n.\r\n");
            if (pos != std::string::npos) {
                // 终止符之后的剩余数据（如下一条命令）推回缓冲区
                std::string remaining = write_chunk.substr(pos + 5);
                write_chunk = write_chunk.substr(0, pos);
                if (!remaining.empty())
                    this->command_read_buffer_.append(remaining);
            } else if (trimmed == "." || trimmed == ".\r\n") {
                write_chunk.clear();
            }
        }

        if (!write_chunk.empty()) {
            // RFC 5321 dot-stuffing: strip one leading dot from each line
            if (!context_.streaming_enabled) {
                std::string unstuffed;
                unstuffed.reserve(write_chunk.size());
                size_t pos = 0;
                while (pos < write_chunk.size()) {
                    size_t nl = write_chunk.find("\r\n", pos);
                    size_t line_end = (nl != std::string::npos) ? nl : write_chunk.size();
                    size_t content = pos;
                    if (content < line_end && write_chunk[content] == '.')
                        content++;
                    unstuffed.append(write_chunk, content, line_end - content);
                    if (nl != std::string::npos) {
                        unstuffed += "\r\n";
                        pos = nl + 2;
                    } else {
                        break;
                    }
                }
                write_chunk = std::move(unstuffed);
            }
            append_body_data(write_chunk.data(), write_chunk.size());
        }

        if (data_end_seen) {
            finalize_attachment_from_context();
            next_event_ = SmtpsEvent::DATA_END;
            last_command_args_.clear();
        } else {
            next_event_ = SmtpsEvent::DATA;
            if (!context_.streaming_enabled) {
                if (!trimmed.empty() && trimmed[0] == '.') {
                    last_command_args_ = data.substr(data.find('.') + 1);
                } else {
                    last_command_args_ = data;
                }
            } else {
                last_command_args_.clear();
            }
        }

        if (fsm_) {
            LOG_SESSION_DEBUG("IN_MESSAGE next_event_: {}", fsm_->get_event_name(next_event_));
        }
        return;
    }

    ignore_current_command_ = false;

    auto st = static_cast<SmtpsState>(state_.load(std::memory_order_acquire));
    if (st == SmtpsState::WAIT_AUTH_USERNAME || st == SmtpsState::WAIT_AUTH_PASSWORD ||
        (st == SmtpsState::WAIT_AUTH && context_.plain_auth_expected)) {
        next_event_ = SmtpsEvent::AUTH;
        last_command_args_ = trimmed;
        if (fsm_) {
            LOG_SESSION_DEBUG("AUTH next_event_: {}", fsm_->get_event_name(next_event_));
        }
        return;
    }

    std::string command;
    std::string args;
    std::string upper_trimmed = trimmed;
    std::transform(upper_trimmed.begin(), upper_trimmed.end(), upper_trimmed.begin(), ::toupper);

    if (upper_trimmed.compare(0, 9, "MAIL FROM") == 0) {
        command = "MAIL FROM";
        args = trimmed.substr(4);
    } else if (upper_trimmed.compare(0, 7, "RCPT TO") == 0) {
        command = "RCPT TO";
        args = trimmed.substr(4);
    } else {
        size_t space_pos = trimmed.find(' ');
        if (space_pos != std::string::npos) {
            command = trimmed.substr(0, space_pos);
            args = trimmed.substr(space_pos + 1);
        } else {
            command = trimmed;
        }
        std::transform(command.begin(), command.end(), command.begin(), ::toupper);
    }

    last_command_args_ = args;

    LOG_SESSION_DEBUG("command: {}, args: {}", command, args);

    if (command == "EHLO" || command == "HELO") {
        next_event_ = SmtpsEvent::EHLO;
    } else if (command == "AUTH") {
        next_event_ = SmtpsEvent::AUTH;
    } else if (command == "MAIL FROM") {
        next_event_ = SmtpsEvent::MAIL_FROM;
    } else if (command == "RCPT TO") {
        next_event_ = SmtpsEvent::RCPT_TO;
    } else if (command == "DATA") {
        next_event_ = SmtpsEvent::DATA;
    } else if (command == "QUIT") {
        next_event_ = SmtpsEvent::QUIT;
    } else if (command == "STARTTLS") {
        next_event_ = SmtpsEvent::STARTTLS;
    } else {
        next_event_ = SmtpsEvent::ERROR;
        last_command_args_ = "Unknown command: " + command;
    }

    if (fsm_) {
        LOG_SESSION_DEBUG("next_event_: {}", fsm_->get_event_name(next_event_));
    }
}

} // namespace mail_system
