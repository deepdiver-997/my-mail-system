#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_types.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>

namespace mail_system {
namespace algorithm {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string sanitize_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ' ') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out.empty() ? std::string("attachment") : out;
}

std::string ensure_trailing_slash(std::string path) {
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    return path;
}

std::string extract_boundary(const std::string& content_type) {
    std::regex boundary_regex(R"(boundary\s*=\s*\"?([^";]+)\"?)", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(content_type, m, boundary_regex) && m.size() > 1) {
        return m[1];
    }
    return "";
}

std::unordered_map<std::string, std::string> parse_headers_map(const std::string& header_block) {
    std::unordered_map<std::string, std::string> headers;
    std::istringstream iss(header_block);
    std::string line;
    std::string last_key;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // RFC 5322 folded header continuation line.
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            if (!last_key.empty()) {
                headers[last_key] += " " + trim(line);
            }
            continue;
        }

        auto pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        std::string key = to_lower(line.substr(0, pos));
        std::string value = line.substr(pos + 1);
        size_t first = value.find_first_not_of(" \t");
        if (first != std::string::npos) {
            value = value.substr(first);
        } else {
            value.clear();
        }
        headers[key] = value;
        last_key = key;
    }
    return headers;
}

std::string get_header_value(const std::unordered_map<std::string, std::string>& headers, const std::string& key) {
    auto it = headers.find(to_lower(key));
    if (it != headers.end()) {
        return it->second;
    }
    return "";
}

std::string decode_base64(const std::string& input) {
    static const int decode_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::string out;
    int val = 0;
    int bits = -8;
    for (unsigned char c : input) {
        if (decode_table[c] == -1) {
            continue;
        }
        val = (val << 6) + decode_table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// ========== 邮件解析函数实现 ==========

void analyze_top_header(SmtpsContext& ctx) {
    LOG_SESSION_DEBUG("analyze_top_header: header_buffer=[{}]", ctx.header_buffer.substr(0, 200));
    auto headers = parse_headers_map(ctx.header_buffer);
    ctx.parsed_subject = get_header_value(headers, "subject");
    ctx.source_message_id = get_header_value(headers, "message-id");

    if (!ctx.parsed_subject.empty()) {
        LOG_SESSION_DEBUG("analyze_top_header: subject=[{}]", ctx.parsed_subject);
    }
    if (!ctx.source_message_id.empty()) {
        LOG_SESSION_DEBUG("analyze_top_header: message-id=[{}]", ctx.source_message_id);
    }

    auto it = headers.find("content-type");
    if (it != headers.end()) {
        LOG_SESSION_DEBUG("analyze_top_header: content-type raw=[{}]", it->second);
        std::string lower = to_lower(it->second);
        LOG_SESSION_DEBUG("analyze_top_header: content-type lower=[{}]", lower);
        ctx.boundary = extract_boundary(lower);
        LOG_SESSION_DEBUG("analyze_top_header: extracted boundary=[{}]", ctx.boundary);
        if (lower.find("multipart/") != std::string::npos && !ctx.boundary.empty()) {
            ctx.multipart = true;
            ctx.streaming_enabled = true;
            LOG_SESSION_DEBUG("Multipart detected, boundary: {}", ctx.boundary);
        } else {
            LOG_SESSION_DEBUG("Not multipart or no boundary, content-type: {}", lower);
        }
    }
}

void handle_part_header_parsed(SmtpsContext& ctx) {
    LOG_SESSION_DEBUG("handle_part_header_parsed: headers=[{}]", ctx.current_part_headers);
    auto headers = parse_headers_map(ctx.current_part_headers);
    std::string disp = to_lower(headers["content-disposition"]);
    ctx.current_part_encoding = to_lower(headers["content-transfer-encoding"]);
    ctx.current_part_mime = to_lower(headers["content-type"]);

    LOG_SESSION_DEBUG("handle_part_header_parsed: disposition=[{}], mime=[{}], encoding=[{}]",
                     disp, ctx.current_part_mime, ctx.current_part_encoding);

    ctx.current_part_is_attachment = disp.find("attachment") != std::string::npos || disp.find("filename") != std::string::npos;
    if (ctx.current_part_is_attachment) {
        ctx.has_attachment = true;
        std::string filename;
        std::regex filename_regex(R"(filename\s*=\s*\"?([^";]+)\"?)", std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(ctx.current_part_headers, m, filename_regex) && m.size() > 1) {
            filename = m[1];
        }
        if (filename.empty()) {
            filename = "attachment";
        }
        ctx.current_attachment_filename = sanitize_filename(filename);
        LOG_SESSION_DEBUG("Part detected as attachment: {}", ctx.current_attachment_filename);
    } else {
        ctx.current_attachment_filename.clear();
        ctx.current_attachment_path.clear();
    }
}

