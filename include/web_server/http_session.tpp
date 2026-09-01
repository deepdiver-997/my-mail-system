#ifndef WEB_SERVER_HTTP_SESSION_TPP
#define WEB_SERVER_HTTP_SESSION_TPP
// 依赖见实例化文件 include 顺序；本文件在实例化前需 http_parser.h 可见。
#include "web_server/http_parser.h"
#include "web_server/http_types.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#ifdef __linux__
#include <cerrno>
#include <fcntl.h>     // ::open / O_RDONLY
#include <poll.h>      // ::poll / pollfd
#include <type_traits> // std::is_same_v（sendfile 门控用）
#include <unistd.h>    // ::close
#endif

namespace web_server {

#ifdef __linux__
// 零拷贝 worker 任务：在 worker 线程阻塞式跑 sendfile，把文件 fd 内容直接灌进
// socket fd（内核态 disk→socket，无用户态拷贝）。sock 是 ASIO 设成非阻塞的，
// 发送缓冲区满时 sendfile 返回 EAGAIN → 用 poll(POLLOUT) 等可写再续（worker
// 线程容忍阻塞）。完成后由调用方负责 post 回 io 线程复位 keep-alive。
static void http_sendfile_worker(int in_fd, int out_fd, int64_t length) {
    size_t remaining = static_cast<size_t>(length);
    constexpr size_t kChunk = 128 * 1024;
    off_t off = 0;
    while (remaining > 0) {
        ssize_t n = ::sendfile(out_fd, in_fd, &off, kChunk);
        if (n > 0) { remaining -= static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{out_fd, POLLOUT, 0};
            ::poll(&pfd, 1, 1000);
            continue;
        }
        break;   // 出错（EPIPE/对端关闭）或 EOF → 截断
    }
    ::close(in_fd);
}
#endif

// ================================================================
// 生命周期
// ================================================================
template <typename ConnectionType>
HttpSession<ConnectionType>::HttpSession(mail_system::ServerBase* server,
    std::unique_ptr<ConnectionType> connection,
    std::shared_ptr<HttpFsm<ConnectionType>> fsm)
    : mail_system::SessionBase<ConnectionType>(std::move(connection), server)
    , fsm_(std::move(fsm))
{}

template <typename ConnectionType>
void HttpSession<ConnectionType>::start(std::shared_ptr<HttpSession> self) {
    self->set_current_state(static_cast<int>(HttpState::WAIT_REQUEST_LINE));
    self->set_next_event(static_cast<int>(HttpEvent::REQUEST_LINE));
    self->do_async_read();
}

// ================================================================
// 帧式读：has_buffered_input / extract_one_line 按当前状态分派
// ================================================================
template <typename ConnectionType>
bool HttpSession<ConnectionType>::has_buffered_input() const {
    switch (static_cast<HttpState>(state_.load(std::memory_order_acquire))) {
        case HttpState::WAIT_REQUEST_LINE:
        case HttpState::WAIT_HEADERS:
            return this->command_read_buffer_.find('\n') != std::string::npos;
        case HttpState::WAIT_BODY:
            return body_has_buffered_input();
        case HttpState::RESPOND:
        case HttpState::CLOSED:
        default:
            return false;   // 响应发出中/已关，不喂给循环
    }
}

template <typename ConnectionType>
std::string HttpSession<ConnectionType>::extract_one_line() {
    switch (static_cast<HttpState>(state_.load(std::memory_order_acquire))) {
        case HttpState::WAIT_REQUEST_LINE:
        case HttpState::WAIT_HEADERS:
            return take_line();
        case HttpState::WAIT_BODY:
            return body_extract();
        default:
            return {};
    }
}

// 拆出一条 '\n' 行（去行尾 \r，不含换行符）。
template <typename ConnectionType>
std::string HttpSession<ConnectionType>::take_line() {
    auto& buf = this->command_read_buffer_;
    size_t nl = buf.find('\n');
    if (nl == std::string::npos) return {};
    std::string s = buf.substr(0, nl);
    buf.erase(0, nl + 1);
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

// ── WAIT_BODY 帧判定 ────────────────────────────────────────────
template <typename ConnectionType>
bool HttpSession<ConnectionType>::body_has_buffered_input() const {
    auto& buf = this->command_read_buffer_;
    switch (body_frame_) {
        case BodyFrame::NONE:
            return false;
        case BodyFrame::SIZE:
            // 只在该取还差字节时判"有帧"；绝不把 body 之后的流水线字节当 body。
            return body_remaining_ > 0 && !buf.empty();
        case BodyFrame::CHUNKED:
            if (chunk_reading_size_ || chunk_trailers_)
                return buf.find('\n') != std::string::npos;
            if (chunk_data_crlf_)
                return buf.size() >= 2;               // 需读一个 CRLF
            return buf.size() >= chunk_remaining_;    // 需读够本 chunk 数据
    }
    return false;
}

// ── WAIT_BODY 拆帧（长度精确，绝不越界读进下一个请求）──────────
template <typename ConnectionType>
std::string HttpSession<ConnectionType>::body_extract() {
    auto& buf = this->command_read_buffer_;
    switch (body_frame_) {
        case BodyFrame::NONE:
            return {};
        case BodyFrame::SIZE: {
            size_t take = std::min(buf.size(), body_remaining_);
            std::string s = buf.substr(0, take);
            buf.erase(0, take);
            return s;
        }
        case BodyFrame::CHUNKED: {
            if (chunk_reading_size_ || chunk_trailers_)
                return take_line();                   // size 行 / trailer 行
            if (chunk_data_crlf_) {
                if (buf.size() < 2) return {};
                std::string c = buf.substr(0, 2);
                buf.erase(0, 2);
                return c;
            }
            size_t take = std::min(buf.size(), chunk_remaining_);
            std::string s = buf.substr(0, take);
            buf.erase(0, take);
            return s;
        }
    }
    return {};
}

// ================================================================
// handle_read / process_read：解析本单元 → 设 next_event_
// ================================================================
template <typename ConnectionType>
void HttpSession<ConnectionType>::handle_read(const std::string& data) {
    switch (static_cast<HttpState>(state_.load(std::memory_order_acquire))) {
        case HttpState::WAIT_REQUEST_LINE: {
            this->trace_append_inbound(data + "\r\n");
            bool ok = parse_request_line(data, request_);
            next_event_ = ok ? HttpEvent::REQUEST_LINE : HttpEvent::ERROR;
            break;
        }
        case HttpState::WAIT_HEADERS: {
            this->trace_append_inbound(data + "\r\n");
            if (data.empty()) {
                // 空行 → header 结束，归纳 body framing
                bool ok = determine_body_framing(request_,
                              body_chunked_, body_content_length_, body_pending_);
                next_event_ = ok ? HttpEvent::HEADER_END : HttpEvent::ERROR;
            } else if (header_size_too_large_) {
                next_event_ = HttpEvent::ERROR;
            } else if (!parse_header_line(data, request_)) {
                next_event_ = HttpEvent::ERROR;
            } else {
                next_event_ = HttpEvent::HEADER_LINE;
            }
            break;
        }
        case HttpState::WAIT_BODY: {
            auto r = feed_body(data);
            if (r == 2) next_event_ = HttpEvent::BODY_END;
            else if (r == 1) next_event_ = HttpEvent::ERROR;
            else next_event_ = HttpEvent::BODY;
            break;
        }
        default:
            next_event_ = HttpEvent::ERROR;
            break;
    }
}

template <typename ConnectionType>
void HttpSession<ConnectionType>::process_read() {
    auto* fsm = static_cast<HttpFsm<ConnectionType>*>(this->get_fsm());
    fsm->auto_process_event(this->shared_from_this());
}

// ================================================================
// body 装配子状态机
// ================================================================
template <typename ConnectionType>
void HttpSession<ConnectionType>::begin_body() {
    body_armed_ = true;
    if (body_chunked_) {
        body_frame_ = BodyFrame::CHUNKED;
        chunk_reading_size_ = true; chunk_remaining_ = 0;
        chunk_data_crlf_ = false; chunk_trailers_ = false;
    } else {
        body_frame_ = BodyFrame::SIZE;
        body_remaining_ = body_content_length_;
    }
}

// 解析 chunk size 行 "HEX[;ext]"。非法 → SIZE_MAX。
static size_t http_chunk_size_parse(const std::string& line) {
    size_t i = 0, n = 0;
    bool any = false;
    while (i < line.size()) {
        char c = line[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else break;                            // 遇到 ';'（扩展）或非法
        if (n > (SIZE_MAX - static_cast<size_t>(v)) / 16) return SIZE_MAX; // 溢出
        n = n * 16 + static_cast<size_t>(v);
        any = true;
        ++i;
    }
    if (!any) return SIZE_MAX;
    // ';' 之后的扩展忽略
    if (i < line.size() && line[i] != ';') return SIZE_MAX;
    return n;
}

// 返回：0=未收完(继续), 1=畸形(ERROR), 2=收完(BODY_END)
template <typename ConnectionType>
int HttpSession<ConnectionType>::feed_body(const std::string& blob) {
    if (body_frame_ == BodyFrame::SIZE) {
        if (request_.body.size() + blob.size() > kMaxRequestBody) return 1; // 413 → ERROR
        request_.body.append(blob);
        body_remaining_ = body_remaining_ > blob.size() ? body_remaining_ - blob.size() : 0;
        return body_remaining_ == 0 ? 2 : 0;
    }
    // CHUNKED
    if (chunk_trailers_) {
        // trailer 区：读到空行才算完
        return blob.empty() ? 2 : 0;
    }
    if (chunk_reading_size_) {
        size_t sz = http_chunk_size_parse(blob);
        if (sz == SIZE_MAX) return 1;
        if (sz == 0) { chunk_reading_size_ = false; chunk_trailers_ = true; return 0; }
        chunk_remaining_ = sz;
        chunk_reading_size_ = false;
        return 0;
    }
    if (chunk_data_crlf_) {
        chunk_data_crlf_ = false;
        chunk_reading_size_ = true;   // 下一段是 size 行
        return 0;
    }
    // chunk 数据
    if (request_.body.size() + blob.size() > kMaxRequestBody) return 1;
    request_.body.append(blob);
    chunk_remaining_ = chunk_remaining_ > blob.size() ? chunk_remaining_ - blob.size() : 0;
    if (chunk_remaining_ == 0) chunk_data_crlf_ = true;
    return 0;
}

template <typename ConnectionType>
void HttpSession<ConnectionType>::check_body_limit() {
    // 上限在 feed_body 内联检查，此处保留入口以对齐声明。
}

// ================================================================
// 响应管线
// ================================================================
template <typename ConnectionType>
std::string HttpSession<ConnectionType>::build_header(int status,
    const std::string& status_text, const std::string& content_type,
    int64_t content_length, bool /*has_body*/, bool close)
{
    std::string h = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
    h += "Content-Type: " + content_type + "\r\n";
    h += "Content-Length: " + std::to_string(content_length) + "\r\n";
    h += (close ? "Connection: close\r\n" : "Connection: keep-alive\r\n");
    h += "\r\n";
    return h;
}

template <typename ConnectionType>
void HttpSession<ConnectionType>::finish_response(bool keep_alive) {
    if (!this->connection_ || !this->connection_->is_open()) {
        this->set_current_state(static_cast<int>(HttpState::CLOSED));
        this->close();
        return;
    }
    if (!keep_alive) {
        this->set_current_state(static_cast<int>(HttpState::CLOSED));
        this->close();
        return;
    }
    // keep-alive：复位帧状态，处理流水线上的下一个请求
    request_.reset();
    body_armed_ = false; body_frame_ = BodyFrame::NONE; body_remaining_ = 0;
    chunk_reading_size_ = true; chunk_remaining_ = 0; chunk_data_crlf_ = false;
    chunk_trailers_ = false; body_pending_ = false; header_size_too_large_ = false;
    this->set_current_state(static_cast<int>(HttpState::WAIT_REQUEST_LINE));
    this->set_next_event(static_cast<int>(HttpEvent::REQUEST_LINE));
    this->drain_buffered_commands();
    if (!this->has_buffered_input() && !this->is_paused() && !this->is_closed())
        this->do_async_read();
}

template <typename ConnectionType>
void HttpSession<ConnectionType>::send_simple(int status, std::string body, bool close) {
    std::string head = build_header(status, HttpResponse::reason_phrase(status),
                                    "text/html; charset=utf-8",
                                    static_cast<int64_t>(body.size()), !body.empty(), close);
    std::string resp = head + body;
    auto self = self_ptr();
    this->do_async_write(resp,
        [self, close](std::shared_ptr<mail_system::SessionBase<ConnectionType>> s,
                      const boost::system::error_code& ec) mutable {
            if (ec) { self->handle_error(ec); return; }
            self->finish_response(!close);
        });
}

// 便携路径：读入用户缓冲再 async_write（在 macOS 上即默认路径）。
// Linux 部署机可在此换成 sendfile/io_uring 零拷贝（见类型头注释与 docs）。
template <typename ConnectionType>
void HttpSession<ConnectionType>::stream_file_body(
    std::shared_ptr<std::ifstream> ifs, int64_t remaining, bool close)
{
    constexpr size_t kChunk = 64 * 1024;
    if (remaining <= 0) { finish_response(!close); return; }
    char buf[kChunk];
    ifs->read(buf, static_cast<std::streamsize>(std::min<size_t>(kChunk, (size_t)remaining)));
    auto got = ifs->gcount();
    if (got <= 0) { finish_response(!close); return; }   // 读失败 → 截断连接
    auto data = std::make_shared<std::string>(buf, static_cast<size_t>(got));
    auto self = self_ptr();
    int64_t next = remaining - got;
    this->do_async_write(*data,
        [self, ifs, next, close](std::shared_ptr<mail_system::SessionBase<ConnectionType>> s,
                                 const boost::system::error_code& ec) mutable {
            if (ec) { self->handle_error(ec); return; }
            self->stream_file_body(ifs, next, close);
        });
}

// stream_file_body 递归声明为私有成员，需在 header 中声明。这里仅实现。
template <typename ConnectionType>
void HttpSession<ConnectionType>::send_file(const std::string& full_path,
    const std::string& content_type, int64_t length, bool close, bool head_only)
{
    std::string head = build_header(200, "OK", content_type, length, length > 0, close);
    auto self = self_ptr();

#ifdef __linux__
    // 零拷贝兜底：明文 TCP + 拿到原生 fd + 有 worker 池才走 sendfile。
    // `if constexpr` 保证 TLS(SslConnection) 与普通读流绝不经此路径——
    // sendfile 写裸 fd 会绕过 SSL 加密，只能用于 TcpConnection。
    if constexpr (std::is_same_v<ConnectionType, mail_system::TcpConnection>) {
        int sock_fd = this->get_connection().native_handle();
        auto pool = this->get_server() ? this->get_server()->m_workerThreadPool : nullptr;
        if (sock_fd >= 0 && pool) {
            serve_file_sendfile(full_path, length, close, head_only, sock_fd, std::move(pool));
            return;
        }
    }
#endif

    if (head_only || length == 0) {
        this->do_async_write(head,
            [self, close](std::shared_ptr<mail_system::SessionBase<ConnectionType>> s,
                          const boost::system::error_code& ec) mutable {
                if (ec) { self->handle_error(ec); return; }
                self->finish_response(!close);
            });
        return;
    }
    auto ifs = std::make_shared<std::ifstream>(full_path, std::ios::binary);
    if (!ifs || !ifs->is_open()) { send_simple(404, "Not Found", close); return; }
    this->do_async_write(head,
        [self, ifs, length, close](std::shared_ptr<mail_system::SessionBase<ConnectionType>> s,
                                   const boost::system::error_code& ec) mutable {
            if (ec) { self->handle_error(ec); return; }
            self->stream_file_body(ifs, length, close);
        });
}

#ifdef __linux__
template <typename ConnectionType>
void HttpSession<ConnectionType>::serve_file_sendfile(const std::string& full_path,
    int64_t length, bool close, bool head_only, int sock_fd,
    std::shared_ptr<mail_system::ThreadPoolBase> pool)
{
    std::filesystem::path fp(full_path);
    std::string ctype = mime_for_extension(fp.extension().string());
    std::string head = build_header(200, "OK", ctype, length, length > 0, close);
    auto self = self_ptr();
    if (head_only || length == 0) {
        this->do_async_write(head,
            [self, close](std::shared_ptr<mail_system::SessionBase<ConnectionType>> s,
                          const boost::system::error_code& ec) mutable {
                if (ec) { self->handle_error(ec); return; }
                self->finish_response(!close);
            });
        return;
    }
    auto exec = this->get_connection().get_executor();   // 复制 io 执行器，用于 post 回 io
    this->do_async_write(head,
        [self, full_path, length, close, sock_fd, pool, exec](
            std::shared_ptr<mail_system::SessionBase<ConnectionType>> s,
            const boost::system::error_code& ec) mutable {
            if (ec) { self->handle_error(ec); return; }
            int in_fd = ::open(full_path.c_str(), O_RDONLY);
            if (in_fd < 0) { self->send_simple(404, "Not Found", close); return; }
            bool keep_alive = !close;
            // sendfile 是阻塞系统调用 → 交给 worker 池；完成后 post 回 io 线程复位 keep-alive。
            pool->post([self, in_fd, sock_fd, length, keep_alive, exec] {
                http_sendfile_worker(in_fd, sock_fd, length);
                boost::asio::post(exec, [self, keep_alive] {
                    self->finish_response(keep_alive);
                });
            });
        });
}
#endif

template <typename ConnectionType>
void HttpSession<ConnectionType>::serve() {
    auto& req = request_;
    bool close = !req.keep_alive() || req.version == "HTTP/1.0";

    // 只认 GET / HEAD（静态服务器语义；POST 等回 405）
    if (req.method != "GET" && req.method != "HEAD") {
        send_simple(405, "<h1>405 Method Not Allowed</h1>", close);
        return;
    }

    // 路径解析 + 防穿越
    std::string full;
    if (!resolve_safe_path(doc_root_, req.path, full)) {
        send_simple(403, "<h1>403 Forbidden</h1>", close);
        return;
    }

    std::error_code ec;
    if (std::filesystem::is_directory(full, ec)) {
        full = (std::filesystem::path(full) / "index.html").string();   // 目录 → index
    }
    ec.clear();

    // 打开文件定长
    std::ifstream probe(full, std::ios::binary);
    if (!probe.good()) { send_simple(404, "<h1>404 Not Found</h1>", close); return; }
    probe.seekg(0, std::ios::end);
    auto length = static_cast<int64_t>(probe.tellg());
    probe.close();

    std::filesystem::path fp(full);
    std::string ctype = mime_for_extension(fp.extension().string());
    bool head_only = (req.method == "HEAD");
    send_file(full, ctype, length, close, head_only);
}

template <typename ConnectionType>
void HttpSession<ConnectionType>::send_error_and_close(int status) {
    send_simple(status, std::string("<h1>") + HttpResponse::reason_phrase(status) +
                        "</h1><p>Bad or malicious request.</p>", true);
}

// ================================================================
// SessionBase 覆写（桥）
// ================================================================
template <typename ConnectionType>
std::chrono::milliseconds HttpSession<ConnectionType>::compute_reply_delay() const {
    return std::chrono::milliseconds(0);
}

template <typename ConnectionType>
void* HttpSession<ConnectionType>::get_fsm() const { return fsm_.get(); }

template <typename ConnectionType>
void* HttpSession<ConnectionType>::get_context() { return &request_; }

template <typename ConnectionType>
void HttpSession<ConnectionType>::set_current_state(int state) {
    state_.store(state, std::memory_order_release);
}

template <typename ConnectionType>
void HttpSession<ConnectionType>::set_next_event(int event) {
    next_event_ = static_cast<HttpEvent>(event);
}

template <typename ConnectionType>
int HttpSession<ConnectionType>::get_current_state() const {
    return state_.load(std::memory_order_acquire);
}

template <typename ConnectionType>
int HttpSession<ConnectionType>::get_next_event() const {
    return static_cast<int>(next_event_);
}

template <typename ConnectionType>
std::string HttpSession<ConnectionType>::get_last_command_args() const {
    return request_.raw_target;
}

} // namespace web_server

#endif // WEB_SERVER_HTTP_SESSION_TPP