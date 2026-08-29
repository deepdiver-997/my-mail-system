#include "mail_system/back/mailServer/fsm/imaps/imap_utils.h"
#include "mail_system/back/mailServer/fsm/imaps/imap_types.hpp"   // safe_stoull
#include "mail_system/back/common/mime_parser.h"   // MimePart / parse_mime_tree
#include <algorithm>
#include <cstring>
#include <sstream>

namespace mail_system {
namespace imap_utils {

namespace {
// modified Base64 编码表（用 ',' 代替 '/', 不加 '=' 填充）
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

// modified Base64 解码表（用 ',' 代替 '/'）
const int kBase64Decode[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
};
} // namespace

// RFC 3501 date-time: DD-Mon-YYYY HH:MM:SS +ZZZZ
std::string imap_timestamp(time_t t) {
    struct tm result;
    memset(&result, 0, sizeof(result));
#ifdef _WIN32
    gmtime_s(&result, &t);
#else
    gmtime_r(&t, &result);
#endif
    static const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d-%s-%04d %02d:%02d:%02d +0000",
             result.tm_mday, months[result.tm_mon],
             result.tm_year + 1900,
             result.tm_hour, result.tm_min, result.tm_sec);
    return std::string(buf);
}

// RFC 3501 quoted-string
// 注意：IMAP-UTF-7 编码的名称（以 & 开头）也需要加引号，
// 否则 VMime 等客户端会把 &...- 中的 '-' 当作名称的一部分而非编码结束符
std::string quote_string(const std::string& s) {
    // Always quote if: empty, contains special chars, or is IMAP-UTF-7 encoded
    // IMAP-UTF-7 编码的名称（以 & 开头）需要加引号，
    // 纯 ASCII atom 不加引号
    bool needs_quote = s.empty() ||
                       s[0] == '&' ||
                       s.find_first_of("\"\\") != std::string::npos ||
                       s.find(' ') != std::string::npos;
    if (!needs_quote) {
        return s;
    }
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// UTF-8 → modified UTF-7（用于 LIST/LSUB 响应）
std::string encode_mailbox_name(const std::string& name) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "INBOX") return "INBOX";

    std::string result;
    std::vector<uint16_t> buf;

    auto flush = [&]() {
        if (buf.empty()) return;
        std::vector<uint8_t> be;
        for (uint16_t u : buf) {
            be.push_back((u >> 8) & 0xFF);
            be.push_back(u & 0xFF);
        }
        result += '&';
        for (size_t i = 0; i < be.size(); i += 3) {
            uint8_t b[3] = {0}; int n = 0;
            for (int j = 0; j < 3 && i + j < be.size(); ++j, ++n) b[j] = be[i + j];
            uint32_t t = (b[0] << 16) | (b[1] << 8) | b[2];
            result += kBase64[(t >> 18) & 0x3F];
            result += kBase64[(t >> 12) & 0x3F];
            if (n >= 2) result += kBase64[(t >> 6) & 0x3F];
            if (n >= 3) result += kBase64[t & 0x3F];
        }
        result += '-';
        buf.clear();
    };

    size_t i = 0;
    while (i < name.size()) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            flush();
            result += (c == '&') ? "&-" : std::string(1, static_cast<char>(c));
            i++;
        } else {
            uint32_t cp = 0; size_t extra = 0;
            if (c >= 0xF0)      { cp = c & 0x07; extra = 3; }
            else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
            else                { cp = c & 0x1F; extra = 1; }
            for (size_t j = 1; j <= extra; ++j)
                cp = (cp << 6) | (static_cast<unsigned char>(name[i + j]) & 0x3F);
            i += extra + 1;
            if (cp > 0xFFFF) {
                cp -= 0x10000;
                buf.push_back(0xD800 | ((cp >> 10) & 0x3FF));
                buf.push_back(0xDC00 | (cp & 0x3FF));
            } else {
                buf.push_back(static_cast<uint16_t>(cp));
            }
        }
    }
    flush();
    return result;
}

