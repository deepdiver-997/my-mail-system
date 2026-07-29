#ifndef SESSION_BASE_TPP
#define SESSION_BASE_TPP

namespace mail_system {

// ================================================================
// 1. 构造 / 析构
// ================================================================
template <typename ConnectionType>
SessionBase<ConnectionType>::SessionBase(
    std::unique_ptr<ConnectionType> connection, ServerBase* server)
    : connection_(std::move(connection))
    , read_buffer_(8192), use_buffer_(8192)
    , m_server(server)
{}

template <typename ConnectionType>
SessionBase<ConnectionType>::~SessionBase() {
    if (!closed_) close();
}

// ================================================================
// 2. 连接管理
// ================================================================
template <typename ConnectionType>
void SessionBase<ConnectionType>::close() {
    if (closed_) return;
    closed_ = true;

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - session_start_).count();
    if (m_server) {
        MetricsServer::LabelMap lbls;
        lbls["auth"] = session_authenticated_ ? "yes" : "no";
        m_server->push_metric_observe("protorelay_session_duration_seconds", lbls, elapsed);
        m_server->record_session_end(get_client_ip(), session_authenticated_);
        m_server->decrement_connection_count();
    }

    // 将 pipeline 模式下积压的响应刷到连接再关闭
    if (!pending_write_buf_.empty() && connection_ && connection_->is_open()) {
        auto payload = std::make_shared<std::string>(std::move(pending_write_buf_));
        pending_write_buf_.clear();
        connection_->async_write(boost::asio::buffer(*payload),
            [payload](const boost::system::error_code&, std::size_t) {});
    }

    try {
        if (connection_ && connection_->is_open()) {
            connection_->close();
            LOG_SESSION_INFO("Session closed for {}", get_client_ip());
        }
    } catch (const std::exception& e) {
        LOG_SESSION_ERROR("Error closing session: {}", e.what());
    }
}

template <typename ConnectionType>
std::string SessionBase<ConnectionType>::get_client_ip() const {
    if (client_address_.empty() && connection_) {
        try {
            client_address_ = connection_->get_remote_ip();
        } catch (const std::exception& e) {
            LOG_SESSION_ERROR("Error getting client IP: {}", e.what());
        }
    }
    return client_address_;
}

template <typename ConnectionType>
ConnectionType& SessionBase<ConnectionType>::get_connection() {
    return *connection_;
}

template <typename ConnectionType>
const ConnectionType& SessionBase<ConnectionType>::get_connection() const {
    return *connection_;
}

template <typename ConnectionType>
std::unique_ptr<ConnectionType> SessionBase<ConnectionType>::release_connection() {
    return std::move(connection_);
}

template <typename ConnectionType>
ServerBase* SessionBase<ConnectionType>::get_server() const {
    return m_server;
}

template <typename ConnectionType>
void SessionBase<ConnectionType>::set_server(ServerBase* server) {
    m_server = server;
}

template <typename ConnectionType>
bool SessionBase<ConnectionType>::is_closed() const {
    return closed_;
}

// ================================================================
// 3. 异步 I/O
// ================================================================
template <typename ConnectionType>
void SessionBase<ConnectionType>::handle_error(const boost::system::error_code& error) {
    LOG_SESSION_ERROR("SessionBase Error: {}", error.message());
    close();
}

template <typename ConnectionType>
void SessionBase<ConnectionType>::do_async_read() {
    if (closed_ || !connection_) return;

    // 缓冲区已有完整行 → 返回，由外层 while 循环统一消费
    if (has_buffered_input()) return;

    auto* conn = connection_.get();
    auto buf = boost::asio::buffer(read_buffer_);

    conn->async_read(buf,
        [self = this->shared_from_this()](
            const boost::system::error_code& error, std::size_t bytes) mutable {
            if (self->closed_) return;

            if (error) {
                LOG_SESSION_ERROR("Error reading data: {}", error.message());
                self->handle_error(error);
                return;
            }

            if (bytes == 0) {
                self->do_async_read();
                return;
            }

            self->last_bytes_transferred_ = bytes;
            // 原始 TCP 数据追加到命令缓冲区（可能包含多行 + 不完整尾行）
            self->command_read_buffer_.append(
                self->read_buffer_.data(), bytes);

            // 流水线消费：每次从缓冲区取一行，传给 handle_read 处理
            while (self->has_buffered_input()) {
                self->handle_read(self->extract_one_line());
                self->process_read();
            }
        });
}

