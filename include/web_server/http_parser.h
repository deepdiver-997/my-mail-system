#ifndef WEB_SERVER_HTTP_PARSER_H
#define WEB_SERVER_HTTP_PARSER_H
// ──────────────────────────────────────────────────────────────────
// HTTP/1.1 纯解析函数（非 FSM 部分）
//
// 状态机负责"何时读下一块"，这里只管"这一块怎么解析"。函数都写成
// 同步纯函数、不碰连接/session，便于单测。返回 bool 表示解析成功，
// 失败时调用方回 400。
// ──────────────────────────────────────────────────────────────────
#include "web_server/http_types.hpp"
#include <string>

namespace web_server {

// 解析请求行 "METHOD SP target SP HTTP/x.y"，填充 req 的方法/目标/路径/query/版本。
// line 已去掉行尾 CRLF/LF。返回 false = 语法错误 → 400。
bool parse_request_line(const std::string& line, HttpRequest& req);

// 解析一行 "Name: value"。key 小写化、value 去首尾空白。
// 空行（只含 CRLF）应已由上层判为 HEADER_END，不会走到这里。
bool parse_header_line(const std::string& line, HttpRequest& req);

// 把所有已解析 header 归纳成 body framing（Content-Length / Transfer-Encoding: chunked）。
// 规则（RFC 7230 §3.3.3）：TE 优先于 CL；两个 CL 且不一致 → 错误。
// 返回：false = framing 非法 → 400；true = 字段已填到 req（chunked / content_length）。
bool determine_body_framing(const HttpRequest& req,
                            bool& out_chunked, size_t& out_content_length,
                            bool& out_has_body);

// URL 路径安全处理：
//   1. 解码 %XX，拒绝含 0x00 的字节
//   2. 把 "/" + 原始 path 接在 doc_root 后做词法归一化（消除 ./ ../ 与重复斜杠）
//   3. 检查归一化结果仍落在 doc_root 目录内（防目录穿越）
// 成功返回 true 并写出绝对路径 full_path；失败 false（403/404 由调用方决定）。
bool resolve_safe_path(const std::string& doc_root, const std::string& url_path,
                       std::string& out_full_path);

} // namespace web_server

#endif // WEB_SERVER_HTTP_PARSER_H