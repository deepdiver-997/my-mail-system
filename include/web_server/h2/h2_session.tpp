#ifndef WEB_SERVER_H2_SESSION_TPP
#define WEB_SERVER_H2_SESSION_TPP
#include <algorithm>
#include <chrono>
#include <cstring>

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
    if (!frame_ready_) return;                       // 刚啃 preface / 无事可做
    frame_ready_ = false;
    if (get_current_state() == static_cast<int>(ConnState::CLOSED)) return;
    dispatch_frame(cur_frame_);
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
        case FrameType::WINDOW_UPDATE:
            // 连接级发送窗口增加（本骨架响应极小，不实际记账）
            break;
        default:
            close_protocol(ErrorCode::PROTOCOL_ERROR);
            break;
    }
}

// ── 流级帧（stream>0）：路由到 map<stream_id, H2Stream> ──────
template <typename ConnectionType>
void H2Session<ConnectionType>::handle_stream_frame(const Frame& f) {
    // 客户端必须用单调递增的奇流 ID 开流；非法 → 连接级错误
    if ((f.stream_id & 1) == 0 || f.stream_id <= last_client_stream_) {
        close_protocol(ErrorCode::PROTOCOL_ERROR);
        return;
    }
    last_client_stream_ = f.stream_id;

    if (f.type == FrameType::RST_STREAM) {
        streams_.erase(f.stream_id);                     // 对方主动掐流
        return;
    }

    auto& st = streams_[f.stream_id];                    // 惰性建流
    st.id = f.stream_id;
    if (st.state == StreamState::IDLE) st.state = StreamState::OPEN;

    switch (f.type) {
        case FrameType::HEADERS:
        case FrameType::CONTINUATION:
            st.header_block += f.payload;                // 原始 HPACK 存流上（解码留给 codec）
            if (f.flags & flag::END_HEADERS) st.headers_done = true;
            if (f.flags & flag::END_STREAM) { st.end_stream_received = true; st.state = StreamState::HALF_CLOSED_REMOTE; }
            if (st.headers_done && st.end_stream_received) serve_stream(st);
            break;
        case FrameType::DATA:
            st.body += f.payload;
            if (f.flags & flag::END_STREAM) {
                st.end_stream_received = true;
                st.state = StreamState::HALF_CLOSED_REMOTE;
                serve_stream(st);
            }
            break;
        case FrameType::WINDOW_UPDATE:
            if (f.payload.size() >= 4) {
                uint32_t inc = (uint32_t((uint8_t)f.payload[0]) & 0x7f) << 24 |
                               uint32_t((uint8_t)f.payload[1]) << 16 |
                               uint32_t((uint8_t)f.payload[2]) << 8 |
                               uint32_t((uint8_t)f.payload[3]);
                if (inc != 0) st.send_window += (int32_t)inc;
            }
            break;
        case FrameType::PRIORITY:
            break;   // 影响调度优先级，本骨架忽略
        default:
            close_protocol(ErrorCode::PROTOCOL_ERROR);
            break;
    }
}

// ── 响应：HEADERS(:status 200) + DATA(END_STREAM)，然后关流 ──
template <typename ConnectionType>
void H2Session<ConnectionType>::serve_stream(H2Stream& st) {
    // （此处可解析 st.header_block 的 :path 去路由静态文件；HPACK 解码未接，先固定回 200）
    send_frame(build_headers_200(st.id, /*end_stream=*/false));
    std::string body = "HTTP/2 stream " + std::to_string(st.id) +
                       " (multiplexed on one connection) ok\n";
    send_frame(build_data(st.id, body, /*end_stream=*/true));
    st.state = StreamState::CLOSED;
    streams_.erase(st.id);   // 流已结束，回收（骨架直接删，不做保留观察）
}

// ── 对端 SETTINGS 应用（骨架：只解析初始流窗口，其余忽略）─────
template <typename ConnectionType>
void H2Session<ConnectionType>::apply_peer_settings(const std::string& payload) {
    for (size_t i = 0; i + 6 <= payload.size(); i += 6) {
        uint16_t key = uint16_t((uint8_t)payload[i]) << 8 | uint16_t((uint8_t)payload[i + 1]);
        uint32_t val = (uint32_t((uint8_t)payload[i + 2]) << 24) |
                       (uint32_t((uint8_t)payload[i + 3]) << 16) |
                       (uint32_t((uint8_t)payload[i + 4]) << 8) |
                       (uint32_t((uint8_t)payload[i + 5]));
        (void)val;
        switch (static_cast<SettingsId>(key)) {
            case SettingsId::INITIAL_WINDOW_SIZE:
                // 更新今后新建流的发送窗口（现有流按协议需调整 delta，骨架从简）
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