void handle_multipart_line(SmtpsContext& ctx, const std::string& line) {
    if (ctx.body_limit_exceeded) {
        return;
    }

    if (!ctx.multipart || ctx.boundary.empty()) {
        LOG_SESSION_DEBUG("Multipart disabled or no boundary, appending to mail_data");
        ctx.mail_data += line + "\r\n";
        return;
    }

    std::string boundary_marker = "--" + ctx.boundary;
    std::string boundary_end = boundary_marker + "--";
    std::string line_lower = to_lower(line);

    LOG_SESSION_DEBUG("Checking line: [{}] vs boundary_marker: [{}]", line_lower, boundary_marker);

    if (line_lower == boundary_marker || line_lower == boundary_end) {
        LOG_SESSION_DEBUG("Boundary detected: [{}]", line_lower);
        // 附件元数据的收尾在 DATA_END（SmtpsSession::finalize_attachment_from_context）完成
        if (line_lower == boundary_end) {
            LOG_SESSION_DEBUG("Multipart end detected");
            ctx.in_part_header = false;
            ctx.current_part_headers.clear();
            return;
        }
        LOG_SESSION_DEBUG("New part starting");
        ctx.in_part_header = true;
        ctx.current_part_headers.clear();
        return;
    }

    if (ctx.in_part_header) {
        if (line.empty()) {
            LOG_SESSION_DEBUG("Part header complete, processing headers: [{}]", ctx.current_part_headers.substr(0, 100));
            ctx.in_part_header = false;
            handle_part_header_parsed(ctx);
            LOG_SESSION_DEBUG("After handle_part_header: mime=[{}], is_attachment={}, filename=[{}]",
                             ctx.current_part_mime, ctx.current_part_is_attachment, 
                             ctx.current_attachment_filename);
        } else {
            LOG_SESSION_DEBUG("Part header line: [{}]", line);
            ctx.current_part_headers += line + "\r\n";
        }
        return;
    }

    if (ctx.current_part_is_attachment) {
        // 附件字节已随整封邮件落盘到 body 文件，这里只跳过，不进 text_body_buffer
        LOG_SESSION_DEBUG("Attachment body line skipped (stored in body file): [{}]", line.substr(0, 60));
        return;
    }

    // 文本内容处理
    size_t added = line.size() + 2;
    const size_t MAX_BODY_BYTES = 10 * 1024 * 1024;
    if (ctx.text_body_size + added > MAX_BODY_BYTES) {
        LOG_SESSION_DEBUG("Body size exceeds limit, marking as exceeded");
        return;
    }
    ctx.text_body_size += added;
    if (ctx.current_part_mime.find("text/plain") != std::string::npos) {
        ctx.text_body_buffer += line + "\r\n";
    } else if (ctx.text_body_buffer.empty()) {
        ctx.text_body_buffer += line + "\r\n";
    }
}

void process_message_data(SmtpsContext& ctx, const std::string& data) {
    ctx.line_buffer += data;
    LOG_SESSION_DEBUG("process_message_data: buffer size={}, streaming_enabled={}",
                     ctx.line_buffer.size(), ctx.streaming_enabled);

    while (true) {
        size_t pos = ctx.line_buffer.find("\r\n");
        if (pos == std::string::npos) {
            break;
        }
        std::string line = ctx.line_buffer.substr(0, pos);
        ctx.line_buffer.erase(0, pos + 2);

        if (ctx.body_limit_exceeded) {
            continue;
        }

        if (!ctx.header_parsed) {
            ctx.header_buffer += line + "\r\n";
            if (line.empty()) {
                ctx.header_parsed = true;
                analyze_top_header(ctx);
                LOG_SESSION_DEBUG("Header parsing complete, multipart={}, boundary=[{}], streaming={}",
                                 ctx.multipart, ctx.boundary, ctx.streaming_enabled);
            }
            continue;
        }

        if (ctx.streaming_enabled) {
            LOG_SESSION_DEBUG("Processing body line (streaming): [{}]", line.substr(0, 60));
            handle_multipart_line(ctx, line);
        } else {
            LOG_SESSION_DEBUG("Processing body line (buffered): [{}]", line.substr(0, 60));
            const size_t MAX_BODY_BYTES = 10 * 1024 * 1024;
            ctx.buffered_body_size += line.size() + 2;
            if (ctx.buffered_body_size > MAX_BODY_BYTES) {
                LOG_SESSION_DEBUG("Buffered body size exceeds limit");
                ctx.body_limit_exceeded = true;
            } else {
                ctx.mail_data += line + "\r\n";
            }
        }
    }
}

void cleanup_streamed_attachments(SmtpsContext& ctx) {
    for (auto& att : ctx.streamed_attachments) {
        if (!att.filepath.empty()) {
            std::remove(att.filepath.c_str());
        }
    }
    ctx.streamed_attachments.clear();
}
} // namespace algorithm
} // namespace mail_system