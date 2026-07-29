#pragma once

#include <cctype>

namespace mail_system {

// IMAP 命令名 → ImapEvent 映射表
namespace {
    inline const std::unordered_map<std::string, ImapEvent>& get_imap_command_map() {
        static const std::unordered_map<std::string, ImapEvent> map = {
            {"CAPABILITY", ImapEvent::CAPABILITY},
            {"LOGIN", ImapEvent::LOGIN},
            {"AUTHENTICATE", ImapEvent::AUTHENTICATE},
            {"LOGOUT", ImapEvent::LOGOUT},
            {"SELECT", ImapEvent::SELECT},
            {"EXAMINE", ImapEvent::EXAMINE},
            {"CREATE", ImapEvent::CREATE},
            {"DELETE", ImapEvent::DELETE},
            {"RENAME", ImapEvent::RENAME},
            {"SUBSCRIBE", ImapEvent::SUBSCRIBE},
            {"UNSUBSCRIBE", ImapEvent::UNSUBSCRIBE},
            {"LIST", ImapEvent::LIST},
            {"LSUB", ImapEvent::LSUB},
            {"STATUS", ImapEvent::IMAP_STATUS},
            {"APPEND", ImapEvent::APPEND},
            {"CHECK", ImapEvent::CHECK},
            {"CLOSE", ImapEvent::CLOSE},
            {"EXPUNGE", ImapEvent::EXPUNGE},
            {"SEARCH", ImapEvent::SEARCH},
            {"FETCH", ImapEvent::FETCH},
            {"STORE", ImapEvent::STORE},
            {"COPY", ImapEvent::COPY},
            {"MOVE", ImapEvent::MOVE},
            {"UID", ImapEvent::UID},
            {"NOOP", ImapEvent::NOOP},
            {"IDLE", ImapEvent::IDLE},
            {"DONE", ImapEvent::DONE},
            {"STARTTLS", ImapEvent::STARTTLS},
        };
        return map;
    }
}

// ====================================================================
// 构造函数
// ====================================================================
template <typename ConnectionType>
ImapsSession<ConnectionType>::ImapsSession(
    ServerBase* server,
    std::unique_ptr<ConnectionType> connection,
    std::shared_ptr<TraditionalImapsFsm<ConnectionType>> fsm)
    : SessionBase<ConnectionType>(std::move(connection), server)
    , fsm_(std::move(fsm))
    , state_(ImapState::INIT)
    , next_event_(ImapEvent::CONNECT)
    , context_()
{
}

// ====================================================================
// 启动入口
// ====================================================================
template <typename ConnectionType>
void ImapsSession<ConnectionType>::start(std::shared_ptr<ImapsSession> self) {
    SessionBase<ConnectionType>::do_handshake(
        self,
        boost::asio::ssl::stream_base::server,
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_SESSION_ERROR("IMAP handshake failed: {}", ec.message());
                return;
            }
            auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(s->get_fsm());
            fsm->process_event(s, ImapEvent::CONNECT, "");
        }
    );
}

template <typename ConnectionType>
void ImapsSession<ConnectionType>::start_after_starttls(std::shared_ptr<ImapsSession> self) {
    SessionBase<ConnectionType>::do_handshake(
        self,
        boost::asio::ssl::stream_base::server,
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_SESSION_ERROR("IMAP STARTTLS handshake failed: {}", ec.message());
                return;
            }
            s->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
            LOG_IMAP_INFO("STARTTLS handshake complete, waiting for commands on TLS session");
            s->do_async_read();
        }
    );
}

