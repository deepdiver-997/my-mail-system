#ifndef SESSION_BASE_H
#define SESSION_BASE_H

#include "mail_system/back/common/logger.h"
#include "framework/connection/i_connection.h"
#include "framework/server_base.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system {

class ServerBase;
class TcpConnection;
class SslConnection;
template <typename ConnectionType> class SessionBase;
using TcpSessionBase = SessionBase<TcpConnection>;
using SslSessionBase = SessionBase<SslConnection>;

// ================================================================
// SessionError — 会话级错误码
//   状态机 handler 通过 session->set_error(...) 设置，便于统一日志/响应。
//   子类可覆写 error_message() 提供协议特定的文本映射。
// ================================================================
enum class SessionError {
    None,
    AuthFailed,
    InvalidCommand,
    Timeout,
    Internal,
};

// ================================================================
// make_copyable — 将 move-only 回调包装为 copyable（供 do_handshake 使用）
//   成员模板，必须在头文件中 inline 定义
// ================================================================
template <typename F>
auto make_copyable(F&& f) {
    auto s = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
    return [s](auto&&... args) {
        return (*s)(std::forward<decltype(args)>(args)...);
    };
}

// ================================================================
// SessionBase — 协议无关的网络会话基类
//   管理一条 TCP/SSL 连接的生命周期、异步读写、命令缓冲和流水线处理。
//   子类通过纯虚接口注入协议解析和 FSM 事件分发逻辑。
// ================================================================
template <typename ConnectionType>
class SessionBase : public std::enable_shared_from_this<SessionBase<ConnectionType>> {
public:
    // ── 1. 构造 / 析构 ─────────────────────────────────────────
    SessionBase(std::unique_ptr<ConnectionType> connection, ServerBase* server);
    virtual ~SessionBase();

    // ── 2. 连接管理 ────────────────────────────────────────────
    virtual void close();

    std::string        get_client_ip() const;
    ConnectionType&    get_connection();
    const ConnectionType& get_connection() const;
    std::unique_ptr<ConnectionType> release_connection();

    ServerBase* get_server() const;
    void        set_server(ServerBase* server);
    bool        is_closed() const;

    // ── 3. 异步 I/O ────────────────────────────────────────────
    using WriteCallback = std::function<void(
        std::shared_ptr<SessionBase<ConnectionType>>,
        const boost::system::error_code&)>;

    void do_async_read();
    void do_async_write(const std::string& data, WriteCallback callback = nullptr);

    // 成员模板 — 必须在头文件中定义
    template <typename HandshakeHandler>
    static void do_handshake(
        std::shared_ptr<SessionBase<ConnectionType>> self,
        boost::asio::ssl::stream_base::handshake_type type,
        HandshakeHandler&& handler)
    {
        if (self->closed_ || !self->connection_) return;
        auto* conn = self->connection_.get();
        conn->async_handshake(type,
            make_copyable([self, handler = std::forward<HandshakeHandler>(handler)](
                const boost::system::error_code& error) mutable {
                if (self->closed_) return;
                if (error) {
                    // 增强诊断：输出错误码、对端 IP、本地端口、SSL 状态/OpenSSL 错误栈
                    std::string diag = self->connection_
                        ? self->connection_->get_handshake_diagnostic() : "";
                    uint16_t lport = self->connection_
                        ? self->connection_->get_local_port() : 0;
                    LOG_SESSION_ERROR("Handshake failed: {} (ec={}) client={} port={}{}",
                        error.message(), error.value(), self->get_client_ip(), lport,
                        diag.empty() ? "" : (" " + diag));
                    self->handle_error(error);
                } else {
                    LOG_SESSION_INFO("Handshake successful.");
                }
                handler(self, error);
            }));
    }

    virtual void handle_error(const boost::system::error_code& error);

    // ── 4. 命令缓冲与流水线 ────────────────────────────────────
    std::string pop_buffered_line();

    // 暂停流水线消费（DB/DNS 异步查询期间）
    // 跨线程访问：io 线程读（is_paused 决定是否继续消费），异步回调线程写（恢复）。
    bool is_paused() const { return paused_.load(std::memory_order_acquire); }
    void set_paused(bool v) { paused_.store(v, std::memory_order_release); }
    // 排空暂停期间积累的缓冲命令
    void drain_buffered_commands();

    virtual bool        has_buffered_input() const;
    virtual std::string extract_one_line();
    virtual std::string take_buffered_input();
    virtual std::chrono::milliseconds compute_reply_delay() const = 0;

    // ── 5. 认证与安全 ──────────────────────────────────────────
    bool is_authenticated() const;
    void set_authenticated(bool v);

    // 记录一次 AUTH 失败，返回 true 表示超过上限应关闭连接
    bool record_auth_failure_and_check();

    // ── 5b. 错误码 ─────────────────────────────────────────────
    void              set_error(SessionError e, const std::string& detail = "");
    SessionError      get_error() const;
    const std::string& get_error_detail() const;
    // 子类可覆写以提供协议特定的错误消息
    virtual std::string error_message(SessionError e) const;

