#ifndef WEB_SERVER_MESSAGE_PROCESSOR_H
#define WEB_SERVER_MESSAGE_PROCESSOR_H
// ──────────────────────────────────────────────────────────────────
// MessageProcessor — 协议无关的 HTTP 消息处理器。
//
// 目标：让 H1/H2/(未来 H3/QUIC) 复用同一份"请求 → 响应"逻辑。
//   H1: 每连接一个 processor（流槽位 0）
//   H2: 每条流一个 processor（流槽位 N）
//   H3: 每条 QUIC 流一个 processor
//
// 契约（与状态机的分界）：
//   * processor 只接收"完整的规范化请求"，返回"响应消息"（复用 http_types 里的
//     HttpRequest/HttpResponse —— H1 已在用它，别再造一套）。
//   * 它不碰连接/session/帧/流、不做网络 I/O —— 因此不持有 session，调用方
//     （session）在栈上活着，传 this/引用即可，无需、也不准传 shared_ptr
//     （避免双向持有→循环）。
//   * 它只做本地文件读取 / 字节→响应报文的组装（HEAD/GET 静态文件）。
// 音译：这是"被喂食的纯函数"，字节怎么搬成请求、响应怎么搬成线，归 session。
// ──────────────────────────────────────────────────────────────────
#include "web_server/http_types.hpp"     // HttpRequest / HttpResponse（复用 H1 的类型）

namespace web_server {

// 静态文件请求处理：路由 → 防穿越 → 目录补 index.html → 读取。
//   403 forbidden / 404 not found / 405 method not allowed
// HEAD 也照常读好 body（好算 content-length），由调用方决定是否丢弃体。
// 纯同步、只读本地文件，不碰任何网络状态。
HttpResponse process_static_request(const HttpRequest& req, const std::string& doc_root);

} // namespace web_server

#endif // WEB_SERVER_MESSAGE_PROCESSOR_H