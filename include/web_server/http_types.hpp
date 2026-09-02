#ifndef WEB_SERVER_HTTP_TYPES_HPP
#define WEB_SERVER_HTTP_TYPES_HPP
// ──────────────────────────────────────────────────────────────────
// HTTP/1.1 静态文件服务器 — 类型定义
//
// 设计对齐 mail_system 框架：状态/事件用带 COUNT 哨兵的枚举，喂给
// FastFsmBase（O(1) 数组派发）。HttpRequest 即 Session 的 context——
// smtps 的 SmtpsContext 是自有 struct，这里直接用一个可复用的请求对象。
// ──────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace web_server {

// ================================================================
// 状态机枚举（必须带 COUNT 哨兵供 FastFsmBase 定长数组使用）
// ================================================================
enum class HttpState {
    WAIT_REQUEST_LINE = 0,   // 读请求行
    WAIT_HEADERS      = 1,   // 逐行读 header，直到空行
    WAIT_BODY         = 2,   // 按 Content-Length / chunked 读 body
    RESPOND           = 3,   // 已收完整请求，写响应（异步）
    CLOSED            = 4,   // 终态
    COUNT
};

enum class HttpEvent {
    REQUEST_LINE = 0,   // 请求行解析完
    HEADER_LINE  = 1,   // 收到一行 header
    HEADER_END   = 2,   // 收到空行 → header 结束
    BODY         = 3,   // 收到 body 数据（可能多段）
    BODY_END     = 4,   // body 收完 → 进入 RESPOND
    ERROR        = 5,
    TIMEOUT      = 6,
    COUNT
};

inline const char* http_state_name(HttpState s) {
    switch (s) {
        case HttpState::WAIT_REQUEST_LINE: return "WAIT_REQUEST_LINE";
        case HttpState::WAIT_HEADERS:      return "WAIT_HEADERS";
        case HttpState::WAIT_BODY:         return "WAIT_BODY";
        case HttpState::RESPOND:           return "RESPOND";
        case HttpState::CLOSED:            return "CLOSED";
        default:                           return "?";
    }
}
inline const char* http_event_name(HttpEvent e) {
    switch (e) {
        case HttpEvent::REQUEST_LINE: return "REQUEST_LINE";
        case HttpEvent::HEADER_LINE:  return "HEADER_LINE";
        case HttpEvent::HEADER_END:   return "HEADER_END";
        case HttpEvent::BODY:         return "BODY";
        case HttpEvent::BODY_END:     return "BODY_END";
        case HttpEvent::ERROR:        return "ERROR";
        case HttpEvent::TIMEOUT:      return "TIMEOUT";
        default:                      return "?";
    }
}

// ================================================================
// HttpRequest — 单次请求上下文（Session 的 get_context() 返回值）
// ================================================================
struct HttpRequest {
    std::string method;                              // GET/HEAD/...
    std::string raw_target;                          // 原始请求目标 "/a.html?x=1"
    std::string path;                                // '?' 之前的部分
    std::string query;                               // '?' 之后
    std::string version;                             // "HTTP/1.1"

    // header：key 全部小写化（http_parser 落库时做），查找不区分大小写
    std::unordered_map<std::string, std::string> headers;

    // ── body framing 元数据（HEADER_END 时由 parser 填好）──
    bool   has_content_length = false;
    size_t content_length     = 0;
    bool   chunked            = false;

    std::string body;                                // 已收取的请求体

    void reset() {
        method.clear(); raw_target.clear(); path.clear();
        query.clear(); version.clear();
        headers.clear();
        has_content_length = false; content_length = 0; chunked = false;
        body.clear();
    }

    // 小写 key 查找
    const std::string* find_header(const std::string& lower_key) const {
        auto it = headers.find(lower_key);
        return it == headers.end() ? nullptr : &it->second;
    }

    // keep-alive 判定：HTTP/1.1 默认持久，1.0 需显式 Connection: keep-alive
    bool keep_alive() const {
        const std::string* conn = find_header("connection");
        if (conn) {
            if (*conn == "close") return false;
            if (*conn == "keep-alive") return true;
        }
        return version == "HTTP/1.1";
    }
};

// ================================================================
// HttpResponse — 响应描述（session 转成 wire 文本 / 流式发文件）
// ================================================================
struct HttpResponse {
    int         status      = 200;
    std::string status_text = "OK";
    std::string content_type = "text/html; charset=utf-8";
    std::string body;                  // 内联响应体（错误页/首页/HEAD的占位）
    int64_t     content_length = -1;   // -1 = 由 body.size() 推导
    std::string full_path;             // 200 静态文件：本地绝对路径（供 sendfile/流式发送），体不入内存
    std::string extra_headers;         // 附加头部（逐条含 CRLF），如 Cache-Control
    bool        close_after = false;   // 响应后断开（HTTP/1.0 无 keep-alive 等）

    static const char* reason_phrase(int code) {
        switch (code) {
            case 200: return "OK";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 400: return "Bad Request";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 414: return "URI Too Long";
            case 431: return "Request Header Fields Too Large";
            case 501: return "Not Implemented";
            default:  return "Internal Server Error";
        }
    }
};

// MIME 类型表（按扩展名；未知默认 application/octet-stream）
inline const char* mime_for_extension(const std::string& ext) {
    struct Entry { const char* ext; const char* mime; };
    static const Entry kTable[] = {
        {".html","text/html; charset=utf-8"},  {".htm","text/html; charset=utf-8"},
        {".css","text/css"},                    {".js","application/javascript"},
        {".json","application/json"},           {".txt","text/plain; charset=utf-8"},
        {".xml","application/xml"},             {".svg","image/svg+xml"},
        {".png","image/png"},                   {".jpg","image/jpeg"},
        {".jpeg","image/jpeg"},                 {".gif","image/gif"},
        {".webp","image/webp"},                 {".ico","image/x-icon"},
        {".woff","font/woff"},                  {".woff2","font/woff2"},
        {".ttf","font/ttf"},                    {".otf","font/otf"},
        {".pdf","application/pdf"},             {".zip","application/zip"},
        {".wasm","application/wasm"},
    };
    for (auto& e : kTable)
        if (ext.size() == std::strlen(e.ext) && 0 == ext.compare(e.ext))
            return e.mime;
    return "application/octet-stream";
}

} // namespace web_server

#endif // WEB_SERVER_HTTP_TYPES_HPP