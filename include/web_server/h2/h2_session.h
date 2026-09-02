#ifndef WEB_SERVER_H2_SESSION_H
#define WEB_SERVER_H2_SESSION_H
// ──────────────────────────────────────────────────────────────────
// H2Session — HTTP/2 多流会话模型核心
//
// 结构断点：HTTP/1.1 是"一个 SessionBase 一个 FSM 串行"；本类把 SessionBase
// 的"读"改成**帧原子**（has_buffered_input/extract_one_line 按帧拆，不再按行），
// 并在其上维护：
//   连接级生命周期（preface→SETTINGS→OPEN→CLOSED）
//   流注册表 std::map<int32_t, H2Stream>  —— N 条流复用一根连接
//   帧按 stream_id 去复用：==0 走连接级，>0 路由到对应流的独立状态机
// HPACK 解码是独立 codec：这里把原始 header block 存进流，解码落下一个组件。
// 出站帧不依赖 SessionBase 的"请求-响应"写缓冲（那条会给 H2 造成响应挂起），
// 用本连接专用 send_frame 队列串行发。
// ──────────────────────────────────────────────────────────────────
#include "framework/session_base.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "mail_system/back/common/logger.h"
#include "web_server/h2/h2_types.hpp"
#include "web_server/h2/h2_framer.h"
#include "web_server/h2/h2_hpack.h"
#include "web_server/http_parser.h"     // resolve_safe_path / mime_for_extension
#include "web_server/http_types.hpp"    // mime_for_extension
#include "web_server/message_processor.h"  // process_static_request（纯 request→response）
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace web_server {
namespace h2 {

enum class ConnState : int {
    PREFACE = 0,   // 等客户端魔法前导
    OPEN    = 1,
    CLOSED  = 2,
};

template <typename ConnectionType>
class H2Session : public mail_system::SessionBase<ConnectionType> {
public:
    H2Session(mail_system::ServerBase* server,
              std::unique_ptr<ConnectionType> connection);
    ~H2Session() override = default;

    static void start(std::shared_ptr<H2Session> self);
    static void start_after_starttls(std::shared_ptr<H2Session> self) { start(std::move(self)); }

    // ── SessionBase 覆写：帧式读 ────────────────────────────────
    void handle_read(const std::string& data) override;
    void process_read() override;
    bool has_buffered_input() const override;
    std::string extract_one_line() override;
    std::chrono::milliseconds compute_reply_delay() const override;
    void* get_fsm() const override { return nullptr; }
    void* get_context() override { return &streams_; }
    void set_current_state(int s) override { conn_state_.store(s, std::memory_order_release); }
    void set_next_event(int) override {}
    int  get_current_state() const override { return conn_state_.load(std::memory_order_acquire); }
    int  get_next_event() const override { return 0; }
    std::string get_last_command_args() const override { return {}; }

public:
    // 静态根目录（工厂注入）
    void set_doc_root(std::string d) { doc_root_ = std::move(d); }

    // 出站帧队列：串行发送，避免 SessionBase 请求-响应写缓冲把 H2 帧滞留。
    void send_frame(const std::string& wire_frame);

private:
    std::shared_ptr<H2Session<ConnectionType>> self_ptr() {
        return std::static_pointer_cast<H2Session<ConnectionType>>(this->shared_from_this());
    }

    void handle_preface(const std::string& data);
    void flush_out();
    // 帧去复用：连接级 vs 流级
    void dispatch_frame(const Frame& f);
    void handle_connection_frame(const Frame& f);
    void handle_stream_frame(const Frame& f);
    void serve_stream(H2Stream& st);
    void send_response(H2Stream& st, std::vector<Header> resp, std::string body, bool head_only);
    void drain_stream(H2Stream& st);       // 按流/连接窗口发 DATA 余量
    void prune_streams();                  // 移除标记 done 的流（安全点 GC，不在遍历中删）
    uint32_t available_window(const H2Stream& st) const;
    void apply_peer_settings(const std::string& payload);
    void close_protocol(ErrorCode code);

    std::atomic<int> conn_state_{static_cast<int>(ConnState::PREFACE)};
    bool got_preface_ = false;
    bool frame_ready_ = false;
    Frame cur_frame_;
    std::map<int32_t, H2Stream> streams_;
    int32_t last_client_stream_ = 0;   // 校验流 ID 单调递增（客户端开奇流）

    // HTTP/2 语义：连接级 HPACK 解码器（动态表跨流共享）+ 连接级发送窗口 + 静态根
    HeaderDecoder decoder_;
    std::string   doc_root_;
    int32_t       conn_send_window_ = static_cast<int32_t>(kDefaultWindow);

    // 出站队列
    std::string out_pending_;
    bool        out_flushing_ = false;
};

using TcpH2Session = H2Session<mail_system::TcpConnection>;
using SslH2Session = H2Session<mail_system::SslConnection>;

} // namespace h2
} // namespace web_server

#endif // WEB_SERVER_H2_SESSION_H