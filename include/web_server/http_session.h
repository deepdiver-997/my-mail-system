#ifndef WEB_SERVER_HTTP_SESSION_H
#define WEB_SERVER_HTTP_SESSION_H
// ──────────────────────────────────────────────────────────────────
// HttpSession — HTTP/1.1 会话（覆写 SessionBase 的"读"为帧式）
//
// 关键：SMTP 用的是"按行拆命令"，这里保持同一套 do_async_read 循环，
// 只覆写 has_buffered_input/extract_one_line 让它按状态拆帧：
//   - WAIT_REQUEST_LINE / WAIT_HEADERS：按 '\n' 拆行（和 SMTP 完全一致）
//   - WAIT_BODY：按 Content-Length / chunked 拆 body（长度定界，绝不越界
//     多读进下一个 keep-alive 请求 —— 这是与 SMTP dot-stuffing 的本质分叉）
// ──────────────────────────────────────────────────────────────────
#include "framework/session_base.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "mail_system/back/common/logger.h"
#include "web_server/http_types.hpp"
#include "web_server/http_fsm.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

namespace web_server {

template <typename ConnectionType> class HttpFsm;

template <typename ConnectionType>
class HttpSession : public mail_system::SessionBase<ConnectionType> {
    friend class HttpFsm<ConnectionType>;

    // 单请求 body 上限（防内存膨胀，超过回 413）
    static constexpr size_t kMaxRequestBody = 8ull * 1024 * 1024;

public:
    // watchdog 各阶段闲置超时（rearm 是"距上次进度"的 deadline，由本类在每个进度点续）
    static constexpr std::chrono::milliseconds kRequestLineTimeout{30'000};
    static constexpr std::chrono::milliseconds kHeaderTimeout{30'000};
    static constexpr std::chrono::milliseconds kBodyTimeout{60'000};
    static constexpr std::chrono::milliseconds kKeepAliveTimeout{30'000};

    HttpSession(mail_system::ServerBase* server,
                std::unique_ptr<ConnectionType> connection,
                std::shared_ptr<HttpFsm<ConnectionType>> fsm);
    ~HttpSession() override = default;

    // 与 SmtpsSession::start 同型：接收入口，设初态 + 首次读
    static void start(std::shared_ptr<HttpSession> self);
    // TcpServerBase 的 TLS 交接点需要该静态入口（HTTP 无 STARTTLS，永不触发，
    // 仅为通过 handoff_starttls_socket 的编译检查而存在）。
    static void start_after_starttls(std::shared_ptr<HttpSession> self) { start(std::move(self)); }

    // ── SessionBase 覆写 ────────────────────────────────────────
    void handle_read(const std::string& data) override;
    void process_read() override;
    bool has_buffered_input() const override;
    std::string extract_one_line() override;
    std::chrono::milliseconds compute_reply_delay() const override;
    void* get_fsm() const override;
    void* get_context() override;
    void set_current_state(int state) override;
    void set_next_event(int event) override;
    int  get_current_state() const override;
    int  get_next_event() const override;
    std::string get_last_command_args() const override;

    // ── FSM 调用的协议 API ──────────────────────────────────────
    HttpRequest& request() { return request_; }
    // 由 HttpServer 工厂注入静态根目录
    void set_doc_root(std::string d) { doc_root_ = std::move(d); }

    // header 结束是否还有 body 要读（由 determine_body_framing 决定）
    bool has_body_pending() const { return body_pending_; }
    // header_end 过渡到 WAIT_BODY 前，用请求里的 framing 字段武装消费模式
    void begin_body();
    // 完整请求已就绪 → 走响应管线（serve 是终结点，都经这里）
    void serve();
    // 畸形请求：发 400 并关连接
    void send_error_and_close(int status);

private:
    // SessionBase 继承 enable_shared_from_this<SessionBase<...>>，shared_from_this()
    // 返回的是基类指针；这里下转回 HttpSession（实际对象就是它，static_pointer_cast
    // 合法），便于回调里调返回阶段/流式写等派生方法。
    std::shared_ptr<HttpSession<ConnectionType>> self_ptr() {
        return std::static_pointer_cast<HttpSession<ConnectionType>>(this->shared_from_this());
    }

    enum class BodyFrame { NONE, SIZE, CHUNKED };

    // body 消费子状态机（WAIT_BODY 用）
    bool     body_armed_     = false;   // begin_body() 已武装
    bool     body_pending_   = false;   // 本请求是否需要读 body
    bool     body_chunked_   = false;   // HEADER_END 判定出的 framing
    size_t   body_content_length_ = 0;  // HEADER_END 判定出的 Content-Length
    BodyFrame body_frame_    = BodyFrame::NONE;
    size_t   body_remaining_ = 0;       // SIZE 模式剩余字节

    bool     chunk_reading_size_ = true; // CHUNKED：正待读 size 行
    size_t   chunk_remaining_    = 0;    // CHUNKED：当前 chunk 剩余数据字节
    bool     chunk_data_crlf_    = false;// CHUNKED：数据后需读一个 CRLF
    bool     chunk_trailers_     = false;// CHUNKED：0-size chunk 后读 trailer 到空行

    bool     header_size_too_large_ = false; // header 超限标志

    HttpRequest request_;
    std::string doc_root_;   // 静态根目录（工厂注入）
    std::shared_ptr<HttpFsm<ConnectionType>> fsm_;
    std::atomic<int> state_{static_cast<int>(HttpState::WAIT_REQUEST_LINE)};
    HttpEvent next_event_{HttpEvent::REQUEST_LINE};

    // ── 帧式读的内部实现 ────────────────────────────────────────
    bool body_has_buffered_input() const;   // WAIT_BODY 专用判定
    std::string body_extract();             // WAIT_BODY 专用拆帧
    std::string take_line();                // 拆出一条 '\n' 行（去行尾 CRLF）

    // body 数据喂入（0=未收完, 1=畸形, 2=收完）
    int feed_body(const std::string& blob);
    void check_body_limit();                // 超限置 413 标志

    // ── 响应管线 ────────────────────────────────────────────────
    std::string build_header(int status, const std::string& status_text,
                             const std::string& content_type, int64_t content_length,
                             bool has_body, bool close);
    void send_simple(int status, std::string body, bool close);
    void send_file(const std::string& full_path, const std::string& content_type,
                   int64_t length, bool close, bool head_only);
    // 便携式分块读文件 → async_write（非零拷贝路径；Linux 见 sendfile 备注）
    void stream_file_body(std::shared_ptr<std::ifstream> ifs, int64_t remaining, bool close);
#ifdef __linux__
    // 零拷贝兜底：把文件 fd + socket fd 交给 worker 线程池跑纯 sendfile（阻塞
    // 系统调用），完成后 post 回 io 线程复位 keep-alive。仅限明文 TcpConnection
    // （sendfile 绕过 TLS 加密）。逻辑见 send_file 的 if constexpr 门控。
    void serve_file_sendfile(const std::string& full_path, int64_t length,
                             bool close, bool head_only, int sock_fd,
                             std::shared_ptr<mail_system::ThreadPoolBase> pool);
#endif
    // keep-alive / 断开收尾，在最后一次写完成后调用
    void finish_response(bool keep_alive);
};

using TcpHttpSession = HttpSession<mail_system::TcpConnection>;
using SslHttpSession = HttpSession<mail_system::SslConnection>;

} // namespace web_server

#endif // WEB_SERVER_HTTP_SESSION_H