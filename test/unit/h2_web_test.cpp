// h2_web_test —— 剧情优先单测：web_server 新增纯函数层的不变量覆盖
//   MessageProcessor（路由/防穿越/定长）
//   h2_framer         （帧序列化↔解析往返）
//   h2_hpack          （响应头编码 ↔ HeaderDecoder 解码往返）
//   h2_codec          （喂帧 → 归一化请求；流 ID 单调/奇偶门控；CONTINUATION 装配）
#include "web_server/message_processor.h"
#include "web_server/codec/h2_codec.h"
#include "web_server/h2/h2_framer.h"
#include "web_server/h2/h2_hpack.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int g_pass = 0, g_fail = 0;
void check(const char* name, bool cond) {
    if (cond) { ++g_pass; return; }
    ++g_fail;
    std::cerr << "FAIL: " << name << '\n';
}

// ── fixture：静态文件根 ─────────────────────────────────────────
const std::string kRoot = "/tmp/h2_web_test_root";
void make_fixture() {
    std::filesystem::remove_all(kRoot);
    std::filesystem::create_directories(kRoot + "/sub");
    { std::ofstream f(kRoot + "/hello.txt"); f << "hello web"; }
    { std::ofstream f(kRoot + "/index.html"); f << "<h1>idx</h1>"; }
    { std::ofstream f(kRoot + "/sub/index.html"); f << "<h1>sub</h1>"; }
}

void test_message_processor() {
    web_server::HttpRequest req;
    // GET 现有文件 → 200，且定长但不读体（H1 sendfile / H2 流控各自负责送）
    req.method = "GET"; req.path = "/hello.txt";
    auto r = web_server::process_static_request(req, kRoot);
    check("mp:200 status", r.status == 200);
    check("mp:200 content_length", r.content_length == 9);
    check("mp:200 full_path 回填", r.full_path == kRoot + "/hello.txt");
    check("mp:200 体不入内存(content_length 定长，满载由传输层)", r.body.empty());

    // 目录 → index.html
    req.method = "GET"; req.path = "/sub";
    r = web_server::process_static_request(req, kRoot);
    check("mp:目录→index", r.status == 200 && r.full_path == kRoot + "/sub/index.html");

    // 缺失 → 404（错误体带 content_length，H2 要求非负）
    req.method = "GET"; req.path = "/nope.txt";
    r = web_server::process_static_request(req, kRoot);
    check("mp:404", r.status == 404 && r.content_length >= 0);

    // 目录穿越 → 403
    req.method = "GET"; req.path = "/../etc/passwd";
    r = web_server::process_static_request(req, kRoot);
    check("mp:403 traversal", r.status == 403);

    // 非法方法 → 405
    req.method = "POST"; req.path = "/";
    r = web_server::process_static_request(req, kRoot);
    check("mp:405", r.status == 405);

    // HEAD：content_length 也要对，且体同样不入内存（由调用方决定是否丢体）
    req.method = "HEAD"; req.path = "/hello.txt";
    r = web_server::process_static_request(req, kRoot);
    check("mp:HEAD content_length", r.status == 200 && r.content_length == 9);
}

void test_framer_roundtrip() {
    // 序列化 → 解析：类型/flags/stream_id/payload 全还原
    for (uint32_t sid : {1u, 3u, 0x7fffffffu}) {
        auto raw = web_server::h2::serialize_frame(
            web_server::h2::FrameType::DATA, 0x01, sid, "payload-x");
        check("framer:length", web_server::h2::frame_length(raw) == 9);
        web_server::h2::Frame f;
        check("framer:parse", web_server::h2::parse_frame(raw, f));
        check("framer:type", f.type == web_server::h2::FrameType::DATA);
        check("framer:flags", f.flags == 0x01);
        check("framer:stream_id", f.stream_id == sid);
        check("framer:payload", f.payload == "payload-x");
    }
    // 未到齐的帧不完整 → parse false
    web_server::h2::Frame f;
    check("framer:incomplete", !web_server::h2::parse_frame("short", f));
    // GOAWAY 单帧构造
    auto g = web_server::h2::build_goaway(3, web_server::h2::ErrorCode::PROTOCOL_ERROR);
    check("framer:goaway parse", web_server::h2::parse_frame(g, f) && f.type == web_server::h2::FrameType::GOAWAY);
}

