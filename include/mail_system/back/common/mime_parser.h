#pragma once
#include "mail_system/back/entities/mail.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace mail_system {

// 解析邮件 body 文件的 MIME 结构，填充 mime_root 树
// body 内容形如 "Header: value\r\n\r\nbody..."
// raw 收 string_view：调用方可以直接传 mmap 出来的映射区，
// 不必先把整封邮件读进 std::string。所有传 std::string 的旧调用点隐式转换，无需改动。
// MimePart 里存的是偏移量和拷贝出来的小字符串，不持有 raw 的指针，
// 因此解析完成后映射可以安全释放。
// depth：multipart 嵌套层级。恶意构造的深嵌套邮件（每层一个 boundary）
// 会把递归打到栈溢出 —— fuzz 冒烟的第一个必中目标。真实邮件嵌套不超过
// ~10 层，64 层封顶后该 part 按不透明正文处理，长度信息仍由调用方修正。
inline void parse_mime_tree(std::string_view raw, MimePart& root, size_t pos = 0,
                            size_t depth = 0) {
    // 找 header/body 分隔
    size_t sep = raw.find("\r\n\r\n", pos);
    std::string hdrs{(sep != std::string::npos) ? raw.substr(pos, sep - pos) : raw.substr(pos)};
    size_t body_start = (sep != std::string::npos) ? sep + 4 : raw.size();

    // 解析 Content-Type
    {
        auto hdr_lower = hdrs;
        std::transform(hdr_lower.begin(), hdr_lower.end(), hdr_lower.begin(), ::tolower);
        size_t ct_pos = std::string::npos;
        // 找行首的 content-type（避免 DKIM h= 标签干扰）
        for (size_t p = 0; p < hdr_lower.size(); ) {
            size_t f = hdr_lower.find("\ncontent-type:", p);
            if (f == std::string::npos) {
                // 检查第一行
                if (p == 0 && hdr_lower.compare(0, 13, "content-type:") == 0) { ct_pos = 0; }
                break;
            }
            ct_pos = f + 1; // 跳过 \n
            break;
        }
        if (ct_pos == std::string::npos && hdr_lower.compare(0, 13, "content-type:") == 0)
            ct_pos = 0;

        if (ct_pos != std::string::npos) {
            size_t ct_end = hdr_lower.find("\r\n", ct_pos);
            // 展开 folded headers
            while (ct_end != std::string::npos && ct_end + 2 < hdr_lower.size() &&
                   (hdr_lower[ct_end + 2] == ' ' || hdr_lower[ct_end + 2] == '\t'))
                ct_end = hdr_lower.find("\r\n", ct_end + 2);

            std::string ct_line = (ct_end != std::string::npos)
                ? hdr_lower.substr(ct_pos + 13, ct_end - ct_pos - 13)
                : hdr_lower.substr(ct_pos + 13);

            // unfold
            std::string unfolded;
            for (size_t i = 0; i < ct_line.size(); ++i) {
                if (ct_line[i] == '\r' && i + 2 < ct_line.size() &&
                    ct_line[i+1] == '\n' && ct_line[i+2] == '\t') { unfolded += ' '; i += 2; continue; }
                if (ct_line[i] == '\r' && i + 1 < ct_line.size() &&
                    ct_line[i+1] == '\n') { unfolded += ' '; i += 1; continue; }
                unfolded += ct_line[i];
            }
            ct_line = unfolded;
            ct_line.erase(0, ct_line.find_first_not_of(" \t"));
            ct_line.erase(ct_line.find_last_not_of(" \t") + 1);

            auto semi = ct_line.find(';');
            if (semi != std::string::npos) {
                std::string full_type = ct_line.substr(0, semi);
                full_type.erase(full_type.find_last_not_of(" \t") + 1);
                auto slash = full_type.find('/');
                if (slash != std::string::npos) {
                    root.type = full_type.substr(0, slash);
                    root.subtype = full_type.substr(slash + 1);
                }
                std::string params = ct_line.substr(semi + 1);
                // charset（支持带引号和不带引号，如 charset="utf-8" 或 charset=us-ascii）
                auto cp = params.find("charset=");
                if (cp != std::string::npos) {
                    cp += 8;
                    while (cp < params.size() && (params[cp] == ' ' || params[cp] == '\t')) cp++;
                    if (cp < params.size() && params[cp] == '"') {
                        cp++;
                        auto ce = params.find('"', cp);
                        if (ce != std::string::npos) root.charset = params.substr(cp, ce - cp);
                    } else {
                        auto ce = params.find_first_of(" \t;", cp);
                        if (ce == std::string::npos) ce = params.size();
                        root.charset = params.substr(cp, ce - cp);
                    }
                }
                // boundary — 从原始 hdrs 提取（保留大小写）
                // 注意：不带引号时不能 find('"') 向后搜，否则会误匹配后续 header 里的引号
                // （如 To: "test3"），把 boundary 提取成错误值。
                {
                    std::string orig_hdrs{raw.substr(pos, sep - pos)};
                    auto obp = orig_hdrs.find("boundary=");
                    if (obp != std::string::npos) {
                        size_t os = obp + 9; // "boundary=" 之后
                        while (os < orig_hdrs.size() &&
                               (orig_hdrs[os] == ' ' || orig_hdrs[os] == '\t'))
                            os++;
                        if (os < orig_hdrs.size() && orig_hdrs[os] == '"') {
                            // 带引号：boundary="..."
                            os++;
                            size_t oe = orig_hdrs.find('"', os);
                            if (oe != std::string::npos)
                                root.boundary = orig_hdrs.substr(os, oe - os);
                        } else {
                            // 不带引号：boundary=值，到空白/换行/header 末尾为止
                            size_t oe = orig_hdrs.find_first_of(" \t\r\n", os);
                            if (oe == std::string::npos) oe = orig_hdrs.size();
                            root.boundary = orig_hdrs.substr(os, oe - os);
                        }
                    }
                }
                // name (Content-Type name 或 Content-Disposition filename)
                auto np = params.find("name=\"");
                if (np != std::string::npos) {
                    np += 6;
                    auto ne = params.find('"', np);
                    if (ne != std::string::npos) root.name = params.substr(np, ne - np);
                }
            } else {
                auto slash = ct_line.find('/');
                if (slash != std::string::npos) {
                    root.type = ct_line.substr(0, slash);
                    root.subtype = ct_line.substr(slash + 1);
                }
            }
        } else {
            // 畸形邮件（header 在 body 之后，如部分 OpenAI 通知邮件）时，首段
            // （第一个 \r\n\r\n 之前）没有 Content-Type。此时在整封消息里找
            // 行首的 content-type:（避开 DKIM h= 标签），取最后一次匹配。
            bool ct_found = false;
            // 必须显式拷贝成 std::string：raw 现在是 string_view，可能指向只读的
            // mmap 映射区，就地 tolower 会往 PROT_READ 的页里写。
            // 这条是「Content-Type 出现在 body 之后」的畸形报文兜底路径，极少走到，
            // 整封拷贝一次可以接受。
            std::string whole_lower{raw};
            std::transform(whole_lower.begin(), whole_lower.end(), whole_lower.begin(), ::tolower);
            size_t ct_line_start = std::string::npos;
            for (size_t p = 0; p < whole_lower.size(); ) {
                size_t f = whole_lower.find("\ncontent-type:", p);
                if (f == std::string::npos) {
                    if (p == 0 && whole_lower.compare(0, 13, "content-type:") == 0)
                        ct_line_start = 0;
                    break;
                }
                ct_line_start = f + 1;   // 保留最后一次匹配
                p = f + 1;
            }
            if (ct_line_start != std::string::npos && ct_line_start > 0) {
                size_t ct_end = whole_lower.find("\r\n", ct_line_start);
                while (ct_end != std::string::npos && ct_end + 2 < whole_lower.size() &&
                       (whole_lower[ct_end + 2] == ' ' || whole_lower[ct_end + 2] == '\t'))
                    ct_end = whole_lower.find("\r\n", ct_end + 2);
                std::string ct_line = (ct_end != std::string::npos)
                    ? whole_lower.substr(ct_line_start + 13, ct_end - ct_line_start - 13)
                    : whole_lower.substr(ct_line_start + 13);
                auto semi = ct_line.find(';');
                std::string full_type = (semi != std::string::npos)
                    ? ct_line.substr(0, semi) : ct_line;
                full_type.erase(0, full_type.find_first_not_of(" \t"));
                full_type.erase(full_type.find_last_not_of(" \t") + 1);
                auto slash = full_type.find('/');
                if (slash != std::string::npos) {
                    root.type = full_type.substr(0, slash);
                    root.subtype = full_type.substr(slash + 1);
                    if (semi != std::string::npos) {
                        std::string params = ct_line.substr(semi + 1);
                        auto cp = params.find("charset=");
                        if (cp != std::string::npos) {
                            cp += 8;
                            while (cp < params.size() && (params[cp] == ' ' || params[cp] == '\t')) cp++;
                            if (cp < params.size() && params[cp] == '"') {
                                cp++;
                                auto ce = params.find('"', cp);
                                if (ce != std::string::npos) root.charset = params.substr(cp, ce - cp);
                            } else {
                                auto ce = params.find_first_of(" \t;", cp);
                                if (ce == std::string::npos) ce = params.size();
                                root.charset = params.substr(cp, ce - cp);
                            }
                        }
                    }
                    ct_found = true;
                }
            }
            if (!ct_found) {
                // 默认 text/plain
                root.type = "text";
                root.subtype = "plain";
            }
        }
    }

    // Content-Disposition filename
    if (root.name.empty()) {
        auto hdr_lower = hdrs;
        std::transform(hdr_lower.begin(), hdr_lower.end(), hdr_lower.begin(), ::tolower);
        size_t cd = hdr_lower.find("\ncontent-disposition:");
        if (cd == std::string::npos && hdr_lower.compare(0, 20, "content-disposition:") == 0) cd = 0;
        else if (cd != std::string::npos) cd++;
        if (cd != std::string::npos) {
            auto np = hdr_lower.find("filename=\"", cd);
            if (np == std::string::npos) np = hdr_lower.find("filename=", cd);
            if (np != std::string::npos) {
                np = hdr_lower.find('"', np);
                if (np == std::string::npos) np = hdr_lower.find('=', np);
                if (np != std::string::npos) {
                    np++;
                    auto ne = hdr_lower.find('"', np);
                    if (ne == std::string::npos)
                        ne = hdr_lower.find_first_of(" \t\r\n", np);
                    if (ne != std::string::npos)
                        root.name = hdrs.substr(np, ne - np);
                }
            }
        }
    }

    // Content-Transfer-Encoding
    {
        auto hdr_lower = hdrs;
        std::transform(hdr_lower.begin(), hdr_lower.end(), hdr_lower.begin(), ::tolower);
        size_t cte = hdr_lower.find("\ncontent-transfer-encoding:");
        if (cte == std::string::npos && hdr_lower.compare(0, 26, "content-transfer-encoding:") == 0) cte = 0;
        else if (cte != std::string::npos) cte++;
        if (cte != std::string::npos) {
            size_t cte_end = hdr_lower.find("\r\n", cte);
            root.encoding = (cte_end != std::string::npos)
                ? hdr_lower.substr(cte + 27, cte_end - cte - 27)
                : hdr_lower.substr(cte + 27);
            root.encoding.erase(0, root.encoding.find_first_not_of(" \t"));
            root.encoding.erase(root.encoding.find_last_not_of(" \t") + 1);
        }
    }
    if (root.encoding.empty()) root.encoding = "7bit";

    // 记录 offset 和 length（length 覆盖整个 part：header + body）
    // 注意：非 multipart 的根 part 也必须用整封邮件长度，否则 IMAP BODY[1] 只能取到 header
    root.offset = pos;
    root.length = raw.size() - pos;
    // body 部分大小（content after \r\n\r\n）
    if (sep != std::string::npos) {
        root.body_size = raw.size() - body_start;
        root.lines = 0;
        for (size_t i = body_start; i < raw.size(); ++i)
            if (raw[i] == '\n') root.lines++;
    }

    // multipart: 递归解析子 part（深度封顶防栈溢出）
    if (root.is_multipart() && !root.boundary.empty() && depth < 64) {
        std::string bdr = "--" + root.boundary;
        size_t search_from = body_start;

        // 跳到第一个 boundary 后的内容
        size_t first = raw.find(bdr + "\r\n", search_from);
        if (first == std::string::npos) first = raw.find(bdr + "\n", search_from);
        if (first != std::string::npos) {
            size_t nl = raw.find("\r\n", first);
            if (nl == std::string::npos) nl = raw.find('\n', first);
            if (nl != std::string::npos)
                search_from = nl + (raw[nl] == '\r' ? 2 : 1);
        }

        while (search_from < raw.size()) {
            size_t next = raw.find(bdr, search_from);
            if (next == std::string::npos) break;
            if (next > 0 && raw[next-1] != '\n') { search_from = next + bdr.size(); continue; }

            bool is_close = (next + bdr.size() + 2 <= raw.size() &&
                             raw.substr(next + bdr.size(), 2) == "--");

            MimePart sub;
            parse_mime_tree(raw, sub, search_from, depth + 1);
            // 修正 sub 的 length 到 bdr 边界
            sub.length = next - search_from;
            while (sub.length > 0 && (raw[search_from + sub.length - 1] == '\r' ||
                                       raw[search_from + sub.length - 1] == '\n'))
                sub.length--;
            // 写回真实 body_size
            size_t sub_sep = raw.find("\r\n\r\n", search_from);
            if (sub_sep != std::string::npos) {
                sub.body_size = next - (sub_sep + 4);
                while (sub.body_size > 0 && (raw[sub_sep + 4 + sub.body_size - 1] == '\r' ||
                                              raw[sub_sep + 4 + sub.body_size - 1] == '\n'))
                    sub.body_size--;
            }
            root.subs.push_back(std::move(sub));

            if (is_close) break;
            size_t nxt_nl = raw.find("\r\n", next);
            if (nxt_nl == std::string::npos) nxt_nl = raw.find('\n', next);
            if (nxt_nl == std::string::npos) break;
            search_from = nxt_nl + (raw[nxt_nl] == '\r' ? 2 : 1);
        }
    }
}