// ====================================================================
// handle_read: 解析一行命令（tag/command/args），store for process_read
// SMTP 的 handle_read 消费命令 → IMAP 的 handle_read 也消费命令
// ====================================================================
template <typename ConnectionType>
void ImapsSession<ConnectionType>::handle_read(const std::string& data) {
    auto self = this->shared_from_this();
    auto* session = this;

    // literal 数据累积：data 是 extract_one_line 返回的原始 chunk
    if (session->awaiting_literal_) {
        session->literal_data_buffer_.append(data);
        size_t& expected = session->expected_literal_size_;
        if (session->literal_data_buffer_.size() >= expected) {
            session->awaiting_literal_ = false;
            session->last_command_args_ = std::move(session->literal_data_buffer_);
            session->context_.current_tag = session->current_tag_;
            session->command_parsed_ = true;
            session->next_event_ = ImapEvent::APPEND;
        }
        return;
    }

    // 去掉行尾的 \r\n（由 extract_one_line 保留在 data 中）
    std::string line = data;
    if (line.size() >= 2 && line.substr(line.size() - 2) == "\r\n")
        line.resize(line.size() - 2);

    // Trim trailing whitespace
    auto trim_end = line.find_last_not_of(" \t");
    if (trim_end != std::string::npos) line.resize(trim_end + 1);

    if (line.empty()) return;  // 空行，忽略（下轮 do_async_read 取新数据）

    LOG_IMAP_DETAIL_DEBUG("IMAP line: [{}]", line);

    // IDLE 模式：只认 DONE
    if (session->context_.idle_mode) {
        std::string upper = line;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        if (upper == "DONE") {
            session->idle_done_received_ = true;
            session->command_parsed_ = true;
        }
        return;
    }

    // 检查是否 literal 声明（行尾的 {size}）
    size_t brace_pos = line.rfind('{');
    if (brace_pos != std::string::npos && line.back() == '}') {
        std::string size_str = line.substr(brace_pos + 1);
        size_str.pop_back();
        try {
            size_t literal_size = std::stoull(size_str);
            std::string cmd_part = line.substr(0, brace_pos);
            while (!cmd_part.empty() && cmd_part.back() == ' ')
                cmd_part.pop_back();

            // 解析 tag + command（literal 声明行去掉 {size} 后的部分）
            size_t first_space = cmd_part.find(' ');
            if (first_space != std::string::npos) {
                session->current_tag_ = cmd_part.substr(0, first_space);
                std::string rest = cmd_part.substr(first_space + 1);
                size_t second_space = rest.find(' ');
                if (second_space != std::string::npos) {
                    session->current_command_ = rest.substr(0, second_space);
                    session->last_command_args_ = rest.substr(second_space + 1);
                } else {
                    session->current_command_ = rest;
                    session->last_command_args_.clear();
                }
            } else {
                session->current_tag_ = cmd_part;
                session->current_command_ = cmd_part;
                session->last_command_args_.clear();
            }

            if (session->current_command_ == "APPEND")
                session->context_.pending_append_preamble = session->last_command_args_;

            std::transform(session->current_command_.begin(), session->current_command_.end(),
                          session->current_command_.begin(), ::toupper);
            LOG_IMAP_DEBUG("Literal declared: cmd={}, size={}", session->current_command_, literal_size);

            // 进入等待 literal 模式：后续 handle_read 调用会累积数据
            session->awaiting_literal_ = true;
            session->expected_literal_size_ = literal_size;
            session->literal_data_buffer_.clear();

            // 立即把当前缓冲区中已有的数据（literal body）消费掉
            auto& buf = this->command_read_buffer_;
            if (buf.size() >= literal_size) {
                session->literal_data_buffer_ = buf.substr(0, literal_size);
                buf.erase(0, literal_size);
                if (buf.size() >= 2 && buf[0] == '\r' && buf[1] == '\n')
                    buf.erase(0, 2);
                session->awaiting_literal_ = false;
                session->last_command_args_ = std::move(session->literal_data_buffer_);
                session->context_.current_tag = session->current_tag_;
                session->command_parsed_ = true;
                session->next_event_ = ImapEvent::APPEND;
                return;
            } else {
                // 发送继续响应，literal 数据不够时累积
                session->literal_data_buffer_ = buf;
                buf.clear();
                auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
                fsm->send_continuation(self, "Ready for literal data");
                return;
            }
        } catch (const std::exception& e) {
            LOG_IMAP_ERROR("Invalid literal size: {}", e.what());
        }
    }

    // ===== 普通命令解析 =====
    std::string tag, cmd, args;

    size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        tag = line;
        cmd = line;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
    } else {
        tag = line.substr(0, first_space);
        std::string rest = line.substr(first_space + 1);
        size_t second_space = rest.find(' ');
        if (second_space != std::string::npos) {
            cmd = rest.substr(0, second_space);
            args = rest.substr(second_space + 1);
        } else {
            cmd = rest;
            args.clear();
        }
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
    }

    LOG_IMAP_DETAIL_DEBUG("IMAP parsed: tag=[{}] cmd=[{}] args=[{}]", tag, cmd, args);

    auto it = get_imap_command_map().find(cmd);
    if (it != get_imap_command_map().end())
        session->next_event_ = it->second;
    else {
        session->next_event_ = ImapEvent::ERROR;
        args = "Unknown command: " + cmd;
    }

    session->current_tag_ = tag;
    session->current_command_ = cmd;
    session->last_command_args_ = args;
    session->context_.current_tag = tag;
    session->command_parsed_ = true;
}