template <typename ConnectionType>
void SessionBase<ConnectionType>::do_async_write(
    const std::string& data, WriteCallback callback)
{
    if (closed_ || !connection_) return;

    // 还有流水线命令 → 累积响应，同步调用回调（回调中 do_async_read
    // 会取下一行 + process_read，链式消费直到缓冲区空）
    if (has_buffered_input()) {
        pending_write_buf_ += data;
        if (callback) {
            callback(this->shared_from_this(), boost::system::error_code());
        }
        return;
    }

    auto* conn = connection_.get();
    auto payload = std::make_shared<std::string>(
        std::move(pending_write_buf_) + data);
    conn->async_write_with_delay(boost::asio::buffer(*payload),
        compute_reply_delay(),
        [self = this->shared_from_this(), payload, cb = std::move(callback)](
            const boost::system::error_code& ec, std::size_t) mutable {
            if (self->closed_) return;
            if (ec) { self->handle_error(ec); return; }
            if (cb) cb(self, ec);
            else    self->do_async_read();
        });
}

// ================================================================
// 4. 命令缓冲与流水线
// ================================================================
template <typename ConnectionType>
std::string SessionBase<ConnectionType>::pop_buffered_line() {
    size_t nl = command_read_buffer_.find('\n');
    if (nl == std::string::npos) return "";
    std::string line = command_read_buffer_.substr(0, nl + 1);
    command_read_buffer_.erase(0, nl + 1);
    return line;
}

template <typename ConnectionType>
bool SessionBase<ConnectionType>::has_buffered_input() const {
    // 默认检查 \r\n（IMAP）。SMTP 重载为检查 \n。
    return command_read_buffer_.find("\r\n") != std::string::npos;
}

template <typename ConnectionType>
std::string SessionBase<ConnectionType>::extract_one_line() {
    // 默认使用 \r\n。SMTP 重载为 \n（兼容仅发 LF 的客户端）。
    auto pos = command_read_buffer_.find("\r\n");
    if (pos == std::string::npos) return {};
    std::string line = command_read_buffer_.substr(0, pos + 2);
    command_read_buffer_.erase(0, pos + 2);
    return line;
}

template <typename ConnectionType>
std::string SessionBase<ConnectionType>::take_buffered_input() {
    std::string s = std::move(command_read_buffer_);
    command_read_buffer_.clear();
    return s;
}

// ================================================================
// 5. 认证与安全
// ================================================================
template <typename ConnectionType>
bool SessionBase<ConnectionType>::is_authenticated() const {
    return session_authenticated_;
}

template <typename ConnectionType>
void SessionBase<ConnectionType>::set_authenticated(bool v) {
    session_authenticated_ = v;
}

template <typename ConnectionType>
bool SessionBase<ConnectionType>::record_auth_failure_and_check() {
    auth_attempt_count_++;
    size_t max_attempts = 3;
    if (m_server) {
        auto cfg = std::atomic_load(&m_server->m_config);
        if (cfg) max_attempts = cfg->max_auth_attempts;
    }
    if (static_cast<size_t>(auth_attempt_count_) >= max_attempts) {
        LOG_SESSION_WARN("AUTH failures exceeded ({}/{}), closing session from {}",
            auth_attempt_count_, max_attempts, get_client_ip());
        return true;
    }
    return false;
}

// ── 5b. 错误码 ─────────────────────────────────────────────────
template <typename ConnectionType>
void SessionBase<ConnectionType>::set_error(SessionError e, const std::string& detail) {
    last_error_ = e;
    last_error_detail_ = detail;
}

template <typename ConnectionType>
SessionError SessionBase<ConnectionType>::get_error() const {
    return last_error_;
}

template <typename ConnectionType>
const std::string& SessionBase<ConnectionType>::get_error_detail() const {
    return last_error_detail_;
}

template <typename ConnectionType>
std::string SessionBase<ConnectionType>::error_message(SessionError e) const {
    switch (e) {
        case SessionError::None:           return "";
        case SessionError::AuthFailed:     return "Authentication failed";
        case SessionError::InvalidCommand: return "Invalid command";
        case SessionError::Timeout:        return "Connection timeout";
        case SessionError::Internal:       return "Internal server error";
    }
    return "Unknown error";
}

} // namespace mail_system

#endif // SESSION_BASE_TPP
