#ifndef WEB_SERVER_CODEC_H2_CODEC_H
#define WEB_SERVER_CODEC_H2_CODEC_H
// ──────────────────────────────────────────────────────────────────
// H2Codec —— HTTP/2 的 wire→message(codec)，H2Session 的 socket+流控+发送
//
// 职责边界（与 H2Session 分工）：
//   codec   帧/流级去复用 + 每流请求头装配 + HPACK 解码 → 归一化 HttpRequest
//   session socket 读循环 + 流控窗口 + 出站发送 + 连接级帧(流穿 SETTINGS/WU/PING)
//
// H2Session 不再持有 HPACK decoder / header_block / headers / 流 ID 合法性——
// 这些都收进这里，成为"原始字节→一条请求消息"的纯净层。后续 H3Codec 可实现
// 同一 HttpCodec 接口(暂未抽象纯接口，形态一致即可)，MessageProcessor 原样复用。
// 不碰 socket、不碰流控窗口、不发送。
// ──────────────────────────────────────────────────────────────────
#include "web_server/http_types.hpp"     // HttpRequest
#include "web_server/h2/h2_hpack.h"      // h2::HeaderDecoder / h2::Header
#include "web_server/h2/h2_types.hpp"    // h2::FrameType / h2::flag
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace web_server {
namespace codec {

class H2Codec {
public:
    // feed 一帧流级帧（stream_id>0）的产出
    struct Result {
        bool protocol_ok = true;                 // false = 连接级错误，session 应 close_protocol(错误码见 error)
        h2::ErrorCode error = h2::ErrorCode::NO_ERROR;   // 非法流 ID/前缀越界 → PROTOCOL_ERROR；HPACK 解码失败 → COMPRESSION_ERROR
        std::optional<HttpRequest> request;      // 本帧使一条请求完整(END_HEADERS+END_STREAM) → 归一化请求
    };

    // 喂一帧。type ∈ {HEADERS, CONTINUATION, DATA} 参与装配；其余(流控类)由 session 自处理，此处忽略。
    Result feed(int32_t stream_id, h2::FrameType type, uint8_t flags,
                const std::string& payload);

    // RST_STREAM：客户端主动掐流 → 清掉该流装配态（session 对齐清其流控态）
    void abort_stream(int32_t stream_id);

    // 对端 SETTINGS_HEADER_TABLE_SIZE → 配到解码器（session 只负责流控窗，透传此值）
    void apply_header_table_size(uint32_t n) { decoder_.set_header_table_size(n); }

private:
    struct Head {
        int32_t id = 0;
        std::string block;                // 纯 HPACK 字节（跨 HEADERS/CONTINUATION 累积）
        std::vector<h2::Header> headers;  // 解码结果（END_HEADERS 后产出）
        bool headers_done = false;        // END_HEADERS 已到
        bool end_stream   = false;        // END_STREAM 已到
        bool served       = false;        // 已产出一次请求（避免重复触发）
    };
    bool stream_start_valid(int32_t id);  // 偶数 / 非单调递增 → 非法（仅 HEADERS 开流时）

    h2::HeaderDecoder decoder_;           // 连接级 HPACK 解码器（动态表跨流共享）
    std::map<int32_t, Head> heads_;       // 每流请求装配态
    int32_t last_client_stream_ = 0;      // 校验客户端单调开流
};

} // namespace codec
} // namespace web_server
#endif // WEB_SERVER_CODEC_H2_CODEC_H