// ====================================================================
// has_buffered_input / extract_one_line 重载
// literal 等待模式下需特殊处理——literal 数据是纯字节流无 \r\n
// ====================================================================
template <typename ConnectionType>
bool ImapsSession<ConnectionType>::has_buffered_input() const {
    if (awaiting_literal_)
        return !this->command_read_buffer_.empty();
    return SessionBase<ConnectionType>::has_buffered_input();
}

template <typename ConnectionType>
std::string ImapsSession<ConnectionType>::extract_one_line() {
    if (awaiting_literal_)
        return this->take_buffered_input();  // literal 数据按 chunk 返回
    return SessionBase<ConnectionType>::extract_one_line();
}

// ====================================================================
// compute_reply_delay
// ====================================================================
template <typename ConnectionType>
std::chrono::milliseconds ImapsSession<ConnectionType>::compute_reply_delay() const {
    return std::chrono::milliseconds(0);
}

// ====================================================================
// process_read: 根据 handle_read 解析的状态分发到 FSM
// 与 SMTP 一致——handle_read 消费命令，process_read 执行 FSM
// ====================================================================
template <typename ConnectionType>
void ImapsSession<ConnectionType>::process_read() {
    auto self = this->shared_from_this();
    auto* session = this;

    if (!session->command_parsed_) return;

    // IDLE DONE
    if (session->idle_done_received_) {
        session->idle_done_received_ = false;
        session->command_parsed_ = false;
        session->context_.idle_mode = false;
        auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
        fsm->send_tagged(self, session->context_.current_tag, "OK", "IDLE terminated");
        return;
    }

    // APPEND literal 完成
    if (session->next_event_ == ImapEvent::APPEND && !session->awaiting_literal_) {
        session->command_parsed_ = false;
        auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
        fsm->process_event(self, ImapEvent::APPEND,
                           session->current_tag_);
        return;
    }

    // 普通命令
    session->command_parsed_ = false;
    auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
    fsm->process_event(self, session->next_event_,
                       session->current_tag_);
}

// ====================================================================
// 状态机接口
// ====================================================================
template <typename ConnectionType>
void* ImapsSession<ConnectionType>::get_fsm() const {
    return fsm_.get();
}

template <typename ConnectionType>
void* ImapsSession<ConnectionType>::get_context() {
    return &context_;
}

template <typename ConnectionType>
void ImapsSession<ConnectionType>::set_current_state(int state) {
    state_ = static_cast<ImapState>(state);
}

template <typename ConnectionType>
void ImapsSession<ConnectionType>::set_next_event(int event) {
    next_event_ = static_cast<ImapEvent>(event);
}

template <typename ConnectionType>
int ImapsSession<ConnectionType>::get_current_state() const {
    return static_cast<int>(state_);
}

template <typename ConnectionType>
int ImapsSession<ConnectionType>::get_next_event() const {
    return static_cast<int>(next_event_);
}

template <typename ConnectionType>
std::string ImapsSession<ConnectionType>::get_last_command_args() const {
    return last_command_args_;
}

} // namespace mail_system
