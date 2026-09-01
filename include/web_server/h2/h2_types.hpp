#ifndef WEB_SERVER_H2_TYPES_HPP
#define WEB_SERVER_H2_TYPES_HPP
// ──────────────────────────────────────────────────────────────────
// HTTP/2 多流会话模型 — 类型定义
//
// 结构断点（相对 HTTP/1.1）：不是"多加状态"，而是从"每连接一个 FSM"变成
//   连接级 FSM（帧定界/SETTINGS/GOAWAY/流控窗口  +  N 条流的复用）
//   每流 FSM（idle→open→half-close→closed，HEADERS/DATA/RST 按流 ID 路由）
// 本文件只放类型与常量；帧读写见 h2_framer，会话/流注册表见 h2_session。
// HPACK 解码是独立 codec，这里留 raw header block 槽位，后续单独接。
// ──────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>

namespace web_server {
namespace h2 {

// 连接前导魔数（RFC 9113 §3.4）— 客户端发的固定 24 字节
inline constexpr char kConnectionMagic[] =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr size_t kMagicSize = 24;
inline constexpr size_t kFrameHeaderSize = 9;   // len(24) + type(8) + flags(8) + stream(31)

inline constexpr uint32_t kDefaultWindow = 65535;   // 初始流/连接发送窗口
inline constexpr uint32_t kNoStreamId   = 0;        // 连接级帧的 stream_id

// ── 帧类型（RFC 9113 §6）─────────────────────────────────────────
enum class FrameType : uint8_t {
    DATA = 0x0,
    HEADERS = 0x1,
    PRIORITY = 0x2,
    RST_STREAM = 0x3,
    SETTINGS = 0x4,
    PUSH_PROMISE = 0x5,
    PING = 0x6,
    GOAWAY = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION = 0x9,
};

// ── 帧标志（按帧类型复用位）─────────────────────────────────────
namespace flag {
    // HEADERS / DATA / CONTINUATION
    inline constexpr uint8_t END_STREAM      = 0x01;
    // HEADERS
    inline constexpr uint8_t END_HEADERS     = 0x04;
    inline constexpr uint8_t PADDED          = 0x08;
    // HEADERS / PRIORITY
    inline constexpr uint8_t PRIORITY        = 0x20;
    // SETTINGS / PING
    inline constexpr uint8_t ACK             = 0x01;
}

// ── 错误码（RFC 9113 §7）─────────────────────────────────────────
enum class ErrorCode : uint32_t {
    NO_ERROR = 0x0,
    PROTOCOL_ERROR = 0x1,
    INTERNAL_ERROR = 0x2,
    FLOW_CONTROL_ERROR = 0x3,
    SETTINGS_TIMEOUT = 0x4,
    STREAM_CLOSED = 0x5,
    FRAME_SIZE_ERROR = 0x6,
    REFUSED_STREAM = 0x7,
    CANCEL = 0x8,
    COMPRESSION_ERROR = 0x9,
    CONNECT_ERROR = 0xa,
    ENHANCE_YOUR_CALM = 0xb,
    INADEQUATE_SECURITY = 0xc,
    HTTP_1_1_REQUIRED = 0xd,
};

// ── SETTINGS 标识符（RFC 9113 §6.5.2）──────────────────────────
enum class SettingsId : uint16_t {
    HEADER_TABLE_SIZE = 0x1,
    ENABLE_PUSH = 0x2,
    MAX_CONCURRENT_STREAMS = 0x3,
    INITIAL_WINDOW_SIZE = 0x4,
    MAX_FRAME_SIZE = 0x5,
    MAX_HEADER_LIST_SIZE = 0x6,
};

// ── 一帧（已装配）───────────────────────────────────────────────
struct Frame {
    FrameType type{};
    uint8_t flags{};
    uint32_t stream_id{0};
    std::string payload;
};

// ── 每流状态机（RFC 9113 §5.1，一端视角）────────────────────────
enum class StreamState {
    IDLE,                // 尚未开启（收到第一个 HEADERS 才 idle→open）
    OPEN,                // 双向开放
    HALF_CLOSED_LOCAL,   // 我方已 END_STREAM
    HALF_CLOSED_REMOTE,  // 对端已 END_STREAM
    CLOSED,
};

// ── 一条流：状态 + 请求装配 + 流控窗口 ───────────────────────────
struct H2Stream {
    int32_t   id = 0;
    StreamState state = StreamState::IDLE;
    bool      end_stream_received = false;      // 对端 END_STREAM 已到
    bool      headers_done = false;             // END_HEADERS 已到
    std::string header_block;                    // 原始 HPACK 字节（解码留槽位）
    std::string body;                            // DATA 累积（本例 GET 无 body）
    // 本流我方发送窗口（对端给我们的信用）；仅发的 DATA 消耗它
    int32_t send_window = static_cast<int32_t>(kDefaultWindow);
};

} // namespace h2
} // namespace web_server

#endif // WEB_SERVER_H2_TYPES_HPP