void test_hpack_roundtrip() {
    // 响应头编码 → HeaderDecoder 解码 → 头字段逐一致（覆盖静态表 :status 索引 + 字面量）
    std::vector<web_server::h2::Header> in = {
        {":status", "200"}, {"content-type", "text/html"}, {"content-length", "5"}};
    auto block = web_server::h2::encode_response_headers(in);
    web_server::h2::HeaderDecoder dec;
    std::vector<web_server::h2::Header> out;
    check("hpack:decode", dec.decode(block, out));
    bool ok = out.size() == 3
        && out[0] == in[0] && out[1] == in[1] && out[2] == in[2];
    check("hpack:逐一致", ok);
}

// 构造一个 GET /big.bin 的 HPACK 请求头块（curl 同款静态表三字段）
std::string build_get_block(const std::string& path) {
    std::string b;
    b += '\x82';                          // :method GET   (静表索引 2)
    b += '\x86';                          // :scheme http  (静表索引 6)
    b += '\x44';                          // :path 名索引 4（无索引字面量）
    b += (char)path.size(); b += path;    // 值（Huffman=0）
    return b;
}

void test_codec_feed() {
    web_server::codec::H2Codec codec;
    auto b = build_get_block("/big.bin");

    // 完整 HEADERS(END_HEADERS|END_STREAM) → 归一化请求
    auto r = codec.feed(1, web_server::h2::FrameType::HEADERS, 0x05, b);
    check("codec:ok", r.protocol_ok);
    check("codec:method", r.request && r.request->method == "GET");
    check("codec:path", r.request && r.request->path == "/big.bin");

    // 偶数流 ID → PROTOCOL_ERROR
    r = codec.feed(2, web_server::h2::FrameType::HEADERS, 0x05, b);
    check("codec:even-id → PROTOCOL_ERROR",
          !r.protocol_ok && r.error == web_server::h2::ErrorCode::PROTOCOL_ERROR);

    // 非单调重复开流 → PROTOCOL_ERROR（复用已用过的流 ID 开新 HEADERS）
    r = codec.feed(1, web_server::h2::FrameType::HEADERS, 0x05, b);
    check("codec:重复流 ID 开流 → PROTOCOL_ERROR",
          !r.protocol_ok && r.error == web_server::h2::ErrorCode::PROTOCOL_ERROR);

    // CONTINUATION 装配：HEADERS 一半(无 END_HEADERS) + CONTINUATION 余下(END_HEADERS)
    web_server::codec::H2Codec codec2;
    size_t mid = b.size() / 2;
    auto h1 = codec2.feed(3, web_server::h2::FrameType::HEADERS, 0x01,      // END_STREAM only
                          b.substr(0, mid));
    check("codec:split 前半未就绪", h1.protocol_ok && !h1.request);
    auto h2 = codec2.feed(3, web_server::h2::FrameType::CONTINUATION, 0x04, // END_HEADERS
                          b.substr(mid));
    check("codec:continuation 后半装配成请求", h2.protocol_ok && h2.request && h2.request->path == "/big.bin");

    // 无 HEADERS 打头的 CONTINUATION → PROTOCOL_ERROR
    auto bad = codec2.feed(9, web_server::h2::FrameType::CONTINUATION, 0x04, "x");
    check("codec:孤儿 CONTINUATION → PROTOCOL_ERROR",
          !bad.protocol_ok && bad.error == web_server::h2::ErrorCode::PROTOCOL_ERROR);
}

} // namespace

int main() {
    make_fixture();
    test_message_processor();
    test_framer_roundtrip();
    test_hpack_roundtrip();
    test_codec_feed();
    std::cerr << "h2_web_test  pass=" << g_pass << " fail=" << g_fail << '\n';
    std::cout << "pass=" << g_pass << " fail=" << g_fail << '\n';
    return g_fail == 0 ? 0 : 1;
}