// modified UTF-7 → UTF-8（手动实现，不依赖 iconv）
// 处理 &base64- 序列以及 &- 转义
std::string decode_mailbox_name(const std::string& imap7) {
    std::string result;

    size_t i = 0;
    while (i < imap7.size()) {
        char c = imap7[i];

        if (c == '&') {
            if (i + 1 < imap7.size() && imap7[i + 1] == '-') {
                // &- → 字面 '&'
                result += '&';
                i += 2;
            } else {
                // &...- → modified Base64 片段
                size_t end = imap7.find('-', i + 1);
                if (end == std::string::npos) {
                    result += c;
                    i++;
                    continue;
                }
                std::string b64 = imap7.substr(i + 1, end - i - 1);
                i = end + 1;

                // 解码 modified Base64 → 字节
                std::vector<uint8_t> bytes;
                uint32_t acc = 0;
                int bits = 0;
                for (char bc : b64) {
                    if (bc < 0 || bc >= 128) continue;
                    int val = kBase64Decode[static_cast<int>(bc)];
                    if (val < 0) continue;
                    acc = (acc << 6) | static_cast<uint32_t>(val);
                    bits += 6;
                    if (bits >= 8) {
                        bits -= 8;
                        bytes.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
                    }
                }

                // UTF-16BE → UTF-8
                for (size_t j = 0; j + 1 < bytes.size(); j += 2) {
                    uint16_t unit = (static_cast<uint16_t>(bytes[j]) << 8)
                                   | bytes[j + 1];

                    if (unit >= 0xD800 && unit <= 0xDBFF && j + 3 < bytes.size()) {
                        // 高 surrogate + 低 surrogate
                        uint16_t low = (static_cast<uint16_t>(bytes[j + 2]) << 8)
                                      | bytes[j + 3];
                        uint32_t cp = 0x10000
                            + ((unit - 0xD800) << 10)
                            + (low - 0xDC00);
                        result += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                        j += 2; // 跳过低 surrogate
                    } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                        continue; // 孤立的 surrogate，跳过
                    } else if (unit < 0x80) {
                        result += static_cast<char>(unit);
                    } else if (unit < 0x800) {
                        result += static_cast<char>(0xC0 | ((unit >> 6) & 0x1F));
                        result += static_cast<char>(0x80 | (unit & 0x3F));
                    } else {
                        result += static_cast<char>(0xE0 | ((unit >> 12) & 0x0F));
                        result += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (unit & 0x3F));
                    }
                }
            }
        } else {
            result += c;
            i++;
        }
    }
    return result;
}

// IMAP-UTF-7 解码（与 decode_mailbox_name 原为重复实现，语义一致，委托过去）
std::string decode_imap_utf7(const std::string& imap7) {
    return decode_mailbox_name(imap7);
}

// 构建 flags 字符串
std::string build_flags_string(int status, bool starred, bool deleted, bool important) {
    std::string flags;
    // \Seen: status==0 (已读)
    if (status == 0) {
        flags += "\\Seen ";
    }
    if (starred) {
        flags += "\\Flagged ";
    }
    if (deleted) {
        flags += "\\Deleted ";
    }
    if (important) {
        flags += "\\Important ";
    }
    // 去除尾部空格
    if (!flags.empty() && flags.back() == ' ') {
        flags.pop_back();
    }
    return flags;
}

// 构建 ENVELOPE 响应
std::string build_envelope_string(
    const std::string& date_str,
    const std::string& subject,
    const std::string& from,
    const std::string& sender,
    const std::string& reply_to,
    const std::string& to,
    const std::string& cc,
    const std::string& bcc,
    const std::string& in_reply_to,
    const std::string& message_id)
{
    // ENVELOPE( date, subject, from, sender, reply-to, to, cc, bcc, in-reply-to, message-id )
    // 每个地址列表是: ((name NIL addr host) ...)
    auto addr_to_list = [](const std::string& addr) -> std::string {
        if (addr.empty()) return "NIL";
        // Parse "user@domain" or "Name <user@domain>"
        std::string name = "NIL";
        std::string user;
        std::string domain;
        std::string input = addr;

        // Try to extract name part
        size_t angle_start = input.find('<');
        size_t angle_end = input.find('>');
        std::string addr_spec;
        if (angle_start != std::string::npos && angle_end != std::string::npos) {
            std::string before_angle = input.substr(0, angle_start);
            // trim
            before_angle.erase(0, before_angle.find_first_not_of(" \t\""));
            before_angle.erase(before_angle.find_last_not_of(" \t\"") + 1);
            if (!before_angle.empty()) {
                name = "\"" + before_angle + "\"";
            }
            addr_spec = input.substr(angle_start + 1, angle_end - angle_start - 1);
        } else {
            addr_spec = input;
        }

        size_t at_pos = addr_spec.find('@');
        if (at_pos != std::string::npos) {
            user = addr_spec.substr(0, at_pos);
            domain = addr_spec.substr(at_pos + 1);
        } else {
            user = addr_spec;
            domain = "";
        }

        if (user.empty()) user = "NIL";
        if (domain.empty()) domain = "NIL";

        // ENVELOPE address fields MUST be quoted strings per RFC 3501 nstring
        auto force_quote = [](const std::string& s) -> std::string {
            return "\"" + s + "\"";
        };
        return "((" + name + " NIL " + force_quote(user) + " " + force_quote(domain) + "))";
    };

    std::string out = "(";
    out += (date_str.empty() ? "NIL" : "\"" + date_str + "\"") + " ";
    out += (subject.empty() ? "NIL" : "\"" + subject + "\"") + " ";
    out += addr_to_list(from) + " ";
    out += addr_to_list(sender.empty() ? from : sender) + " ";
    out += (reply_to.empty() ? addr_to_list(from) : addr_to_list(reply_to)) + " ";
    out += addr_to_list(to) + " ";
    out += (cc.empty() ? "NIL" : addr_to_list(cc)) + " ";
    out += (bcc.empty() ? "NIL" : addr_to_list(bcc)) + " ";
    out += (in_reply_to.empty() ? "NIL" : "\"" + in_reply_to + "\"") + " ";
    out += (message_id.empty() ? "NIL" : "\"" + message_id + "\"");
    out += ")";
    return out;
}