// sidecar JSON 里的字符串字段转义。boundary/charset 基本是 token，
// 但附件 filename（"n"）可以带引号和反斜杠，不转义会写出读不回来的 JSON。
inline std::string mime_json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 控制字符直接丢弃，避免写出非法 JSON
                    break;
                }
                out += c;
        }
    }
    return out;
}

// 把 MIME 树写成 sidecar JSON —— load_mime_tree 的逆操作。
//
// 先写临时文件再 rename：rename 在 POSIX 上是原子的，因此并发的读者
// （多个 IMAP 会话可能同时 FETCH 同一封邮件）绝不会看到写了一半的 sidecar。
// 临时文件名带 pid，因为 smtpsServer 和 imapsServer 是两个独立进程。
inline bool save_mime_tree(const std::string& body_path, const MimePart& root) {
    if (body_path.empty() || root.type.empty()) {
        return false;
    }

    static std::atomic<std::uint64_t> seq{0};
    const std::string final_path = body_path + ".mime";
    const std::string tmp_path = final_path + ".tmp." +
                                 std::to_string(static_cast<long>(::getpid())) + "." +
                                 std::to_string(seq.fetch_add(1));

    try {
        {
            std::ofstream sf(tmp_path, std::ios::binary | std::ios::trunc);
            if (!sf.is_open()) {
                return false;
            }

            std::function<void(const MimePart&, std::ostream&)> write_part;
            write_part = [&](const MimePart& p, std::ostream& os) {
                os << "{\"t\":\"" << mime_json_escape(p.type)
                   << "\",\"s\":\"" << mime_json_escape(p.subtype) << "\"";
                if (!p.charset.empty())  os << ",\"c\":\"" << mime_json_escape(p.charset) << "\"";
                if (!p.encoding.empty()) os << ",\"e\":\"" << mime_json_escape(p.encoding) << "\"";
                if (!p.boundary.empty()) os << ",\"b\":\"" << mime_json_escape(p.boundary) << "\"";
                if (!p.name.empty())     os << ",\"n\":\"" << mime_json_escape(p.name) << "\"";
                os << ",\"o\":" << p.offset << ",\"l\":" << p.length
                   << ",\"z\":" << p.body_size << ",\"ln\":" << p.lines;
                if (!p.subs.empty()) {
                    os << ",\"p\":[";
                    for (size_t i = 0; i < p.subs.size(); ++i) {
                        if (i) os << ",";
                        write_part(p.subs[i], os);
                    }
                    os << "]";
                }
                os << "}";
            };
            write_part(root, sf);

            sf.flush();
            if (!sf.good()) {
                sf.close();
                std::remove(tmp_path.c_str());
                return false;
            }
        }

        if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
            std::remove(tmp_path.c_str());
            return false;
        }
        return true;
    } catch (...) {
        std::remove(tmp_path.c_str());
        return false;
    }
}

