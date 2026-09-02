#ifndef WEB_SERVER_H2_SESSION_TPP
#define WEB_SERVER_H2_SESSION_TPP
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace web_server {
namespace h2 {

// ================================================================
// 生命周期
// ================================================================
template <typename ConnectionType>
H2Session<ConnectionType>::H2Session(mail_system::ServerBase* server,
                                     std::unique_ptr<ConnectionType> connection)
    : mail_system::SessionBase<ConnectionType>(std::move(connection), server)
{}

template <typename ConnectionType>
void H2Session<ConnectionType>::start(std::shared_ptr<H2Session> self) {
    self->set_current_state(static_cast<int>(ConnState::PREFACE));
    self->rearm(self->config_read_timeout());
    self->do_async_read();
}

// ================================================================
// 帧式读：postface 前按 24 字节魔数，之后按帧（9 字节头 + payload）
// ================================================================
template <typename ConnectionType>
bool H2Session<ConnectionType>::has_buffered_input() const {
    auto& buf = this->command_read_buffer_;
    if (!got_preface_)
        return buf.size() >= kMagicSize;
    if (buf.size() < kFrameHeaderSize) return false;
    return buf.size() >= kFrameHeaderSize + frame_length(buf);
}

template <typename ConnectionType>
std::string H2Session<ConnectionType>::extract_one_line() {
    auto& buf = this->command_read_buffer_;
    if (!got_preface_) {
        if (buf.size() < kMagicSize) return {};
        std::string m = buf.substr(0, kMagicSize);
        buf.erase(0, kMagicSize);
        return m;
    }
    if (buf.size() < kFrameHeaderSize) return {};
    uint32_t len = frame_length(buf);
    if (buf.size() < kFrameHeaderSize + len) return {};
    std::string frame = buf.substr(0, kFrameHeaderSize + len);
    buf.erase(0, kFrameHeaderSize + len);
    return frame;
}

// ================================================================
// handle_read：啃下这"一块"（前导或一帧）→ process_read 去复用
// ================================================================
template <typename ConnectionType>
void H2Session<ConnectionType>::handle_read(const std::string& data) {
    if (!got_preface_) { handle_preface(data); return; }
    if (h2::parse_frame(data, cur_frame_)) {
        frame_ready_ = true;
        this->rearm(this->config_read_timeout());   // 每帧续命（idle keep-alive 超时回收）
    } else {
        close_protocol(ErrorCode::FRAME_SIZE_ERROR);
    }
}

template <typename ConnectionType>
void H2Session<ConnectionType>::handle_preface(const std::string& data) {
    if (data.size() >= kMagicSize &&
        0 == data.compare(0, kMagicSize, kConnectionMagic)) {
        got_preface_ = true;
        set_current_state(static_cast<int>(ConnState::OPEN));
        this->rearm(this->config_read_timeout());
        // RFC 9113 §3.4：服务器前导 = 收到客户端魔数后发自己的 SETTINGS
        send_frame(build_settings_frame());
        // preface 已被本调用消费；回到读循环取下一帧
    } else {
        close_protocol(ErrorCode::PROTOCOL_ERROR);   // 非法前导
    }
}

// ================================================================
// process_read：去复用 cur_frame_ —— 连接级 vs 流级
// ================================================================
template <typename ConnectionType>
void H2Session<ConnectionType>::process_read() {
    if (frame_ready_) {
        frame_ready_ = false;
        if (get_current_state() != static_cast<int>(ConnState::CLOSED))
            dispatch_frame(cur_frame_);
    }
    // 关键：SessionBase::do_async_read 只发一次读、靠各协议续读。H2 用 send_frame
    // 队列发响应（不经 do_async_write 的续读钩子），且请求可能分多个 TCP 段到达——
    // 必须在本 "块" 处理完后重新 arm 读，否则只处理首段、连接挂死直到 watchdog 回收。
    // 若缓冲里还有完整帧，do_async_read() 内 has_buffered_input() 短路，不会叠读。
    if (!this->is_closed()) this->do_async_read();
}

template <typename ConnectionType>
void H2Session<ConnectionType>::dispatch_frame(const Frame& f) {
    if (f.stream_id == kNoStreamId)
        handle_connection_frame(f);
    else
        handle_stream_frame(f);
}

// ── 连接级帧（stream 0）──────────────────────────────────────
template <typename ConnectionType>
void H2Session<ConnectionType>::handle_connection_frame(const Frame& f) {
    switch (f.type) {
        case FrameType::SETTINGS:
            if (f.flags & flag::ACK) {
                // 对端 ACK 了我们的 SETTINGS — 无需动作
            } else {
                apply_peer_settings(f.payload);
                send_frame(build_settings_ack());       // 必须回 ACK
            }
            break;
        case FrameType::PING:
            send_frame(build_ping_ack(f.payload));      // 回 PING ACK
            break;
        case FrameType::GOAWAY:
            set_current_state(static_cast<int>(ConnState::CLOSED));
            this->close();
            break;
        case FrameType::WINDOW_UPDATE: {
            // 连接级发送窗口↑；先 GC 已完结流，再给有 pending 的流续发 DATA
            uint32_t inc = f.payload.size() >= 4
                ? ((uint32_t((uint8_t)f.payload[0]) & 0x7f) << 24 |
                   (uint32_t((uint8_t)f.payload[1]) << 16) |
                   (uint32_t((uint8_t)f.payload[2]) << 8) |
                   (uint32_t((uint8_t)f.payload[3]))) : 0;
            if (inc) conn_send_window_ += (int32_t)inc;
            prune_streams();
            for (auto& kv : streams_) {
                H2Stream& s = kv.second;
                if (!s.pending_body.empty()) drain_stream(s);
            }
            break;
        }
        default:
            close_protocol(ErrorCode::PROTOCOL_ERROR);
            break;
    }
}

// ── 流级帧（stream>0）：流控类 session 自处理；请求装配/去复用交 codec ──
template <typename ConnectionType>
void H2Session<ConnectionType>::handle_stream_frame(const Frame& f) {
    // 流控/资源类帧：session 自处理
    if (f.type == FrameType::RST_STREAM) {
        codec_.abort_stream(f.stream_id);                // 清装配态
        streams_.erase(f.stream_id);                     // 清流控态
        return;
    }
    if (f.type == FrameType::WINDOW_UPDATE) {
        auto it = streams_.find(f.stream_id);
        if (it == streams_.end()) return;                // 未知/已关流的 WU → 规范上忽略
        uint32_t inc = f.payload.size() >= 4
            ? ((uint32_t((uint8_t)f.payload[0]) & 0x7f) << 24 |
               (uint32_t((uint8_t)f.payload[1]) << 16) |
               (uint32_t((uint8_t)f.payload[2]) << 8) |
               (uint32_t((uint8_t)f.payload[3]))) : 0;
        if (inc) it->second.send_window += (int32_t)inc;
        if (!it->second.pending_body.empty()) drain_stream(it->second);
        return;
    }
    if (f.type == FrameType::PRIORITY) return;           // 影响调度优先级，本骨架忽略

    // HEADERS 记录最近客户流（GOAWAY 用）；单调/Q门控在 codec 内校验
    if (f.type == FrameType::HEADERS) last_client_stream_ = (int32_t)f.stream_id;

    // 请求装配/去复用/HPACK 归 codec：HEADERS/CONTINUATION/DATA → 可能产出归一化请求
    auto cr = codec_.feed((int32_t)f.stream_id, f.type, f.flags, f.payload);
    if (!cr.protocol_ok) { close_protocol(cr.error); return; }

    prune_streams();                                     // 安全点 GC 已完结流
    auto& st = streams_[f.stream_id];                    // 惰性建【流控】流
    st.id = f.stream_id;
    if (st.state == StreamState::IDLE) {
        st.state = StreamState::OPEN;
        st.send_window = peer_stream_window_;            // 新流发送窗口 = 对端广告值
    }
    if (f.type == FrameType::DATA) st.body += f.payload; // 静态 GET 无体，仅占位累积
    if (cr.request && !st.served) {
        st.served = true;
        serve_stream(st, *cr.request);
    }
}

// ── 响应：MessageProcessor(纯路由) → HEADERS+分片 DATA(流控) ──
// req 已由 codec 归一化（wire→message）；本函数只做"请求→响应 + 发送"，不碰帧/头装配。
template <typename ConnectionType>
void H2Session<ConnectionType>::serve_stream(H2Stream& st, const web_server::HttpRequest& req) {
    web_server::HttpResponse resp = web_server::process_static_request(req, doc_root_);

    bool head_only = (req.method == "HEAD");
    std::vector<Header> hdrs;
    hdrs.push_back({":status", std::to_string(resp.status)});
    if (!resp.content_type.empty()) hdrs.push_back({"content-type", resp.content_type});
    hdrs.push_back({"content-length", std::to_string(resp.content_length)});
    if (resp.status != 200) {
        // 内联错误体随 DATA 送出：只发头而 content-length≠0，H2 客户端会判 DATA 未闭合
        // = PROTOCOL_ERROR（HEAD 是规范豁免除外）。
        send_response(st, std::move(hdrs), std::move(resp.body), /*head_only=*/false);
        return;
    }
    if (head_only) { send_response(st, std::move(hdrs), "", true); return; }  // HEAD：光发头
    // 200 且非 HEAD：读整个文件到内存（H2 靠流控把大 body 分片成多个 DATA 帧发给对端）
    std::ifstream in(resp.full_path, std::ios::binary | std::ios::ate);
    if (!in.good()) { send_response(st, {{":status", "404"}}, "not found\n", false); return; }
    size_t fsz = (size_t)in.tellg();
    in.seekg(0);
    std::string body(fsz, '\0');
    if (fsz) in.read(&body[0], (std::streamsize)fsz);
    send_response(st, std::move(hdrs), std::move(body), false);
}

// 发 HEADERS + (body) DATA；body 过大/窗口不足时分段等流控续发
template <typename ConnectionType>
void H2Session<ConnectionType>::send_response(H2Stream& st, std::vector<Header> resp,
                                              std::string body, bool head_only) {
    std::string block = web_server::h2::encode_response_headers(resp);
    if (head_only || body.empty()) {
        send_frame(build_headers(st.id, block, /*end_stream=*/true));
        st.state = StreamState::CLOSED;
        st.done = true;                          // 不再立即 erase（避免悬垂引用）
        return;
    }
    send_frame(build_headers(st.id, block, /*end_stream=*/false));
    st.pending_body = std::move(body);
    drain_stream(st);
}

// 按 min(流窗口, 连接窗口) 分段发 DATA；发完 END_STREAM 并关流
template <typename ConnectionType>
void H2Session<ConnectionType>::drain_stream(H2Stream& st) {
    while (!st.pending_body.empty()) {
        uint32_t avail = available_window(st);
        if (avail == 0) return;                       // 窗口耗尽，等 WINDOW_UPDATE
        size_t n = std::min((size_t)avail, st.pending_body.size());
        if (n > max_frame_size_) n = max_frame_size_; // 单帧不得超过对端最大帧大小
        std::string chunk = st.pending_body.substr(0, n);
        st.pending_body.erase(0, n);
        st.send_window -= (int32_t)n;
        conn_send_window_ -= (int32_t)n;
        if (st.pending_body.empty()) {
            send_frame(build_data(st.id, chunk, /*end_stream=*/true));
            st.state = StreamState::CLOSED;
            st.done = true;                           // 完成，待安全点 GC
            return;
        }
        send_frame(build_data(st.id, chunk, false));
    }
}

template <typename ConnectionType>
void H2Session<ConnectionType>::prune_streams() {
    // 只从未持有的安全点调用（handle_stream_frame 入口 / WINDOW_UPDATE 前），
    // 避免在持有 H2Stream& 的 serve/drain 路径中被该引用再 erase（UAF 源）。
    for (auto it = streams_.begin(); it != streams_.end();) {
        if (it->second.done) it = streams_.erase(it);
        else ++it;
    }
}

template <typename ConnectionType>
uint32_t H2Session<ConnectionType>::available_window(const H2Stream& st) const {
    if (st.send_window <= 0 || conn_send_window_ <= 0) return 0;
    return (uint32_t)std::min(st.send_window, conn_send_window_);
}

// ── 对端 SETTINGS 应用（骨架：只解析初始流窗口，其余忽略）─────
template <typename ConnectionType>
void H2Session<ConnectionType>::apply_peer_settings(const std::string& payload) {
    for (size_t i = 0; i + 6 <= payload.size(); i += 6) {
        uint16_t key = uint16_t(uint8_t(payload[i])) << 8 | uint16_t(uint8_t(payload[i + 1]));
        uint32_t val = (uint32_t(uint8_t(payload[i + 2])) << 24) |
                       (uint32_t(uint8_t(payload[i + 3])) << 16) |
                       (uint32_t(uint8_t(payload[i + 4])) << 8) |
                       (uint32_t(uint8_t(payload[i + 5])));
        switch (static_cast<SettingsId>(key)) {
            case SettingsId::INITIAL_WINDOW_SIZE: {
                // 对端给我方的每流发送窗口（RFC 9113 §6.5.2）：排他地管"流级"流控，
                // 与连接窗无关。改动需按 delta 调整所有在建流（RFC 9113 §6.9.2）。
                int32_t delta = (int32_t)val - peer_stream_window_;
                peer_stream_window_ = (int32_t)val;
                for (auto& kv : streams_) kv.second.send_window += delta;
                break;
            }
            case SettingsId::HEADER_TABLE_SIZE:
                codec_.apply_header_table_size(val);       // 解码器归 codec
                break;
            case SettingsId::MAX_FRAME_SIZE:
                // 对端允许的最大帧大小（决定我们 DATA 单帧上限）
                if (val >= 16384 && val <= (1u << 24) - 1) max_frame_size_ = val;
                break;
            default: break;
        }
    }
}

// ── 协议错误：GOAWAY(<=last) 然后关 ──────────────────────────
template <typename ConnectionType>
void H2Session<ConnectionType>::close_protocol(ErrorCode code) {
    if (get_current_state() == static_cast<int>(ConnState::CLOSED)) return;
    set_current_state(static_cast<int>(ConnState::CLOSED));
    send_frame(build_goaway(last_client_stream_, code));
    this->streams_.clear();
    this->close();
}

// ================================================================
// 出站帧队列（串行发送；不依赖 SessionBase 请求-响应写缓冲）
// ================================================================
template <typename ConnectionType>
void H2Session<ConnectionType>::send_frame(const std::string& wire_frame) {
    out_pending_ += wire_frame;
    if (!out_flushing_) flush_out();
}

template <typename ConnectionType>
void H2Session<ConnectionType>::flush_out() {
    if (out_pending_.empty()) { out_flushing_ = false; return; }
    out_flushing_ = true;
    auto data = std::make_shared<std::string>(std::move(out_pending_));
    out_pending_.clear();
    auto self = self_ptr();
    this->connection_->async_write(boost::asio::buffer(*data),
        [self, data](const boost::system::error_code& ec, std::size_t) mutable {
            self->out_flushing_ = false;
            if (ec) { self->handle_error(ec); return; }
            self->flush_out();
        });
}

template <typename ConnectionType>
std::chrono::milliseconds H2Session<ConnectionType>::compute_reply_delay() const {
    return std::chrono::milliseconds(0);
}

} // namespace h2
} // namespace web_server

#endif // WEB_SERVER_H2_SESSION_TPP