// 构建 BODY[] 响应
std::string build_fetch_body_response(const std::string& body_content, size_t octets) {
    (void)octets;
    if (body_content.empty()) {
        return "\"\"";
    }
    // Use literal: {size}\r\n<content>
    std::string out = "{" + std::to_string(body_content.size()) + "}\r\n";
    out += body_content;
    return out;
}

// BODYSTRUCTURE 构建（RFC 3501 §7.4.2）
std::string build_bodystructure(const std::string& raw) {
    // 旧邮件回退路径（无预解析 MIME 树时使用）：用 parse_mime_tree 解析，
    // 而非硬编码 text/plain（否则 HTML 邮件会被按纯文本渲染）。
    MimePart root;
    parse_mime_tree(raw, root);
    return build_bodystructure_tree(root);
}

std::string build_bodystructure_tree(const MimePart& mp) {
    if (mp.is_multipart()) {
        // RFC 3501 §7.4.2: multipart 子 part 在最前，subtype 在最后
        //   ((sub1)(sub2) "alternative" ("BOUNDARY" "...") NIL NIL NIL)
        std::string result = "(";
        for (const auto& sub : mp.subs)
            result += build_bodystructure_tree(sub);
        result += " \"" + mp.subtype + "\"";
        if (!mp.boundary.empty())
            result += " (\"BOUNDARY\" \"" + mp.boundary + "\")";
        else
            result += " NIL";
        result += " NIL NIL NIL)";
        return result;
    }
    std::string result = "(\"" + mp.type + "\" \"" + mp.subtype + "\"";
    if (!mp.charset.empty()) result += " (\"CHARSET\" \"" + mp.charset + "\")";
    else result += " NIL";
    if (!mp.name.empty()) result += " (\"" + mp.name + "\")";
    else result += " NIL";
    result += " NIL \"" + mp.encoding + "\" " + std::to_string(mp.body_size);
    result += " " + std::to_string(mp.lines) + " NIL NIL NIL NIL)";
    return result;
}

// 提取 MIME part 的正文：跳过该 part 自己的 MIME header，返回原始内容。
// 注意：BODY[<section>] 按 RFC 3501 返回的是原始编码内容（base64/quoted-printable 保持原样），
// 由客户端根据 BODYSTRUCTURE 的 body-fld-enc 自行解码。因此这里不做 transfer-encoding 解码。
// raw 是完整 body 文件内容（含顶层 header + multipart），part 来自预解析 MimePart 树
// （offset/length 为该 part 在 raw 中的字节区间，含其自己的 MIME header）。
std::string extract_part_content(const std::string& raw, const MimePart& part) {
    size_t start = part.offset;
    size_t end = part.offset + part.length;
    if (start >= raw.size()) return {};
    if (end > raw.size()) end = raw.size();
    if (end <= start) return {};

    // 在 part 范围内定位 header/body 分隔
    size_t sep = raw.find("\r\n\r\n", start);
    if (sep == std::string::npos || sep >= end) {
        sep = raw.find("\n\n", start);
        if (sep == std::string::npos || sep >= end) return {};
    }
    size_t body_start = sep + (raw[sep] == '\r' ? 4 : 2);
    // 兼容旧 sidecar：非 multipart 根 part 的 length 可能只覆盖 header
    // （parse_mime_tree 的旧 bug）。此时 body 应从 header 之后延伸到消息末尾。
    if (body_start >= end && start == 0) end = raw.size();
    if (body_start >= end) return {};

    std::string body = raw.substr(body_start, end - body_start);
    // 去掉尾部空白行
    while (!body.empty() && (body.back() == '\r' || body.back() == '\n'))
        body.pop_back();
    return body;
}

// 展开逗号分隔的序列号集（支持 "1" / "1:*" / "1,3,5" / "1:3,5" / "*"）
// 输出到 ranges（(start,end) 闭区间列表，已 clamp 到 [1,total]）
void expand_seq_set(const std::string& seq_set, size_t total,
                    std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    ranges.clear();
    std::istringstream ss(seq_set);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        uint64_t s, e;
        if (token.find(':') != std::string::npos) {
            size_t c = token.find(':');
            std::string a = token.substr(0, c), b = token.substr(c + 1);
            s = (a == "*") ? 1 : safe_stoull(a);
            e = (b == "*") ? total : std::min((uint64_t)safe_stoull(b), (uint64_t)total);
        } else if (token == "*") {
            s = 1;
            e = total;
        } else {
            s = safe_stoull(token);
            e = s;
        }
        if (s < 1) s = 1;
        if (e > total) e = total;
        if (s <= e) ranges.emplace_back(s, e);
    }
}

// 数值 ID 列表 → "a,b,c"（批量 IN 语句用；全数值，无注入面）
std::string join_mail_ids(const std::vector<uint64_t>& ids) {
    std::string out;
    bool first = true;
    for (uint64_t id : ids) {
        if (!first) out += ",";
        out += std::to_string(id);
        first = false;
    }
    return out;
}

} // namespace imap_utils
} // namespace mail_system
