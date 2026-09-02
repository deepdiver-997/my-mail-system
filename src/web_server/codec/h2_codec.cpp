#include "web_server/codec/h2_codec.h"

namespace web_server {
namespace codec {

bool H2Codec::stream_start_valid(int32_t id) {
    if ((id & 1) == 0) return false;                     // 偶数流 ID 不可能是客户端开的流
    if (id <= last_client_stream_) return false;         // 非单调递增重复开流
    last_client_stream_ = id;
    return true;
}

// 剥掉 HEADERS 里不在 HPACK 中的前缀：PRIORITY(5B) 与 PADDED(1B长度+尾部填充)。
// 返回 false = 前缀越界/非法 → PROTOCOL_ERROR。
static bool strip_headers_prefix(uint8_t flags, const std::string& payload, std::string& out) {
    size_t off = 0;
    if (flags & h2::flag::PRIORITY) off += 5;
    if (flags & h2::flag::PADDED) {
        if (payload.size() <= off) return false;
        uint8_t padlen = (uint8_t)payload[off++];
        if (payload.size() < off + padlen) return false;
        out = payload.substr(off, payload.size() - off - padlen);
    } else {
        out = payload.substr(off);
    }
    return true;
}

H2Codec::Result H2Codec::feed(int32_t stream_id, h2::FrameType type, uint8_t flags,
                              const std::string& payload) {
    Result r;

    if (type == h2::FrameType::HEADERS) {
        if (!stream_start_valid(stream_id)) { r.protocol_ok = false; r.error = h2::ErrorCode::PROTOCOL_ERROR; return r; }
        Head& h = heads_[stream_id];
        h.id = stream_id;
        std::string hp;
        if (!strip_headers_prefix(flags, payload, hp)) { r.protocol_ok = false; r.error = h2::ErrorCode::PROTOCOL_ERROR; return r; }
        h.block += hp;
        if (flags & h2::flag::END_HEADERS) h.headers_done = true;
        if (flags & h2::flag::END_STREAM) h.end_stream = true;
    } else if (type == h2::FrameType::CONTINUATION) {
        auto it = heads_.find(stream_id);
        if (it == heads_.end()) { r.protocol_ok = false; r.error = h2::ErrorCode::PROTOCOL_ERROR; return r; }   // 无 HEADERS 打头的续帧
        it->second.block += payload;                       // 纯 HPACK 续块（无前缀）
        if (flags & h2::flag::END_HEADERS) it->second.headers_done = true;
    } else if (type == h2::FrameType::DATA) {
        // 静态 GET/HEAD 无请求体；只需留意 END_STREAM 使请求闭合
        auto it = heads_.find(stream_id);
        if (it != heads_.end() && (flags & h2::flag::END_STREAM)) it->second.end_stream = true;
    } else {
        return r;                                          // 流控/管理帧：session 自处理，忽略
    }

    // 请求完整（END_HEADERS + END_STREAM）且未产出过 → 解码 + 归一化
    auto it = heads_.find(stream_id);
    if (it != heads_.end() && it->second.headers_done && it->second.end_stream && !it->second.served) {
        if (!decoder_.decode(it->second.block, it->second.headers)) {
            r.protocol_ok = false; r.error = h2::ErrorCode::COMPRESSION_ERROR;   // HPACK 解码失败
            return r;
        }
        it->second.served = true;
        HttpRequest q;
        for (auto& hh : it->second.headers) {
            if      (hh.first == ":method") q.method = hh.second;
            else if (hh.first == ":path")   q.path   = hh.second;
        }
        if (q.method.empty()) q.method = "GET";
        if (q.path.empty())   q.path = "/";
        r.request = std::move(q);
    }
    return r;
}

void H2Codec::abort_stream(int32_t stream_id) {
    heads_.erase(stream_id);
}

} // namespace codec
} // namespace web_server