inline bool load_mime_tree(const std::string& body_path, MimePart& root);

// 取得 MIME 树：优先读 sidecar，没有就现场解析**并回写 sidecar**。
//
// 回写是关键：否则每次 FETCH 都要把整封邮件重新解析一遍。旧邮件、以及
// 超过 inbound_mime_parse_limit_bytes 而被 SMTP 侧跳过的大邮件，都走这条路。
// 回写失败无所谓，只是下次再解析一次，不影响本次结果。
inline bool ensure_mime_tree(const std::string& body_path,
                             const std::string& raw,
                             MimePart& root) {
    if (load_mime_tree(body_path, root)) {
        return true;
    }
    root = MimePart{};
    parse_mime_tree(raw, root);
    if (root.type.empty()) {
        return false;
    }
    save_mime_tree(body_path, root);
    return true;
}

// 从 sidecar JSON 加载预解析的 MIME 树
inline bool load_mime_tree(const std::string& body_path, MimePart& root) {
    std::string path = body_path + ".mime";
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (json.empty() || json[0] != '{') return false;

    std::function<void(const std::string&, size_t&, MimePart&)> parse;
    parse = [&](const std::string& s, size_t& pos, MimePart& p) {
        auto read_str = [&]() -> std::string {
            size_t start = s.find('"', pos); if (start == std::string::npos) return "";
            // 找到未被转义的收尾引号（save_mime_tree 会转义 " 和 \）
            size_t end = start + 1;
            while (end < s.size() && s[end] != '"') {
                if (s[end] == '\\' && end + 1 < s.size()) ++end;
                ++end;
            }
            if (end >= s.size()) return "";
            const std::string escaped = s.substr(start + 1, end - start - 1);
            pos = end + 1;

            std::string out;
            out.reserve(escaped.size());
            for (size_t i = 0; i < escaped.size(); ++i) {
                if (escaped[i] != '\\' || i + 1 >= escaped.size()) { out += escaped[i]; continue; }
                switch (escaped[++i]) {
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    default:   out += escaped[i]; break;
                }
            }
            return out;
        };
        auto read_uint = [&]() -> uint64_t {
            while (pos < s.size() && !std::isdigit(s[pos])) pos++;
            uint64_t v = 0;
            while (pos < s.size() && std::isdigit(s[pos])) { v = v * 10 + (s[pos] - '0'); pos++; }
            return v;
        };
        pos = s.find('{', pos); if (pos == std::string::npos) return;
        pos++; // skip {
        while (pos < s.size() && s[pos] != '}') {
            std::string key = read_str(); pos++; // skip :
            if (key == "t") p.type = read_str();
            else if (key == "s") p.subtype = read_str();
            else if (key == "c") p.charset = read_str();
            else if (key == "e") p.encoding = read_str();
            else if (key == "b") p.boundary = read_str();
            else if (key == "n") p.name = read_str();
            else if (key == "o") p.offset = read_uint();
            else if (key == "l") p.length = read_uint();
            else if (key == "z") p.body_size = read_uint();
            else if (key == "ln") p.lines = read_uint();
            else if (key == "p") {
                pos = s.find('[', pos); if (pos == std::string::npos) return;
                pos++; // skip [
                while (pos < s.size() && s[pos] != ']') {
                    MimePart sub;
                    parse(s, pos, sub);
                    if (!sub.type.empty()) p.subs.push_back(std::move(sub));
                    if (pos < s.size() && s[pos] == ',') pos++;
                }
                if (pos < s.size()) pos++; // skip ]
            }
            if (pos < s.size() && s[pos] == ',') pos++;
        }
        if (pos < s.size()) pos++; // skip }
    };
    try {
        size_t pos = 0;
        parse(json, pos, root);
        return !root.type.empty();
    } catch (...) { return false; }
}

} // namespace mail_system
