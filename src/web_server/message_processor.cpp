// MessageProcessor 实现 —— 见 message_processor.h
//
// 分工：这里只做"路由 + 分类"（方法校验/防穿越/目录→index/打开/定长/ctype），
//       把你该不该允许、该回哪个码、该流哪个文件定下来。**体不入内存**：
//       200 时只给 full_path + content_length，让各传输层自己决定怎么送
//       （H1 走 sendfile/流式，H2 读入 body 走流控分片）。
#include "web_server/message_processor.h"
#include "web_server/http_parser.h"     // resolve_safe_path
#include "web_server/http_types.hpp"    // mime_for_extension / HttpResponse
#include <filesystem>
#include <fstream>

namespace web_server {

HttpResponse process_static_request(const HttpRequest& req, const std::string& doc_root) {
    HttpResponse res;
    // 只服 GET/HEAD（静态服务器语义）
    if (req.method != "GET" && req.method != "HEAD") {
        res.status = 405;
        res.status_text = HttpResponse::reason_phrase(405);
        res.body = "method not allowed\n";
        res.content_length = (int64_t)res.body.size();   // H2 要求 content-length 非负
        return res;
    }
    std::string full;
    if (!resolve_safe_path(doc_root, req.path, full)) {
        res.status = 403;
        res.status_text = HttpResponse::reason_phrase(403);
        res.body = "forbidden\n";
        res.content_length = (int64_t)res.body.size();
        return res;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(full, ec)) {
        full = (std::filesystem::path(full) / "index.html").string();   // 目录 → index
    }
    std::ifstream probe(full, std::ios::binary);
    if (!probe.good()) {
        res.status = 404;
        res.status_text = HttpResponse::reason_phrase(404);
        res.body = "not found\n";
        res.content_length = (int64_t)res.body.size();
        return res;
    }
    probe.seekg(0, std::ios::end);
    int64_t length = static_cast<int64_t>(probe.tellg());
    probe.close();
    res.status = 200;
    res.status_text = "OK";
    res.full_path = full;
    res.content_length = length;                                  // HEAD 也要带对 content-length
    res.content_type = mime_for_extension(
        std::filesystem::path(full).extension().string());
    return res;
}

} // namespace web_server