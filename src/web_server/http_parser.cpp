#include "web_server/http_parser.h"
#include <cctype>
#include <filesystem>
#include <sstream>

namespace web_server {

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e-1]))) --e;
    return s.substr(b, e - b);
}

static std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_request_line(const std::string& line, HttpRequest& req) {
    // "VERB SP target SP HTTP/1.1" —— 用空格切，严格三段
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    std::string method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string version = line.substr(sp2 + 1);

    // 只认方法令牌（RFC 7230 §3.1.1 允许任意 token，但静态服务器只需几个）
    for (char c : method)
        if (std::isspace(static_cast<unsigned char>(c))) return false;

    // 版本必须精确匹配
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return false;
    if (target.empty() || target[0] != '/') return false;

    req.method = method;
    req.raw_target = target;
    req.version = version;
    // 拆 query
    size_t q = target.find('?');
    if (q == std::string::npos) {
        req.path = target;
        req.query.clear();
    } else {
        req.path = target.substr(0, q);
        req.query = target.substr(q + 1);
    }
    return true;
}

bool parse_header_line(const std::string& line, HttpRequest& req) {
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return false;   // 无冒号或空名字
    std::string name = to_lower(trim(line.substr(0, colon)));
    std::string value = trim(line.substr(colon + 1));
    if (name.empty()) return false;
    req.headers[name] = value;   // 同名字头后覆盖（简化：不合并逗号列表）
    return true;
}

bool determine_body_framing(const HttpRequest& req,
                            bool& out_chunked, size_t& out_content_length,
                            bool& out_has_body) {
    const std::string* te = req.find_header("transfer-encoding");
    const std::string* cl = req.find_header("content-length");

    out_chunked = false;
    out_content_length = 0;
    out_has_body = false;

    if (te && !te->empty()) {
        // 只支持最后一个编码为 chunked（RFC 7230 §3.3.1）。其余编码一律拒绝。
        std::string lower = to_lower(*te);
        size_t last = lower.find_last_of(',');
        std::string last_coding = lower.substr(last == std::string::npos ? 0 : last + 1);
        last_coding = trim(last_coding);
        if (last_coding != "chunked") return false;
        out_chunked = true;
        out_has_body = true;
        return true;
    }

    if (cl) {
        // Content-Length 必须是一串十进制数字
        if (cl->empty()) return false;
        size_t bytes = 0;
        for (char c : *cl) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            if (bytes > (SIZE_MAX - static_cast<size_t>(c - '0')) / 10) return false; // 溢出
            bytes = bytes * 10 + static_cast<size_t>(c - '0');
        }
        out_content_length = bytes;
        out_has_body = (bytes != 0);
        return true;
    }

    // 无 body
    return true;
}

// RFC 3986 %XX 解码。遇非法字节（含 0x00 或截断的 %）→ false 整体拒绝，防路径注入。
static bool percent_decode(const std::string& in, std::string& out) {
    static const auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return false;          // 截断
            int hi = hexval(in[i+1]), lo = hexval(in[i+2]);
            if (hi < 0 || lo < 0) return false;            // 非 hex
            int byte = (hi << 4) | lo;
            if (byte == 0) return false;                   // 空字节
            out.push_back(static_cast<char>(byte));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return true;
}

bool resolve_safe_path(const std::string& doc_root, const std::string& url_path,
                       std::string& out_full_path) {
    std::string decoded;
    if (!percent_decode(url_path, decoded)) return false;

    // 词法归一化：把 doc_root 与 decoded 拼起来再 lexically_normal，
    // 自动消除 ./ ../ 与重复斜杠，也消掉 doc_root 末尾的 / 差异。
    // 注意：url_path 恒以 '/' 开头，去掉前导 '/' 再拼接——否则 operator/ 把
    // 绝对路径当第二个操作数时整条路径会被覆盖（/a 直接落到文件系统根）。
    std::filesystem::path root(doc_root);
    std::string rel = decoded;
    if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    std::filesystem::path joined = root / rel;
    std::filesystem::path norm = joined.lexically_normal();

    // 目录穿越检查：归一化后仍必须以 doc_root 的归一化形式作为前缀。
    std::filesystem::path root_norm = root.lexically_normal();
    std::string rn = root_norm.string();
    std::string pn = norm.string();
    // 前缀匹配，且要求边界是 '/'（防止 /doc_root2 也匹配 /doc_root）
    if (pn.size() < rn.size() ||
        pn.compare(0, rn.size(), rn) != 0 ||
        (pn.size() > rn.size() && pn[rn.size()] != '/')) {
        return false;   // 越权（../ 逃逸）
    }

    out_full_path = pn;
    return true;
}

} // namespace web_server