    // ── 5c. 连接追踪（诊断） ──────────────────────────────────
    // 在读写路径拦截拷贝应用层字节（天然排除 TLS 握手，握手在 SSL 层内部）。
    // 正常关闭(QUIT/LOGOUT)丢弃缓冲；异常结束在 close() 时落盘到 traces/ 目录。
    void trace_append_inbound(const std::string& line) { trace_append("C: ", line); }
    void trace_append_outbound(const std::string& data) { trace_append("S: ", data); }
    void set_trace_clean_close() { m_trace_clean_close = true; }
    // STARTTLS 交接：把旧会话已记录的对话交给新会话，保证完整且不误判为异常
    std::string take_trace_buffer() { return std::exchange(m_trace_buf, std::string()); }
    void set_trace_buffer(std::string t) { m_trace_buf = std::move(t); }

    // ── 6. 纯虚接口（协议子类实现） ─────────────────────────────
    virtual void handle_read(const std::string& data) = 0;
    virtual void process_read() = 0;

    virtual void        set_current_state(int state) = 0;
    virtual void        set_next_event(int event) = 0;
    virtual int         get_current_state() const = 0;
    virtual int         get_next_event() const = 0;
    virtual void*       get_fsm() const = 0;
    virtual void*       get_context() = 0;
    virtual std::string get_last_command_args() const = 0;

    // ── 6d. Watchdog 阶段/闲置超时 ──────────────────────────────
    // rearm(d)：协议逻辑在"有进度/该续"时显式调用，重排单一定时器。
    // 到点（未被 rearm 取代，ec==operation_aborted 之外的触发）→ 置
    // close_requested_ 并 cancel 挂起读，强制制造一个 io 完成点；
    // do_async_read 完成回调见 close_requested_ 即走正常 close。
    // 对端断电/keep-alive 挂死时读永不返回 → 这里靠 cancel 唤醒回收。
    // 必须在 io 线程调用（连接同线程串行读/写/timer）。
    void rearm(std::chrono::milliseconds timeout);
    void disarm_timeout();
    bool close_requested() const { return close_requested_.load(std::memory_order_acquire); }

    // ── 7. 数据成员（public 临时变量，供 FSM 使用） ──────────────
    size_t last_bytes_transferred_ = 0;
    int    stay_times_ = 0;
    int    timeout_times_ = 0;

    // 未出现完整行的命令缓冲上限（见 do_async_read 中的防线）。
    // 默认 16MB：SMTP 邮件上限 10MB + 单行无换行的合法长行余量。
    virtual size_t max_command_buffer_bytes() const { return 16ull * 1024 * 1024; }

protected:
    // ── 8. 受保护数据成员 ──────────────────────────────────────
    std::unique_ptr<ConnectionType> connection_;
    std::vector<char> read_buffer_;
    std::vector<char> use_buffer_;
    std::string command_read_buffer_;
    std::string pending_write_buf_;
    mutable std::string client_address_;
    // 跨线程访问：io 线程在读写错误路径 close() 写；SPF/DNS/存储等异步
    // 回调线程在续作入口读（pause 独占约定覆盖不到这个标志本身）。
    std::atomic<bool> closed_{false};
    std::atomic<bool> paused_{false};
    // watchdog 到期标志：timer（io 线程）触发置位；do_async_read 完成回调（同样
    // io 线程）看到即 close。单线程写+读，atomic 仅为与可能存在的跨线程续跑路径
    // （worker 回调）保持一致，实际上严格串行。
    std::atomic<bool> close_requested_{false};
    bool session_authenticated_ = false;
    int  auth_attempt_count_ = 0;
    SessionError last_error_ = SessionError::None;
    std::string  last_error_detail_;
    ServerBase* m_server = nullptr;
    std::chrono::steady_clock::time_point session_start_{
        std::chrono::steady_clock::now()};

    // watchdog 单定时器（绑连接 executor，io 线程触发）。rearm 惰性创建；
    // close() 必须 cancel() 释放 async_wait 捕获的 self（断 self 循环）。
    std::shared_ptr<boost::asio::steady_timer> timeout_timer_;
    void on_watchdog_fire();   // timer 到期（未取代）→ 置标志 + cancel 挂起读

    std::string m_trace_buf;            // 累积的 C:/S: 对话
    bool m_trace_clean_close = false;   // QUIT/LOGOUT 干净关闭 → 丢弃 trace

    // ── 连接追踪（诊断）内部实现 ──────────────────────────────
    void trace_append(const std::string& prefix, const std::string& data) {
        if (data.empty()) return;
        constexpr size_t kTraceCap = 64 * 1024;   // 每连接对话上限，防内存/磁盘膨胀
        if (m_trace_buf.size() >= kTraceCap) return;
        size_t avail = kTraceCap - m_trace_buf.size();
        if (avail <= prefix.size()) return;
        m_trace_buf += prefix;
        m_trace_buf += data.substr(0, avail - prefix.size());
    }
    void trace_maybe_save();
};

} // namespace mail_system

#endif // SESSION_BASE_H
