#ifndef WEB_SERVER_H2_FRAMER_H
#define WEB_SERVER_H2_FRAMER_H
// ──────────────────────────────────────────────────────────────────
// HTTP/2 帧读写（纯函数，不碰 session，便于单测：字节流 <-> Frame）。
// ──────────────────────────────────────────────────────────────────
#include "web_server/h2/h2_types.hpp"
#include <cstdint>
#include <string>

namespace web_server {
namespace h2 {

// 从 9 字节头读帧 payload 长度（24-bit big-endian）
uint32_t frame_length(const std::string& raw);

// 解析一帧（raw 必须 >= 9 + length 字节）。返回 false = 帧头非法/不完整。
bool parse_frame(const std::string& raw, Frame& out);

// 序列化一帧成 wire 字节（9 字节头 + payload）
std::string serialize_frame(FrameType type, uint8_t flags, uint32_t stream_id,
                            const std::string& payload);

// ── 常用帧构造 ──────────────────────────────────────────────────
// 服务器连接前导的 SETTINGS（非 ACK）：max_concurrent_streams + initial_window
std::string build_settings_frame();
std::string build_settings_ack();
std::string build_ping_ack(const std::string& opaque8);
// stream_id 上发 END_STREAM 的空 DATA
std::string build_data(uint32_t stream_id, const std::string& body, bool end_stream);
// 一个 HEADERS 帧（payload = HPACK 编码的 header block）
std::string build_headers(uint32_t stream_id, const std::string& block, bool end_stream = false);
// 一个最小响应头块：静态表索引 8 = `:status: 200`（单字节 0x88）
std::string build_headers_200(uint32_t stream_id, bool end_stream = false);
std::string build_goaway(uint32_t last_stream, ErrorCode code);
std::string build_window_update(uint32_t stream_id, uint32_t increment);

} // namespace h2
} // namespace web_server

#endif // WEB_SERVER_H2_FRAMER_H