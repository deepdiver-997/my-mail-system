#include "web_server/h2/h2_framer.h"

namespace web_server {
namespace h2 {

static uint32_t be24(const std::string& s, size_t off) {
    return (uint32_t((uint8_t)s[off]) << 16) |
           (uint32_t((uint8_t)s[off + 1]) << 8) |
           (uint32_t((uint8_t)s[off + 2]));
}
static std::string u24(uint32_t v) {
    return {char((v >> 16) & 0xff), char((v >> 8) & 0xff), char(v & 0xff)};
}
static uint32_t be31(const std::string& s, size_t off) {
    return (uint32_t((uint8_t)s[off]) & 0x7f) << 24 |
           uint32_t((uint8_t)s[off + 1]) << 16 |
           uint32_t((uint8_t)s[off + 2]) << 8 |
           uint32_t((uint8_t)s[off + 3]);
}

uint32_t frame_length(const std::string& raw) {
    if (raw.size() < kFrameHeaderSize) return 0;
    return be24(raw, 0);
}

bool parse_frame(const std::string& raw, Frame& out) {
    if (raw.size() < kFrameHeaderSize) return false;
    uint32_t len = be24(raw, 0);
    if (raw.size() < kFrameHeaderSize + len) return false;   // payload 未到齐
    out.type = static_cast<FrameType>(raw[3]);
    out.flags = static_cast<uint8_t>(raw[4]);
    out.stream_id = be31(raw, 5);
    out.payload.assign(raw, kFrameHeaderSize, len);
    return true;
}

std::string serialize_frame(FrameType type, uint8_t flags, uint32_t stream_id,
                            const std::string& payload) {
    std::string s;
    s.reserve(kFrameHeaderSize + payload.size());
    s += u24(static_cast<uint32_t>(payload.size()));
    s += char(static_cast<uint8_t>(type));
    s += char(flags);
    s += char((stream_id >> 24) & 0x7f);
    s += char((stream_id >> 16) & 0xff);
    s += char((stream_id >> 8) & 0xff);
    s += char(stream_id & 0xff);
    s += payload;
    return s;
}

std::string build_settings_frame() {
    // SETTINGS_MAX_CONCURRENT_STREAMS=128,  SETTINGS_INITIAL_WINDOW_SIZE=1048576(>= 默认且含推送? 保守 65535 亦可)
    // 这里广告：MAX_CONCURRENT_STREAMS=128, INITIAL_WINDOW_SIZE=65535（省得管复杂流控）
    std::string p;
    auto add = [&](uint16_t id, uint32_t v) {
        p += char((id >> 8) & 0xff); p += char(id & 0xff);
        p += char((v >> 24) & 0xff); p += char((v >> 16) & 0xff);
        p += char((v >> 8) & 0xff);  p += char(v & 0xff);
    };
    add(static_cast<uint16_t>(SettingsId::MAX_CONCURRENT_STREAMS), 128u);
    add(static_cast<uint16_t>(SettingsId::INITIAL_WINDOW_SIZE),    65535u);
    return serialize_frame(FrameType::SETTINGS, 0, kNoStreamId, p);
}

std::string build_settings_ack() {
    return serialize_frame(FrameType::SETTINGS, flag::ACK, kNoStreamId, "");
}

std::string build_ping_ack(const std::string& opaque8) {
    std::string p = opaque8; p.resize(8, '\0');
    return serialize_frame(FrameType::PING, flag::ACK, kNoStreamId, p);
}

std::string build_data(uint32_t stream_id, const std::string& body, bool end_stream) {
    uint8_t f = end_stream ? flag::END_STREAM : 0;
    return serialize_frame(FrameType::DATA, f, stream_id, body);
}

std::string build_headers(uint32_t stream_id, const std::string& block, bool end_stream) {
    uint8_t f = flag::END_HEADERS | (end_stream ? flag::END_STREAM : 0);
    return serialize_frame(FrameType::HEADERS, f, stream_id, block);
}

std::string build_headers_200(uint32_t stream_id, bool end_stream) {
    // HPACK 静态表索引 8 = `:status: 200` → 编码为字面值 0x88
    std::string block(1, char(0x88));
    return build_headers(stream_id, block, end_stream);
}

std::string build_goaway(uint32_t last_stream, ErrorCode code) {
    std::string p;
    p += char((last_stream >> 24) & 0x7f);
    p += char((last_stream >> 16) & 0xff);
    p += char((last_stream >> 8) & 0xff);
    p += char(last_stream & 0xff);
    p += char((uint32_t(code) >> 24) & 0xff);
    p += char((uint32_t(code) >> 16) & 0xff);
    p += char((uint32_t(code) >> 8) & 0xff);
    p += char(uint32_t(code) & 0xff);
    return serialize_frame(FrameType::GOAWAY, 0, kNoStreamId, p);
}

std::string build_window_update(uint32_t stream_id, uint32_t increment) {
    std::string p;
    p += char((increment >> 24) & 0x7f);
    p += char((increment >> 16) & 0xff);
    p += char((increment >> 8) & 0xff);
    p += char(increment & 0xff);
    return serialize_frame(FrameType::WINDOW_UPDATE, 0, stream_id, p);
}

} // namespace h2
} // namespace web_server