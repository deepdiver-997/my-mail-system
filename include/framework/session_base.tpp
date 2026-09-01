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

    // 释放 watchdog：cancel 让 async_wait 的 handler 以 operation_aborted 立即
    // 返回 → io_context 释放其捕获的 self（断 timeout_timer_ ↔ self 的引用循环）
    disarm_timeout();

    // 连接追踪：正常关闭(QUIT/LOGOUT)丢弃，异常结束落盘
    trace_maybe_save();

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

            // watchdog 回收点：timer 到期已置 close_requested_ 并 cancel 本读 → 这里
            // 以 operation_aborted 完成 → 见标志即正常 close（对端断电时唯一观察点）。
            if (self->close_requested_) { self->close(); return; }

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

            // 无换行数据的无界累积防线：单次 read() 受 read_buffer_（8KB）约束，
            // 内核侧由 TCP 接收窗口兜底，但应用层若永远等不到换行符，缓冲会
            // 随发送持续增长（SMTP DATA 单行无 CRLF 的 200MB 就从这里过）。
            // 超上限直接断开：超限数据不可能是合法命令/合法邮件。
            if (self->command_read_buffer_.size() > self->max_command_buffer_bytes()) {
                LOG_SESSION_ERROR("Command buffer {} bytes without a complete line (limit {}), closing",
                                  self->command_read_buffer_.size(),
                                  self->max_command_buffer_bytes());
                self->close();
                return;
            }

            // 流水线消费：paused 时停止消费，等待 DB 回调排空
            while (self->has_buffered_input() && !self->is_paused()) {
                std::string line = self->extract_one_line();
                self->trace_append_inbound(line);   // 连接追踪：在"移除点"记录收到的行
                self->handle_read(line);
                self->process_read();
            }
        });
}

template <typename ConnectionType>
void SessionBase<ConnectionType>::do_async_write(
    const std::string& data, WriteCallback callback)
{
    if (closed_ || !connection_) return;

    trace_append_outbound(data);   // 连接追踪：记录发出的响应

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
// 3b. Watchdog 阶段/闲置超时
// ================================================================
template <typename ConnectionType>
void SessionBase<ConnectionType>::rearm(std::chrono::milliseconds timeout) {
    if (closed_ || !connection_) return;
    if (!timeout_timer_)
        timeout_timer_ = std::make_shared<boost::asio::steady_timer>(connection_->get_executor());
    // expires_after 重排：旧的 async_wait 因到期时间改变而自发 operation_aborted，
    // 只剩最后一次 rearm 真正会触发 → 天然实现"单定时器、后设取代先设"。
    timeout_timer_->expires_after(timeout);
    timeout_timer_->async_wait(
        [self = this->shared_from_this()](const boost::system::error_code& ec) {
            if (ec) return;                 // operation_aborted：被 rearm / close 取代
            if (self->closed_) return;
            self->on_watchdog_fire();
        });
}

template <typename ConnectionType>
void SessionBase<ConnectionType>::disarm_timeout() {
    if (timeout_timer_) timeout_timer_->cancel();
}

// io 线程触发（steady_timer handler 绑连接 executor）。
template <typename ConnectionType>
void SessionBase<ConnectionType>::on_watchdog_fire() {
    close_requested_.store(true, std::memory_order_release);
    // 强制挂起读完成：对端断电时读永不返回，cancel 制造一个 io 完成点；
    // do_async_read 完成回调看到 close_requested_ 即走正常 close。
    if (connection_ && connection_->is_open()) connection_->cancel();
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
void SessionBase<ConnectionType>::drain_buffered_commands() {
    paused_ = false;
    while (has_buffered_input() && !paused_) {
        std::string line = extract_one_line();
        trace_append_inbound(line);   // 连接追踪：同上，在移除点记录
        handle_read(line);
        process_read();
    }
    // 注意: drain 只负责消费已缓冲的命令，不启动新的异步读。
    // 调用方必须在 drain 返回后自行判断是否需要 do_async_read()。
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

// ================================================================
// 连接追踪（诊断）：正常关闭丢弃，异常结束落盘
// ================================================================
template <typename ConnectionType>
void SessionBase<ConnectionType>::trace_maybe_save() {
    if (m_trace_buf.empty()) return;
    if (m_trace_clean_close) { m_trace_buf.clear(); return; }  // 正常关闭 → 丢弃

    // 异常结束 → 落盘到 <log_dir>/traces/
    try {
        std::filesystem::path dir;
        if (m_server) {
            auto cfg = std::atomic_load(&m_server->m_config);
            if (cfg && !cfg->log_file.empty())
                dir = std::filesystem::path(cfg->log_file).parent_path();
        }
        if (dir.empty()) dir = std::filesystem::path("/tmp/logs");
        dir /= "traces";
        std::filesystem::create_directories(dir);

        // 诊断期可能大量异常连接（如垃圾扫描器），限制文件数防磁盘膨胀：超 300 删最旧
        constexpr size_t kMaxTraceFiles = 300;
        try {
            std::vector<std::filesystem::path> files;
            for (auto& e : std::filesystem::directory_iterator(dir)) {
                if (e.is_regular_file() && e.path().extension() == ".txt")
                    files.push_back(e.path());
            }
            if (files.size() >= kMaxTraceFiles) {
                std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
                    return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
                });
                for (size_t i = 0; i + kMaxTraceFiles <= files.size(); ++i)
                    std::filesystem::remove(files[i]);
            }
        } catch (...) {}

        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
        gmtime_r(&t, &tm);
        char ts[32];
        std::snprintf(ts, sizeof(ts), "%04d%02d%02d-%02d%02d%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);

        static std::atomic<uint64_t> seq{0};
        uint16_t lport = 0;
        if (connection_) {
            try { lport = connection_->get_local_port(); } catch (...) {}
        }
        std::string fname = "trace-" + std::string(ts) + "-" + get_client_ip()
                          + "-" + std::to_string(lport) + "-"
                          + std::to_string(seq.fetch_add(1)) + ".txt";
        std::ofstream of(dir / fname, std::ios::trunc);
        if (of.is_open()) {
            of << "=== abnormal connection trace ===\n"
               << "client_ip: " << get_client_ip() << "\n"
               << "local_port: " << lport << "\n"
               << "auth: " << (session_authenticated_ ? "yes" : "no") << "\n"
               << "error: " << error_message(last_error_);
            if (!last_error_detail_.empty()) of << " (" << last_error_detail_ << ")";
            of << "\n--- conversation ---\n" << m_trace_buf << "\n";
        }
    } catch (...) {}
    m_trace_buf.clear();
}

} // namespace mail_system

#endif // SESSION_BASE_TPP
