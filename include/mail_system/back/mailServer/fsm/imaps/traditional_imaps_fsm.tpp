#ifndef TRADITIONAL_IMAPS_FSM_TPP
#define TRADITIONAL_IMAPS_FSM_TPP

#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/common/mime_parser.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace mail_system {

// 展开逗号分隔的序列号集（支持 "1" / "1:*" / "1,3,5" / "1:3,5" / "*"）
// 输出到 ranges（(start,end) 闭区间列表，已 clamp 到 [1,total]）
namespace {
inline void expand_seq_set(const std::string& seq_set, size_t total,
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
inline std::string join_mail_ids(const std::vector<uint64_t>& ids) {
    std::string out;
    bool first = true;
    for (uint64_t id : ids) {
        if (!first) out += ",";
        out += std::to_string(id);
        first = false;
    }
    return out;
}
} // namespace

// ====================================================================
// 工具方法
// ====================================================================

// RFC 3501 date-time: DD-Mon-YYYY HH:MM:SS +ZZZZ
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::imap_timestamp(time_t t) {
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
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::quote_string(const std::string& s) {
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

// ====================================================================
// 邮箱名编解码
//
// RFC 3501 §5.1.3: IMAP mailbox 名称必须用 modified UTF-7 编码。
// 编码方式：非 ASCII 连续块 → 取 UTF-16BE → modified Base64 → &...-
// (modified Base64: 用 ',' 代替 '/', 不加 '=' 填充)
// ====================================================================

// 辅助：modified Base64 编码表 & 解码表
namespace {
    const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
}

// UTF-8 → modified UTF-7（用于 LIST/LSUB 响应）
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::encode_mailbox_name(const std::string& name) {
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

// 辅助：Base64 解码表（modified: 用 ',' 代替 '/'）
namespace {
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
}

// modified UTF-7 → UTF-8（手动实现，不依赖 iconv）
// 处理 &base64- 序列以及 &- 转义
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::decode_mailbox_name(const std::string& imap7) {
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

// 构建 flags 字符串
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_flags_string(
    int status, bool starred, bool deleted, bool important)
{
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
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_envelope_string(
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
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_fetch_body_response(
    const std::string& body_content, size_t octets)
{
    if (body_content.empty()) {
        return "\"\"";
    }
    // Use literal: {size}\r\n<content>
    std::string out = "{" + std::to_string(body_content.size()) + "}\r\n";
    out += body_content;
    return out;
}

// ====================================================================
// BODYSTRUCTURE 构建（RFC 3501 §7.4.2）
// ====================================================================
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_bodystructure(const std::string& raw) {
    // 旧邮件回退路径（无预解析 MIME 树时使用）：用 parse_mime_tree 解析，
    // 而非硬编码 text/plain（否则 HTML 邮件会被按纯文本渲染）。
    MimePart root;
    parse_mime_tree(raw, root);
    return build_bodystructure_tree(root);
}

template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_bodystructure_tree(const MimePart& mp) {
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
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::extract_part_content(
    const std::string& raw, const MimePart& part)
{
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

// ====================================================================
// 发送 IMAP 响应
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_untagged(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    const std::string& data)
{
    session->do_async_write("* " + data + "\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) {
            if (!ec) s->do_async_read();
        }
    );
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_tagged(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    const std::string& tag,
    const std::string& status,
    const std::string& message)
{
    session->do_async_write(tag + " " + status + " " + message + "\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) {
            if (!ec) s->do_async_read();
        }
    );
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_continuation(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    const std::string& message)
{
    session->do_async_write("+ " + message + "\r\n",
        nullptr
    );
}

// ====================================================================
// 初始化：转换表 / 处理器
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::init_transition_table() {
    // INIT → NOT_AUTHENTICATED (on CONNECT)
    this->add_transition(ImapState::INIT, ImapEvent::CONNECT, ImapState::NOT_AUTHENTICATED);

    // NOT_AUTHENTICATED: stay on same state for most commands
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::CAPABILITY, ImapState::NOT_AUTHENTICATED);
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGIN, ImapState::NOT_AUTHENTICATED); // may transition in handler
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::AUTHENTICATE, ImapState::NOT_AUTHENTICATED);
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::NOOP, ImapState::NOT_AUTHENTICATED);
    this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGOUT, ImapState::LOGOUT);
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>)
        this->add_transition(ImapState::NOT_AUTHENTICATED, ImapEvent::STARTTLS, ImapState::NOT_AUTHENTICATED);

    // AUTHENTICATED
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::SELECT, ImapState::SELECTED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::EXAMINE, ImapState::SELECTED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CAPABILITY, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::LIST, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::LSUB, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::IMAP_STATUS, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CREATE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::DELETE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::RENAME, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::SUBSCRIBE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::UNSUBSCRIBE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::APPEND, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CHECK, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::CLOSE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::NOOP, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::IDLE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::AUTHENTICATED, ImapEvent::LOGOUT, ImapState::LOGOUT);

    // SELECTED
    this->add_transition(ImapState::SELECTED, ImapEvent::FETCH, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::STORE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::SEARCH, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::COPY, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::MOVE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::UID, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::EXPUNGE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::CLOSE, ImapState::AUTHENTICATED);
    this->add_transition(ImapState::SELECTED, ImapEvent::CHECK, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::CAPABILITY, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::NOOP, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::APPEND, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::IDLE, ImapState::SELECTED);
    this->add_transition(ImapState::SELECTED, ImapEvent::LOGOUT, ImapState::LOGOUT);

    // ERROR / TIMEOUT everywhere (remain in current state)
    for (int i = 0; i <= static_cast<int>(ImapState::SELECTED); ++i) {
        auto s = static_cast<ImapState>(i);
        this->add_transition(s, ImapEvent::ERROR, s);
        this->add_transition(s, ImapEvent::TIMEOUT, s);
    }
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::init_state_handlers() {
    // INIT
    this->add_handler(ImapState::INIT, ImapEvent::CONNECT, [this](auto session) { handle_init_connect(session); });

    // NOT_AUTHENTICATED
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::CAPABILITY, [this](auto session) { handle_capability(session); });
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>)
        this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::STARTTLS, [this](auto session) { handle_starttls(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGIN, [this](auto session) { handle_login(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::AUTHENTICATE, [this](auto session) { handle_authenticate(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::NOOP, [this](auto session) { handle_noop(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::LOGOUT, [this](auto session) { handle_logout(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::ERROR, [this](auto session) { handle_error(session); });
    this->add_handler(ImapState::NOT_AUTHENTICATED, ImapEvent::TIMEOUT, [this](auto session) { handle_timeout(session); });

    // AUTHENTICATED
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CAPABILITY, [this](auto session) { handle_capability(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::SELECT, [this](auto session) { handle_select(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::EXAMINE, [this](auto session) { handle_examine(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::LIST, [this](auto session) { handle_list(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::LSUB, [this](auto session) { handle_lsub(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::IMAP_STATUS, [this](auto session) { handle_status(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CREATE, [this](auto session) { handle_create(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::DELETE, [this](auto session) { handle_delete(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::RENAME, [this](auto session) { handle_rename(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::SUBSCRIBE, [this](auto session) { handle_subscribe(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::UNSUBSCRIBE, [this](auto session) { handle_unsubscribe(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::APPEND, [this](auto session) { handle_append(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CHECK, [this](auto session) { handle_check(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::CLOSE, [this](auto session) { handle_close(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::NOOP, [this](auto session) { handle_noop(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::IDLE, [this](auto session) { handle_idle(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::LOGOUT, [this](auto session) { handle_logout(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::ERROR, [this](auto session) { handle_error(session); });
    this->add_handler(ImapState::AUTHENTICATED, ImapEvent::TIMEOUT, [this](auto session) { handle_timeout(session); });

    // SELECTED
    this->add_handler(ImapState::SELECTED, ImapEvent::FETCH, [this](auto session) { handle_fetch(session, false); });
    this->add_handler(ImapState::SELECTED, ImapEvent::STORE, [this](auto session) { handle_store(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::SEARCH, [this](auto session) { handle_search(session, false); });
    this->add_handler(ImapState::SELECTED, ImapEvent::COPY, [this](auto session) { handle_copy(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::MOVE, [this](auto session) { handle_move(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::UID, [this](auto session) { handle_uid(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::EXPUNGE, [this](auto session) { handle_expunge(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::CLOSE, [this](auto session) { handle_close(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::CHECK, [this](auto session) { handle_check(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::APPEND, [this](auto session) { handle_append(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::CAPABILITY, [this](auto session) { handle_capability(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::NOOP, [this](auto session) { handle_noop(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::IDLE, [this](auto session) { handle_idle(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::LOGOUT, [this](auto session) { handle_logout(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::ERROR, [this](auto session) { handle_error(session); });
    this->add_handler(ImapState::SELECTED, ImapEvent::TIMEOUT, [this](auto session) { handle_timeout(session); });
}

// ====================================================================
// 事件派发
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::process_event(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    ImapEvent event,
    const std::string& tag)
{
    if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) {
        LOG_IMAP_DETAIL_DEBUG("Current State: {}, Event: {}, Tag: {}",
                          TraditionalImapsFsm<ConnectionType>::get_state_name(
                              static_cast<ImapState>(session->get_current_state())),
                          TraditionalImapsFsm<ConnectionType>::get_event_name(event),
                          tag);
    }

    // 保存 tag 到 context
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (ctx) {
        ctx->current_tag = tag;
    }

    this->dispatch(session, static_cast<ImapState>(session->get_current_state()), event);
}
template <typename ConnectionType>
bool TraditionalImapsFsm<ConnectionType>::is_terminal_state(ImapState s) const {
    return s == ImapState::LOGOUT || s == ImapState::CLOSED;
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::on_invalid_transition(
    ImapState, ImapEvent,
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    send_tagged(session, ctx ? ctx->current_tag : "*", "BAD", "Invalid command sequence");
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::on_handler_not_found(
    ImapState, ImapEvent,
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    send_tagged(session, ctx ? ctx->current_tag : "*", "BAD", "Unsupported command in current state");
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::invoke_handler(
    typename FsmBase<ConnectionType, ImapState, ImapEvent>::Handler& h,
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    try {
        h(session);
    } catch (const std::exception& e) {
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        LOG_IMAP_ERROR("IMAP handler exception: {} tag={}", e.what(),
                       ctx ? ctx->current_tag : "?");
        send_tagged(session, ctx ? ctx->current_tag : "*", "NO", "Internal server error");
    } catch (...) {
        auto* ctx = static_cast<ImapContext*>(session->get_context());
        LOG_IMAP_ERROR("IMAP handler unknown exception tag={}",
                       ctx ? ctx->current_tag : "?");
        send_tagged(session, ctx ? ctx->current_tag : "*", "NO", "Internal server error");
    }
}


template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::auto_process_event(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (!ctx) return;

    // 如果是 IDLE 状态的特殊处理
    if (ctx->idle_mode) {
        handle_done(session);
        return;
    }

    // 从 session 获取待处理事件
    // 注意：session 中 event 和 args 在 parse 阶段已设置好
    // 这里需要一个适配: 从 session 获取 event type 和 args
    // ImapsSession 会在 parse 时设置这些值
    // 但现在 process_event 签名多了 tag 参数，auto_process_event 需要适配

    // 对于 IMAP，会话解析完命令后直接调用 process_event，
    // 这里留空作为兼容接口
}

// ====================================================================
// 状态处理器实现
// ====================================================================

// ---------- INIT → CONNECT → greeting ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_init_connect(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));

    // 发送 IMAP 欢迎语
    session->do_async_write("* OK IMAP4rev1 Server Ready\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> self,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending IMAP greeting: {}", ec.message());
                return;
            }
            LOG_IMAP_DEBUG("Sent IMAP greeting, waiting for commands...");
            self->do_async_read();
        }
    );
}

// ---------- CAPABILITY ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_capability(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string caps = "* CAPABILITY IMAP4rev1";
    // 非 SSL 连接可以通告 STARTTLS
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>) {
        caps += " STARTTLS";
    }
    caps += " IDLE UIDPLUS MOVE\r\n";

    caps += tag + " OK CAPABILITY completed\r\n";

    session->do_async_write(caps,
        nullptr
    );
}

// ---------- LOGIN ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_login(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // 解析 "username password"
    std::string username, password, args = session->get_last_command_args();
    size_t space = args.find(' ');
    if (space != std::string::npos && space > 0) {
        // 可能带引号
        if (args[0] == '"') {
            size_t end_quote = args.find('"', 1);
            if (end_quote != std::string::npos) {
                username = args.substr(1, end_quote - 1);
                password = args.substr(end_quote + 2); // skip " and space
            }
        } else {
            username = args.substr(0, space);
            password = args.substr(space + 1);
        }
        // trim password
        if (!password.empty() && password[0] == '"') {
            size_t end = password.find('"', 1);
            if (end != std::string::npos) {
                password = password.substr(1, end - 1);
            }
        }
    } else {
        username = args;
    }

    // 自动补 @domain（兼容只传本地部分的客户端）
    if (!username.empty() && username.find('@') == std::string::npos) {
        auto config = std::atomic_load(&session->get_server()->m_config);
        username += "@" + config->system_domain;
        LOG_IMAP_DEBUG("Auto-domain applied: {}@{}", username, config->system_domain);
    }

    // bcrypt 是几十到几百毫秒的纯 CPU：绝不能在共享 io 线程上算 ——
    // 那会把同线程上所有其他会话的读写/握手/定时器全部卡住。
    // pause 后把「查 DB + 验密码」整体丢给 worker 线程池；回调在 worker
    // 线程触发，pause 期间独占 session（SPF/DNS/commit 回调同一约定）。
    // 登录者自己的延迟不变（bcrypt 该算多久还是多久），变的是别人不再陪等。
    auto router = this->m_shardRouter;
    auto auth_cache = this->m_authCache;
    auto worker = this->m_workerThreadPool;
    if (!worker) {
        send_tagged(session, tag, "NO", "Server not ready");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    worker->post(
        [self, tag = std::move(tag), username = std::move(username),
         password = std::move(password), router, auth_cache]() mutable {
            // async CPS 链（仿 POP3/SMTP auth）：每步 async_query + 回调续作。
            // 底层 MySQL async_* 是默认同步包装（回调内联执行），链在 worker
            // 线程跑完；将来接真异步 DB，回调在 DB 线程触发，conn 由链内
            // shared ScopedConnection 保活，调用方结构无需改动。
            TraditionalImapsFsm<ConnectionType>::auth_user_async(
                router, auth_cache, username, password,
                [self, tag, username = username](
                    bool ok, uint64_t user_id, int shard) mutable {
                    if (!self || self->is_closed()) return;
                    if (ok) {
                        auto* c = static_cast<ImapContext*>(self->get_context());
                        if (c) {
                            c->is_authenticated = true;
                            c->username = username;
                            c->user_id = user_id;
                            c->shard_index = shard;
                        }
                        self->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
                        send_tagged(self, tag, "OK", "LOGIN completed");
                        LOG_IMAP_INFO("IMAP login successful: {} (user_id={})", username, user_id);
                    } else {
                        LOG_IMAP_WARN("IMAP login failed: {}", username);
                        if (self->record_auth_failure_and_check()) {
                            self->close();
                            return;
                        }
                        send_tagged(self, tag, "NO", "LOGIN failed");
                    }
                    self->drain_buffered_commands();
                });
        });
}

// ---------- AUTHENTICATE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_authenticate(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // 解析机制名
    std::string mechanism = session->get_last_command_args();
    size_t space = mechanism.find(' ');
    if (space != std::string::npos) {
        mechanism = mechanism.substr(0, space);
    }
    std::transform(mechanism.begin(), mechanism.end(), mechanism.begin(), ::toupper);

    if (mechanism == "LOGIN") {
        // AUTHENTICATE LOGIN → 直接转发到 LOGIN 逻辑
        // args 已经是 "LOGIN" 或 "LOGIN <base64>"
        // 简化版：通知客户端用 LOGIN 命令
        send_tagged(session, tag, "NO", "Use LOGIN command directly (AUTHENTICATE LOGIN not yet implemented)");
    } else {
        send_tagged(session, tag, "NO", "Unsupported authentication mechanism: " + mechanism);
    }
}

// ---------- LOGOUT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_logout(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string current_tag = ctx ? ctx->current_tag : "*";
    ctx->clear();

    session->set_trace_clean_close();   // 正常 LOGOUT → 连接追踪丢弃
    session->set_current_state(static_cast<int>(ImapState::LOGOUT));

    std::string response = "* BYE IMAP4rev1 Server logging out\r\n";
    response += current_tag + " OK LOGOUT completed\r\n";

    session->do_async_write(response,
        [](std::shared_ptr<SessionBase<ConnectionType>> s,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending LOGOUT reply: {}", ec.message());
                return;
            }
            auto io_ctx = s->get_server()->get_io_context();
            if (io_ctx) {
                auto timer = std::make_shared<boost::asio::steady_timer>(*io_ctx);
                timer->expires_after(std::chrono::milliseconds(100));
                timer->async_wait([s = std::move(s), timer](const boost::system::error_code& ec) mutable {
                    if (!ec) s->close();
                });
            } else {
                s->close();  // no io_context (test mock) → close immediately
            }
        }
    );
}

// ---------- SELECT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_select(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos)
            mailbox_name = mailbox_name.substr(1, end - 1);
    }
    mailbox_name = this->decode_mailbox_name(mailbox_name);

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 统计（缓存感知）→ 组装 SELECT 响应
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, tag = std::move(tag), conn, user_id,
         mailbox_name = mailbox_name](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            auto* ctx = static_cast<ImapContext*>(self->get_context());
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found: " + mailbox_name);
                self->drain_buffered_commands();
                return;
            }

            // Save selected mailbox info
            ctx->mailbox_selected = true;
            ctx->selected_mailbox_name = mailbox_name;
            ctx->selected_mailbox_id = mailbox_id;
            ctx->read_only = false;
            // Generate UIDVALIDITY (use mailbox_id as validity)
            ctx->uid_validity = mailbox_id;

            this->get_mailbox_stats_cached_async(
                conn, user_id, mailbox_id,
                [self, tag = std::move(tag), ctx](MailboxCacheEntry stats, bool, bool) {
                    if (!self || self->is_closed()) return;
                    size_t count = stats.exists;
                    size_t unseen = stats.unseen;
                    uint64_t uidnext = stats.uidnext;

                    self->set_current_state(static_cast<int>(ImapState::SELECTED));

                    // Build SELECT response (RFC 3501: [READ-WRITE]/[READ-ONLY] on tagged OK)
                    std::string response;
                    response += "* " + std::to_string(count) + " EXISTS\r\n";
                    // 我们没有跟踪 \Recent 标志（自上次访问后到达的邮件），报告 0。
                    // 之前用 count - unseen（已读数量）是错的：只要有已读邮件就恒 > 0，
                    // 导致客户端一直认为有新邮件而反复重同步。
                    response += "* 0 RECENT\r\n";
                    if (unseen > 0) {
                        response += "* OK [UNSEEN " + std::to_string(count - unseen + 1) + "]\r\n";
                    }
                    response += "* OK [UIDVALIDITY " + std::to_string(ctx->uid_validity) + "]\r\n";
                    response += "* OK [UIDNEXT " + std::to_string(uidnext) + "]\r\n";
                    response += tag + " OK [";
                    response += ctx->read_only ? "READ-ONLY" : "READ-WRITE";
                    response += "] SELECT completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- EXAMINE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_examine(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 统计 → 组装 EXAMINE 响应
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, tag = std::move(tag), conn, user_id,
         mailbox_name = mailbox_name](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            auto* ctx = static_cast<ImapContext*>(self->get_context());
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found: " + mailbox_name);
                self->drain_buffered_commands();
                return;
            }

            ctx->mailbox_selected = true;
            ctx->selected_mailbox_name = mailbox_name;
            ctx->selected_mailbox_id = mailbox_id;
            ctx->read_only = true;
            ctx->uid_validity = mailbox_id;

            this->get_mailbox_stats_cached_async(
                conn, user_id, mailbox_id,
                [self, tag = std::move(tag), ctx](MailboxCacheEntry stats, bool, bool) {
                    if (!self || self->is_closed()) return;
                    size_t count = stats.exists;
                    size_t unseen = stats.unseen;
                    uint64_t uidnext = stats.uidnext;

                    self->set_current_state(static_cast<int>(ImapState::SELECTED));

                    std::string response;
                    response += "* " + std::to_string(count) + " EXISTS\r\n";
                    response += "* 0 RECENT\r\n";
                    if (unseen > 0) {
                        response += "* OK [UNSEEN " + std::to_string(count - unseen + 1) + "]\r\n";
                    }
                    response += "* OK [UIDVALIDITY " + std::to_string(ctx->uid_validity) + "]\r\n";
                    response += "* OK [UIDNEXT " + std::to_string(uidnext) + "]\r\n";
                    response += "* OK [READ-ONLY]\r\n";
                    response += tag + " OK EXAMINE completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- LIST ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_list(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // LIST 参数: reference mailbox_name
    // 简单实现：列出用户所有邮箱
    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailboxes_async(
        conn, user_id,
        [self, tag = std::move(tag)](bool ok,
            std::vector<std::tuple<uint64_t, std::string, int>> mailboxes) mutable {
            if (!self || self->is_closed()) return;
            if (!ok) {
                send_tagged(self, tag, "OK", "LIST completed");
                self->drain_buffered_commands();
                return;
            }
            std::string response;
            for (const auto& mb : mailboxes) {
                const std::string& name = std::get<1>(mb);
                int box_type = std::get<2>(mb);

                std::string encoded_name = encode_mailbox_name(name);

                // 如果是收件箱（box_type=1），也要以 INBOX 形式呈现
                if (box_type == 1) {
                    // 发送 INBOX 和中文名两个条目
                    response += "* LIST (\\HasNoChildren) \"/\" INBOX\r\n";
                    response += "* LIST (\\HasNoChildren) \"/\" " + quote_string(encoded_name) + "\r\n";
                    continue;
                }
                std::string attrs = "()";
                response += "* LIST " + attrs + " \"/\" " + quote_string(encoded_name) + "\r\n";
            }
            response += tag + " OK LIST completed\r\n";

            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
            self->drain_buffered_commands();
        });
}

// ---------- LSUB ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_lsub(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // Simplified: return all mailboxes as subscribed
    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailboxes_async(
        conn, user_id,
        [self, tag = std::move(tag)](bool,
            std::vector<std::tuple<uint64_t, std::string, int>> mailboxes) mutable {
            if (!self || self->is_closed()) return;
            std::string response;
            for (const auto& mb : mailboxes) {
                const std::string& name = std::get<1>(mb);
                int box_type = std::get<2>(mb);
                std::string encoded_name = encode_mailbox_name(name);

                if (box_type == 1) {
                    response += "* LSUB (\\HasNoChildren) \"/\" INBOX\r\n";
                }
                response += "* LSUB () \"/\" " + quote_string(encoded_name) + "\r\n";
            }
            response += tag + " OK LSUB completed\r\n";

            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
            self->drain_buffered_commands();
        });
}

// ---------- STATUS ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_status(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // Parse "mailbox (MESSAGES UNSEEN UIDNEXT UIDVALIDITY)"
    std::string args = session->get_last_command_args();
    std::string mailbox_name = args;
    std::string status_attrs;

    size_t paren_open = args.find('(');
    if (paren_open != std::string::npos) {
        mailbox_name = args.substr(0, paren_open);
        // trim
        mailbox_name.erase(mailbox_name.find_last_not_of(" \t") + 1);
        size_t paren_close = args.find(')', paren_open);
        if (paren_close != std::string::npos) {
            status_attrs = args.substr(paren_open + 1, paren_close - paren_open - 1);
        }
    }

    // Trim quotes from mailbox name
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 统计 → 组装 STATUS 响应
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, tag = std::move(tag), conn, user_id,
         mailbox_name = mailbox_name,
         status_attrs = std::move(status_attrs)](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found");
                self->drain_buffered_commands();
                return;
            }
            this->get_mailbox_stats_cached_async(
                conn, user_id, mailbox_id,
                [self, tag = std::move(tag), mailbox_id, mailbox_name = std::move(mailbox_name),
                 status_attrs = std::move(status_attrs)](MailboxCacheEntry stats, bool, bool) mutable {
                    if (!self || self->is_closed()) return;
                    size_t messages = stats.exists;
                    size_t unseen = stats.unseen;
                    uint64_t uidnext = stats.uidnext;
                    uint64_t uidvalidity = mailbox_id;

                    std::transform(status_attrs.begin(), status_attrs.end(), status_attrs.begin(), ::toupper);

                    std::string response = "* STATUS " + quote_string(encode_mailbox_name(mailbox_name)) + " (";
                    if (status_attrs.find("MESSAGES") != std::string::npos || status_attrs.empty()) {
                        response += "MESSAGES " + std::to_string(messages) + " ";
                    }
                    if (status_attrs.find("UNSEEN") != std::string::npos || status_attrs.empty()) {
                        response += "UNSEEN " + std::to_string(unseen) + " ";
                    }
                    if (status_attrs.find("UIDNEXT") != std::string::npos || status_attrs.empty()) {
                        response += "UIDNEXT " + std::to_string(uidnext) + " ";
                    }
                    if (status_attrs.find("UIDVALIDITY") != std::string::npos || status_attrs.empty()) {
                        response += "UIDVALIDITY " + std::to_string(uidvalidity) + " ";
                    }
                    // Remove trailing space and close
                    if (response.back() == ' ') response.pop_back();
                    response += ")\r\n";
                    response += tag + " OK STATUS completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- FETCH ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_fetch(
    std::shared_ptr<SessionBase<ConnectionType>> session, bool is_uid)
{
    LOG_IMAP_INFO("FETCH ENTER args=[{}]", session->get_last_command_args());
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // Parse: <sequence-set> <message-data-item-names>
    auto args = (ctx && ctx->is_uid_command) ? ctx->uid_overridden_args : session->get_last_command_args();
    size_t space = args.find(' ');
    if (space == std::string::npos) {
        send_tagged(session, tag, "BAD", "FETCH requires arguments");
        return;
    }

    std::string seq_set = args.substr(0, space);
    std::string attrs = args.substr(space + 1);
    if (!attrs.empty() && attrs[0] == '(') {
        size_t close = attrs.find(')');
        if (close != std::string::npos) {
            attrs = attrs.substr(1, close - 1);
        }
    }

    // 读路径 DB 查询走 CPS 链：查完在回调里继续组装 FETCH。
    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [self, tag = std::move(tag), is_uid,
         seq_set = std::move(seq_set), attrs = std::move(attrs)](
            bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok || mails.empty()) {
                send_tagged(self, tag, "OK", "FETCH completed (empty)");
                self->drain_buffered_commands();
                return;
            }

            std::transform(attrs.begin(), attrs.end(), attrs.begin(), ::toupper);

            bool want_uid = is_uid || attrs.find("UID") != std::string::npos;
            bool want_flags = attrs.find("FLAGS") != std::string::npos || attrs.find("ALL") != std::string::npos || attrs.find("FAST") != std::string::npos;
            bool want_internaldate = attrs.find("INTERNALDATE") != std::string::npos || attrs.find("ALL") != std::string::npos;
            bool want_rfc822_size = attrs.find("RFC822.SIZE") != std::string::npos || attrs.find("ALL") != std::string::npos || attrs.find("FAST") != std::string::npos;
            bool want_envelope = attrs.find("ENVELOPE") != std::string::npos || attrs.find("ALL") != std::string::npos;
            bool want_body = attrs.find("BODY[]") != std::string::npos || attrs.find("BODY.PEEK[]") != std::string::npos;
            bool has_header_fields = attrs.find("HEADER.FIELDS") != std::string::npos;
            bool want_body_header = has_header_fields ||
                attrs.find("BODY.PEEK[HEADER]") != std::string::npos ||
                attrs.find("BODY[HEADER]") != std::string::npos;
            std::string header_fields_filter;
            bool header_fields_not = false;
            if (has_header_fields) {
                header_fields_not = attrs.find("HEADER.FIELDS.NOT") != std::string::npos;
                size_t lp = attrs.find('(', attrs.find("HEADER.FIELDS"));
                if (lp != std::string::npos) {
                    size_t rp = attrs.find(')', lp);
                    if (rp != std::string::npos)
                        header_fields_filter = attrs.substr(lp + 1, rp - lp - 1);
                }
            }
            int body_part_num = 0;
            {
                auto bracket = attrs.find('[');
                if (bracket != std::string::npos) {
                    auto close = attrs.find(']', bracket);
                    if (close != std::string::npos) {
                        std::string num_str = attrs.substr(bracket + 1, close - bracket - 1);
                        bool all_digits = !num_str.empty();
                        for (char c : num_str) if (c < '0' || c > '9') { all_digits = false; break; }
                        if (all_digits) body_part_num = std::stoi(num_str);
                    }
                }
            }
            bool want_body_part = (body_part_num > 0);
            if (want_body_part) want_body = true;
            bool want_body_struct = attrs.find("BODYSTRUCTURE") != std::string::npos;

            std::vector<std::pair<uint64_t, uint64_t>> ranges;
            expand_seq_set(seq_set, mails.size(), ranges);
            if (ranges.empty()) {
                send_tagged(self, tag, "OK", "FETCH completed");
                self->drain_buffered_commands();
                return;
            }

            auto state = std::make_shared<FetchContext>();
            state->tag = tag;
            state->mails = std::move(mails);
            state->ranges = std::move(ranges);
            state->want_uid = want_uid;
            state->want_flags = want_flags;
            state->want_internaldate = want_internaldate;
            state->want_rfc822_size = want_rfc822_size;
            state->want_envelope = want_envelope;
            state->want_body = want_body;
            state->want_body_header = want_body_header;
            state->want_body_struct = want_body_struct;
            state->has_header_fields = has_header_fields;
            state->header_fields_not = header_fields_not;
            state->header_fields_filter = std::move(header_fields_filter);
            state->body_part_num = body_part_num;
            auto* srv = self->get_server();
            state->provider = srv->m_shardRouter ? srv->m_shardRouter->get_storage(0) : nullptr;

            // 正文读取走 async：本地内联（行为与同步版一致），远程后端由装饰器
            // 投递 worker —— 逐封 size/正文不再阻塞 io 线程；每封正文只读一次，
            // header/body/BODYSTRUCTURE 兜底共用（旧路径最多读三次）。
            fetch_drive(self, state);
        });
}

// ---------- FETCH 续作链 ----------

template <typename ConnectionType>
struct TraditionalImapsFsm<ConnectionType>::FetchContext {
    std::string tag;
    std::vector<MailboxMailInfo> mails;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    size_t range_idx = 0;
    uint64_t cur_seq = 0;             // 0 = 待推进到下一封
    std::string cur;                  // 当前邮件的响应片段
    std::string response;
    std::shared_ptr<storage::IStorageProvider> provider;
    std::string cur_body_path;        // 供异步回调里的日志
    bool struct_sidecar_hit = false;
    MimePart struct_tree;

    bool want_uid = false, want_flags = false, want_internaldate = false;
    bool want_rfc822_size = false, want_envelope = false;
    bool want_body = false, want_body_header = false, want_body_struct = false;
    bool has_header_fields = false, header_fields_not = false;
    std::string header_fields_filter;
    int body_part_num = 0;

    // 推进游标到下一封待处理的邮件；false = 全部完成
    bool advance() {
        for (;;) {
            if (cur_seq == 0) {
                if (range_idx >= ranges.size()) return false;
                cur_seq = ranges[range_idx].first;
            }
            if (cur_seq > ranges[range_idx].second || cur_seq > mails.size()) {
                ++range_idx;
                cur_seq = 0;
                continue;
            }
            return true;
        }
    }
};

// 无 provider 时的本地兜底（与原 read_mail_body 的 ifstream 分支一致）
static std::string fetch_read_local_body(const std::string& body_path) {
    if (body_path.empty()) return "";
    std::ifstream in(body_path, std::ios::binary);
    if (!in.is_open()) {
        LOG_FILE_IO_ERROR("Failed to open mail body: {}", body_path);
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 用已就绪的正文完成当前邮件剩余 item（纯数据推进，不触碰 session）
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_complete_mail_with_body(
    FetchContext& c, std::string body_content)
{
    const auto& mail_info = c.mails[c.cur_seq - 1];

    if (c.want_body_header) {
        std::string headers = body_content;
        size_t hdr_end = body_content.find("\r\n\r\n");
        if (hdr_end != std::string::npos)
            headers = body_content.substr(0, hdr_end + 2);

        std::string label = "BODY[HEADER]";
        if (c.has_header_fields && !c.header_fields_filter.empty()) {
            std::set<std::string> wanted;
            {
                std::istringstream fs(c.header_fields_filter);
                std::string f;
                while (fs >> f) {
                    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
                    wanted.insert(f);
                }
            }
            std::string filtered;
            std::istringstream hs(headers);
            std::string line;
            while (std::getline(hs, line)) {
                if (line.empty() || line == "\r") break;
                if (line.back() == '\r') line.pop_back();
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string hdr_name = line.substr(0, colon);
                    std::transform(hdr_name.begin(), hdr_name.end(), hdr_name.begin(), ::tolower);
                    bool match = wanted.count(hdr_name) > 0;
                    if (c.header_fields_not) match = !match;
                    if (match) filtered += line + "\r\n";
                }
            }
            if (!filtered.empty() && filtered.size() >= 2)
                filtered.resize(filtered.size() - 2);
            headers = filtered;
            label = c.header_fields_not ? "BODY[HEADER.FIELDS.NOT (" : "BODY[HEADER.FIELDS (";
            label += c.header_fields_filter + ")]";
        }
        c.cur += label + " " + build_fetch_body_response(headers, headers.size()) + " ";
    }
    if (c.want_body) {
        if (c.body_part_num > 0) {
            MimePart mime_tree;
            if (ensure_mime_tree(mail_info.body_path, body_content, mime_tree)) {
                const MimePart* part = nullptr;
                if (mime_tree.is_multipart() && (size_t)c.body_part_num <= mime_tree.subs.size())
                    part = &mime_tree.subs[c.body_part_num - 1];
                else if (!mime_tree.is_multipart() && c.body_part_num == 1)
                    part = &mime_tree;
                if (part)
                    body_content = extract_part_content(body_content, *part);
                else
                    body_content.clear();
            } else {
                body_content.clear();
            }
        }
        std::string body_label = c.body_part_num > 0 ? ("BODY[" + std::to_string(c.body_part_num) + "]") : "BODY[]";
        c.cur += body_label + " " + build_fetch_body_response(body_content, body_content.size()) + " ";
    }
    if (c.want_body_struct) {
        if (c.struct_sidecar_hit) {
            c.cur += "BODYSTRUCTURE " + build_bodystructure_tree(c.struct_tree) + " ";
        } else if (ensure_mime_tree(mail_info.body_path, body_content, c.struct_tree)) {
            c.cur += "BODYSTRUCTURE " + build_bodystructure_tree(c.struct_tree) + " ";
        } else {
            c.cur += "BODYSTRUCTURE " + build_bodystructure(body_content) + " ";
        }
    }

    if (!c.cur.empty() && c.cur.back() == ' ') c.cur.pop_back();
    c.cur += ")\r\n";
    c.response += c.cur;
    c.cur.clear();
    ++c.cur_seq;
}

// size 之后的阶段：envelope（内存）→ 决定是否读正文 → 完成本封。
// 返回 true 表示已发起异步正文读取（回调里 fetch_continue）；false 表示正文已
// 同步就绪并完成本封。
template <typename ConnectionType>
bool TraditionalImapsFsm<ConnectionType>::fetch_after_size(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx,
    std::shared_ptr<std::atomic<bool>> alive)
{
    const auto& mail_info = ctx->mails[ctx->cur_seq - 1];

    if (ctx->want_envelope) {
        std::string date_str;
        {
            struct tm result;
            memset(&result, 0, sizeof(result));
            time_t t = mail_info.send_time;
#ifdef _WIN32
            gmtime_s(&result, &t);
#else
            gmtime_r(&t, &result);
#endif
            static const char* months[] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d-%s-%04d",
                     result.tm_mday, months[result.tm_mon],
                     result.tm_year + 1900);
            date_str = buf;
        }
        ctx->cur += "ENVELOPE " + build_envelope_string(
            date_str, mail_info.subject, mail_info.sender, mail_info.sender,
            "", mail_info.recipient, "", "", "",
            std::to_string(mail_info.mail_id)) + " ";
    }

    // BODYSTRUCTURE 先试 sidecar（本地小文件，同步读即可）
    ctx->struct_sidecar_hit = false;
    if (ctx->want_body_struct) {
        ctx->struct_sidecar_hit = load_mime_tree(mail_info.body_path, ctx->struct_tree);
    }
    const bool need_body = !mail_info.body_path.empty() &&
        (ctx->want_body_header || ctx->want_body ||
         (ctx->want_body_struct && !ctx->struct_sidecar_hit));

    if (need_body && ctx->provider) {
        session->set_paused(true);
        ctx->cur_body_path = mail_info.body_path;
        auto provider = ctx->provider;
        provider->async_read_all(mail_info.body_path,
            [session, ctx, alive](bool ok, std::string data, const storage::IoError& error) mutable {
                if (!ok) {
                    LOG_FILE_IO_ERROR("Failed to read mail body {}: {}",
                                      ctx->cur_body_path, error.message);
                    data.clear();
                }
                fetch_complete_mail_with_body(*ctx, std::move(data));
                fetch_continue(session, ctx, alive);   // inline→外层循环续；deferred→驱动下一封
            });
        return true;
    }
    std::string body = need_body ? fetch_read_local_body(mail_info.body_path) : std::string();
    fetch_complete_mail_with_body(*ctx, std::move(body));
    return false;
}

// 完成本封剩余 item 后的续作：inline 回调（外层 fetch_drive 帧仍在）→ 外层循环继续；
// deferred 回调（外层已返回）→ 由这里驱动下一封。alive 每封一个，size 与 body 两个
// 异步读共享，保证"最后完成本封的回调"恰好驱动/续作一次。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_continue(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx,
    std::shared_ptr<std::atomic<bool>> alive)
{
    if (alive->exchange(false)) return;   // inline：外层 fetch_drive 会 continue
    fetch_drive(session, ctx);            // deferred：驱动下一封
}

// 驱动器：迭代化续作链（不递归）。同步路径在 for 循环里逐封完成；需要异步
// size/正文读取时发起后：若回调 inline（外层帧仍在）→ 循环 continue；若回调
// deferred（外层已返回）→ 由回调里的 fetch_continue 驱动下一封。
// alive 每封一个共享标记，size 与 body 两个异步读共用，保证续作恰好一次。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_drive(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx)
{
    for (;;) {
        if (!ctx->advance()) {
            fetch_finalize(session, ctx);
            return;
        }
        const auto& mail_info = ctx->mails[ctx->cur_seq - 1];

        ctx->cur = "* " + std::to_string(ctx->cur_seq) + " FETCH (";
        if (ctx->want_uid) {
            ctx->cur += "UID " + std::to_string(mail_info.mail_id) + " ";
        }
        if (ctx->want_flags) {
            std::string flags = build_flags_string(
                mail_info.status, mail_info.is_starred,
                mail_info.is_deleted, mail_info.is_important);
            ctx->cur += "FLAGS (" + flags + ") ";
        }
        if (ctx->want_internaldate) {
            ctx->cur += "INTERNALDATE \"" + imap_timestamp(mail_info.send_time) + "\" ";
        }

        // 本封的续作标记：size / body 两个异步读共享
        auto alive = std::make_shared<std::atomic<bool>>(true);

        if (ctx->want_rfc822_size) {
            if (!mail_info.body_path.empty() && ctx->provider) {
                session->set_paused(true);
                ctx->cur_body_path = mail_info.body_path;
                auto provider = ctx->provider;
                provider->async_object_size(mail_info.body_path,
                    [session, ctx, alive](bool ok, std::uint64_t sz, const storage::IoError& error) mutable {
                        if (!ok) {
                            LOG_FILE_IO_ERROR("RFC822.SIZE lookup failed for {}: {}",
                                              ctx->cur_body_path, error.message);
                            sz = 0;
                        }
                        ctx->cur += "RFC822.SIZE " + std::to_string(sz) + " ";
                        // 正文可能再异步读；只有最后完成本封的一步才续作
                        if (fetch_after_size(session, ctx, alive)) return;
                        fetch_continue(session, ctx, alive);
                    });
                if (alive->exchange(false)) return;   // deferred：回调会 fetch_continue 驱动
                continue;                             // inline：本封已完成，下一封
            }
            std::uint64_t sz = 0;
            if (!mail_info.body_path.empty()) {
                std::error_code ec;
                const auto fsz = std::filesystem::file_size(mail_info.body_path, ec);
                sz = ec ? 0 : static_cast<std::uint64_t>(fsz);
            }
            ctx->cur += "RFC822.SIZE " + std::to_string(sz) + " ";
        }
        if (fetch_after_size(session, ctx, alive)) {
            if (alive->exchange(false)) return;   // deferred：正文回调驱动
            continue;                             // inline：本封已完成
        }
        // 同步完成本封 → 循环处理下一封
    }
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::fetch_finalize(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    std::shared_ptr<FetchContext> ctx)
{
    std::string response = std::move(ctx->response);
    response += ctx->tag + " OK FETCH completed\r\n";

    session->do_async_write(response,
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) {
            if (ec) return;
            // 异步链期间流水线被 pause：先排空缓冲，再视情况续读
            s->drain_buffered_commands();
            if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
        });
}

// ---------- STORE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_store(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // Parse: <sequence-set> <data-item> <value>
    // e.g. "1:* +FLAGS (\\Seen \\Flagged)"
    //      "2:4 FLAGS (\\Deleted)"
    auto* ctx_store = static_cast<ImapContext*>(session->get_context());
    auto args = (ctx_store && ctx_store->is_uid_command) ? ctx_store->uid_overridden_args : session->get_last_command_args();
    size_t space1 = args.find(' ');
    if (space1 == std::string::npos) {
        send_tagged(session, tag, "BAD", "STORE requires arguments");
        return;
    }

    std::string seq_set = args.substr(0, space1);
    std::string rest = args.substr(space1 + 1);

    // Handle optional SILENT
    bool silent = false;
    size_t space2 = rest.find(' ');
    if (space2 == std::string::npos) {
        send_tagged(session, tag, "BAD", "STORE requires flags");
        return;
    }

    std::string store_cmd = rest.substr(0, space2);
    std::string flags_part = rest.substr(space2 + 1);

    // Check for SILENT（大小写不敏感）
    std::string cmd_upper = store_cmd;
    std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);
    if (cmd_upper.find("SILENT") != std::string::npos) {
        silent = true;
        // 注意顺序：先匹配带 +/- 的，再匹配裸 FLAGS，否则 -FLAGS 会丢失负号被当成添加
        if (cmd_upper.find("-FLAGS") != std::string::npos) {
            store_cmd = "-FLAGS";
        } else if (cmd_upper.find("+FLAGS") != std::string::npos) {
            store_cmd = "+FLAGS";
        } else if (cmd_upper.find("FLAGS") != std::string::npos) {
            store_cmd = "FLAGS";
        }
    }

    // Parse flags from parentheses
    if (!flags_part.empty() && flags_part[0] == '(') {
        size_t close = flags_part.find(')');
        if (close != std::string::npos) {
            flags_part = flags_part.substr(1, close - 1);
        }
    }

    // RFC 3501 属性名大小写不敏感：客户端可能发 \SEEN / \Seen / \seen
    // （之前只匹配 \Seen，网易大师发 \SEEN 时 flag_seen=false → 已读从不落库）
    std::string flags_upper = flags_part;
    std::transform(flags_upper.begin(), flags_upper.end(), flags_upper.begin(), ::toupper);
    bool flag_seen = flags_upper.find("\\SEEN") != std::string::npos;
    bool flag_flagged = flags_upper.find("\\FLAGGED") != std::string::npos;
    bool flag_deleted = flags_upper.find("\\DELETED") != std::string::npos;
    bool add = store_cmd.find('+') != std::string::npos || (store_cmd.find("FLAGS") != std::string::npos && store_cmd[0] != '-');
    bool remove = store_cmd.find('-') != std::string::npos;

    // 批量 flag 更新（STORE）：读路径 CPS 链查邮件 + 收件人，然后
    // 单条 IN 列表 SQL 批量写（拍平原 N 次循环 update_mail_seen/...）。
    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [this, self, conn, tag = std::move(tag), seq_set = std::move(seq_set),
         flag_seen, flag_flagged, flag_deleted, add, remove, silent,
         mailbox_id, user_id](bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok || mails.empty()) {
                send_tagged(self, tag, "OK", "STORE completed");
                self->drain_buffered_commands();
                return;
            }

            // Parse sequence ranges（支持逗号分隔）
            std::vector<std::pair<uint64_t, uint64_t>> ranges;
            expand_seq_set(seq_set, mails.size(), ranges);
            if (ranges.empty()) {
                send_tagged(self, tag, "OK", "STORE completed");
                self->drain_buffered_commands();
                return;
            }

            // 收件人（seen 写 mail_recipients.status 需要）；查完再继续
            TraditionalImapsFsm<ConnectionType>::get_user_email_async(
                conn, user_id,
                [this, self, conn, tag = std::move(tag), mails = std::move(mails),
                 ranges = std::move(ranges), flag_seen, flag_flagged, flag_deleted,
                 add, remove, silent, mailbox_id, user_id](
                    std::string user_email) mutable {
                    if (!self || self->is_closed()) return;
                    if (user_email.empty()) {
                        auto* c = static_cast<ImapContext*>(self->get_context());
                        if (c) user_email = c->username;
                    }

                    // 收集每个 flag 操作的 mail_id 集合（add/remove 互斥，各至多一次批量）
                    std::vector<uint64_t> seen_ids, flagged_ids, deleted_ids;
                    std::string response;
                    for (const auto& range : ranges) {
                    for (uint64_t seq = range.first; seq <= range.second; ++seq) {
                        size_t idx = seq - 1;
                        const auto& mail_info = mails[idx];

                        if ((add || !remove) && flag_seen) seen_ids.push_back(mail_info.mail_id);
                        if (remove && flag_seen) seen_ids.push_back(mail_info.mail_id);
                        if ((add || !remove) && flag_flagged) flagged_ids.push_back(mail_info.mail_id);
                        if (remove && flag_flagged) flagged_ids.push_back(mail_info.mail_id);
                        if ((add || !remove) && flag_deleted) deleted_ids.push_back(mail_info.mail_id);
                        if (remove && flag_deleted) deleted_ids.push_back(mail_info.mail_id);

                        if (!silent) {
                            int new_status = (add && flag_seen) ? 0 : mail_info.status;
                            if (remove && flag_seen) new_status = 1;

                            std::string flags = build_flags_string(
                                new_status,
                                (add && flag_flagged) ? true : (remove && flag_flagged) ? false : mail_info.is_starred,
                                (add && flag_deleted) ? true : (remove && flag_deleted) ? false : mail_info.is_deleted,
                                mail_info.is_important);
                            response += "* " + std::to_string(seq) + " FETCH (FLAGS (" + flags + "))\r\n";
                        }
                    }
                    } // for each range

                    // 批量写：seen / flagged / deleted 各一条 IN SQL，串行执行。
                    // 完成后失效统计缓存并回包。
                    int seen_status = (add && flag_seen) ? 0 : 1;
                    std::vector<std::function<void(std::function<void(bool)>)>> steps;
                    if (!seen_ids.empty()) {
                        auto ids = std::move(seen_ids);
                        auto recipient = user_email;
                        int status = seen_status;
                        steps.push_back([conn, ids = std::move(ids), recipient = std::move(recipient), status](
                                            std::function<void(bool)> done) {
                            TraditionalImapsFsm<ConnectionType>::batch_mark_seen_async(
                                conn, ids, recipient, status, std::move(done));
                        });
                    }
                    if (!flagged_ids.empty()) {
                        auto ids = std::move(flagged_ids);
                        int v = add ? 1 : 0;
                        steps.push_back([conn, ids = std::move(ids), mailbox_id, user_id, v](
                                            std::function<void(bool)> done) {
                            TraditionalImapsFsm<ConnectionType>::batch_mark_flagged_async(
                                conn, ids, user_id, mailbox_id, v, std::move(done));
                        });
                    }
                    if (!deleted_ids.empty()) {
                        auto ids = std::move(deleted_ids);
                        int v = add ? 1 : 0;
                        steps.push_back([conn, ids = std::move(ids), mailbox_id, user_id, v](
                                            std::function<void(bool)> done) {
                            TraditionalImapsFsm<ConnectionType>::batch_mark_deleted_async(
                                conn, ids, user_id, mailbox_id, v, std::move(done));
                        });
                    }

                    auto run_next = std::make_shared<std::function<void(size_t)>>();
                    *run_next = [this, self, tag = std::move(tag), response = std::move(response),
                                 steps = std::move(steps), mailbox_id, user_id,
                                 run_next](size_t i) mutable {
                        if (i >= steps.size()) {
                            // 全部批量完成：失效统计缓存，回包
                            if (this->m_mailboxStatsCache) {
                                this->m_mailboxStatsCache->invalidate(mbox_cache_key(user_id, mailbox_id));
                            }
                            response += tag + " OK STORE completed\r\n";
                            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                            self->drain_buffered_commands();
                            return;
                        }
                        steps[i]([run_next, i](bool) { (*run_next)(i + 1); });
                    };
                    (*run_next)(0);
                });
        });
}

// ---------- EXPUNGE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_expunge(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // 读邮件列表 → 算 \Deleted 序号 → 批量物理删除 → 回 EXPUNGE 通知
    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [self, conn, tag = std::move(tag), mailbox_id, user_id](
            bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok) {
                send_tagged(self, tag, "NO", "Server error");
                self->drain_buffered_commands();
                return;
            }

            // Find which sequences are deleted
            std::vector<uint64_t> expunged_seqs;
            for (size_t i = 0; i < mails.size(); ++i) {
                if (mails[i].is_deleted) {
                    expunged_seqs.push_back(i + 1);
                }
            }

            // Actually delete from database
            TraditionalImapsFsm<ConnectionType>::expunge_mailbox_async(
                conn, mailbox_id, user_id,
                [self, tag = std::move(tag), expunged_seqs = std::move(expunged_seqs)](bool) {
                    if (!self || self->is_closed()) return;
                    std::string response;
                    for (auto seq : expunged_seqs) {
                        response += "* " + std::to_string(seq) + " EXPUNGE\r\n";
                    }
                    response += tag + " OK EXPUNGE completed\r\n";

                    self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                    self->drain_buffered_commands();
                });
        });
}

// ---------- CLOSE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_close(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // 清除选中状态 + 回 CLOSE OK（回调里 session/ctx 由捕获保持存活）
    auto finish = [session, ctx, tag](std::function<void()> cont) {
        if (ctx) {
            ctx->mailbox_selected = false;
            ctx->selected_mailbox_name.clear();
            ctx->selected_mailbox_id = 0;
            ctx->read_only = false;
        }
        session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
        send_tagged(session, tag, "OK", "CLOSE completed");
        cont();
    };

    // If SELECTED, expunge deleted mails first（异步 CPS，完成后回包）
    if (static_cast<ImapState>(session->get_current_state()) == ImapState::SELECTED && ctx) {
        auto conn = std::make_shared<ScopedConnection>(
            this->acquire_connection(ctx->shard_index));
        if (conn->is_valid()) {
            session->set_paused(true);
            auto self = session->shared_from_this();
            uint64_t mailbox_id = ctx->selected_mailbox_id;
            uint64_t user_id = ctx->user_id;
            TraditionalImapsFsm<ConnectionType>::expunge_mailbox_async(
                conn, mailbox_id, user_id,
                [self, finish](bool) {
                    if (!self || self->is_closed()) return;
                    finish([self]() { self->drain_buffered_commands(); });
                });
            return;
        }
    }
    finish([]() {});
}

// ---------- NOOP ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_noop(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(session, tag, "OK", "NOOP completed");
}

// ---------- CHECK ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_check(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(session, tag, "OK", "CHECK completed");
}

// ---------- STARTTLS ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_starttls(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // STARTTLS only available on TCP connections (not already SSL)
    // Handoff logic: send OK, extract TCP socket, call server handoff
    session->do_async_write(tag + " OK Begin TLS negotiation now\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> self,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending STARTTLS response: {}", ec.message());
                return;
            }
            auto server = static_cast<ImapsServer*>(self->get_server());
            auto trace = self->take_trace_buffer();   // 交接给 TLS 会话，延续对话记录
            auto tcp_sock = self->release_connection()->release_socket();
            server->handoff_starttls_socket(std::move(tcp_sock), std::move(trace));
        }
    );
}

// ---------- CREATE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_create(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    if (mailbox_name.empty()) {
        send_tagged(session, tag, "BAD", "CREATE requires mailbox name");
        return;
    }

    // 解码 IMAP-UTF-7 → UTF-8（客户端发来的名称可能是编码后的）
    mailbox_name = this->decode_mailbox_name(mailbox_name);

    auto conn = this->acquire_connection(ctx->shard_index);
    if (!conn.is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }

    session->set_paused(true);
    conn->async_execute(db::sql::build_imap_create_mailbox(),
                        {std::to_string(ctx->user_id), mailbox_name},
                        [session, tag](bool ok) {
                            if (ok) send_tagged(session, tag, "OK", "CREATE completed");
                            else send_tagged(session, tag, "NO", "CREATE failed (maybe already exists)");
                            session->drain_buffered_commands();
                        });
}

// ---------- DELETE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_delete(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    std::string mailbox_name = session->get_last_command_args();
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 系统邮箱检查 → 删消息 → 删邮箱
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [self, conn, tag = std::move(tag)](uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found");
                self->drain_buffered_commands();
                return;
            }

            // Check if it's a system mailbox
            (*conn)->async_query(db::sql::build_imap_check_mailbox_is_system(),
                                 {std::to_string(mailbox_id)},
                                 [self, conn, tag = std::move(tag), mailbox_id]
                                 (std::shared_ptr<IDBResult> result) mutable {
                                     if (result && result->get_row_count() > 0 &&
                                         result->get_value(0, "is_system") == "1") {
                                         send_tagged(self, tag, "NO", "Cannot delete system mailbox");
                                         self->drain_buffered_commands();
                                         return;
                                     }
                                     // 删除消息 + 删除邮箱
                                     (*conn)->async_execute(db::sql::build_imap_delete_mailbox_messages(),
                                                            {std::to_string(mailbox_id)},
                                                            [self, conn, tag = std::move(tag), mailbox_id]
                                                            (bool ok1) mutable {
                                                                (*conn)->async_execute(
                                                                    db::sql::build_imap_delete_mailbox(),
                                                                    {std::to_string(mailbox_id)},
                                                                    [self, tag = std::move(tag), ok1](bool ok2) {
                                                                        if (ok1 && ok2)
                                                                            send_tagged(self, tag, "OK", "DELETE completed");
                                                                        else
                                                                            send_tagged(self, tag, "NO", "DELETE failed");
                                                                        self->drain_buffered_commands();
                                                                    });
                                                              });
                                 });
        });
}

// ---------- RENAME ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_rename(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    // Parse old_name new_name
    std::string old_name, new_name, args = session->get_last_command_args();
    size_t space = args.find(' ');
    if (space != std::string::npos) {
        old_name = args.substr(0, space);
        new_name = args.substr(space + 1);
    }

    // Trim quotes
    auto trim_quotes = [](std::string& s) {
        if (!s.empty() && s[0] == '"') {
            size_t end = s.find('"', 1);
            if (end != std::string::npos) s = s.substr(1, end - 1);
        }
    };
    trim_quotes(old_name);
    trim_quotes(new_name);

    if (old_name.empty() || new_name.empty()) {
        send_tagged(session, tag, "BAD", "RENAME requires old and new names");
        return;
    }

    // 新名称也要解码（客户端发来的可能是 IMAP-UTF-7 编码）
    new_name = this->decode_mailbox_name(new_name);

    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    // CPS 链：找邮箱 id → 改名
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, old_name,
        [self, conn, tag = std::move(tag), new_name = std::move(new_name)](
            uint64_t mailbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (mailbox_id == 0) {
                send_tagged(self, tag, "NO", "Mailbox not found");
                self->drain_buffered_commands();
                return;
            }
            (*conn)->async_execute(db::sql::build_imap_rename_mailbox(),
                                   {new_name, std::to_string(mailbox_id)},
                                   [self, tag = std::move(tag)](bool ok) {
                                       if (ok) send_tagged(self, tag, "OK", "RENAME completed");
                                       else send_tagged(self, tag, "NO", "RENAME failed");
                                       self->drain_buffered_commands();
                                   });
        });
}

// ---------- SUBSCRIBE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_subscribe(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    LOG_IMAP_INFO("SUBSCRIBE {} — subscription persistence not yet implemented");
    send_tagged(session, tag, "OK", "SUBSCRIBE completed");
}

// ---------- UNSUBSCRIBE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_unsubscribe(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    LOG_IMAP_INFO("UNSUBSCRIBE {} — subscription persistence not yet implemented");
    send_tagged(session, tag, "OK", "UNSUBSCRIBE completed");
}

// ---------- APPEND ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_append(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    if (ctx->pending_append_preamble.empty()) {
        send_tagged(session, tag, "BAD", "APPEND missing preamble");
        return;
    }

    // 解析 preamble: mailbox (flags) "InternalDate"
    std::string preamble = ctx->pending_append_preamble;
    std::string mailbox_name;
    std::string flags_str;

    // 提取邮箱名（可能带引号）
    if (preamble[0] == '"') {
        size_t end = preamble.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = preamble.substr(1, end - 1);
            std::string rest = preamble.substr(end + 1);
            // trim leading spaces
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '(') {
                size_t paren = rest.find(')');
                if (paren != std::string::npos) {
                    flags_str = rest.substr(0, paren + 1);
                }
            }
        }
    } else {
        size_t sp = preamble.find(' ');
        if (sp != std::string::npos) {
            mailbox_name = preamble.substr(0, sp);
            std::string rest = preamble.substr(sp + 1);
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '(') {
                size_t paren = rest.find(')');
                if (paren != std::string::npos) {
                    flags_str = rest.substr(0, paren + 1);
                }
            }
        } else {
            mailbox_name = preamble;
        }
    }

    // 解析 flags
    int init_status = 0; // 0=read
    if (flags_str.find("\\Seen") != std::string::npos || flags_str.find("\\seen") != std::string::npos) {
        init_status = 0;
    } else if (flags_str.find("\\Unseen") != std::string::npos || flags_str.find("\\Draft") != std::string::npos) {
        init_status = 1; // unread / draft
    }
    if (flags_str.find("\\Deleted") != std::string::npos) {
        init_status = 5; // deleted
    }

    // 解码邮箱名（IMAP-UTF-7 → UTF-8）
    mailbox_name = this->decode_mailbox_name(mailbox_name);

    // 正文内容即 literal 数据（args）
    std::string body_content = session->get_last_command_args();
    std::string subject = "(APPEND)";

    // 尝试提取 Subject（正文第一行或全文首 200 字符）
    if (!body_content.empty()) {
        std::istringstream ss(body_content);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("Subject:") == 0 || line.find("subject:") == 0) {
                subject = line.substr(8);
                // trim
                subject.erase(0, subject.find_first_not_of(" \t"));
                break;
            }
        }
    }

    // CPS 链：找目标邮箱 → worker 写 storage + DB（阻塞 I/O 不进 io 线程）→
    // create → user_email → link → 回 APPENDUID
    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, mailbox_name,
        [this, self, conn, tag = std::move(tag), subject = std::move(subject),
         body_content = std::move(body_content), init_status,
         mailbox_name = mailbox_name, user_id](
            uint64_t target_mbox_id) mutable {
            if (!self || self->is_closed()) return;
            if (target_mbox_id == 0) {
                send_tagged(self, tag, "NO", "APPEND failed: mailbox not found");
                self->drain_buffered_commands();
                return;
            }
            auto storage = this->get_storage(0);
            auto worker = this->m_workerThreadPool;
            if (!worker) {
                send_tagged(self, tag, "NO", "Server not ready");
                self->drain_buffered_commands();
                return;
            }
            // storage append 是阻塞 I/O，整体丢 worker
            worker->post([self, conn, tag = std::move(tag), storage,
                          subject = std::move(subject), body_content = std::move(body_content),
                          init_status, mailbox_name = std::move(mailbox_name),
                          user_id, target_mbox_id]() mutable {
                TraditionalImapsFsm<ConnectionType>::create_mail_async(
                    storage, conn, subject, body_content,
                    [self, conn, tag = std::move(tag), user_id, target_mbox_id,
                     init_status, mailbox_name = std::move(mailbox_name)](
                        uint64_t mail_id, std::string, std::string error) mutable {
                        if (!self || self->is_closed()) return;
                        if (mail_id == 0) {
                            send_tagged(self, tag, "NO", "APPEND failed: " + error);
                            self->drain_buffered_commands();
                            return;
                        }
                        TraditionalImapsFsm<ConnectionType>::get_user_email_async(
                            conn, user_id,
                            [self, conn, tag = std::move(tag), mail_id, user_id,
                             target_mbox_id, init_status, mailbox_name = std::move(mailbox_name)](
                                std::string user_email) mutable {
                                if (!self || self->is_closed()) return;
                                if (user_email.empty()) {
                                    auto* c = static_cast<ImapContext*>(self->get_context());
                                    if (c) user_email = c->username;
                                }
                                TraditionalImapsFsm<ConnectionType>::link_mail_to_mailbox_async(
                                    conn, mail_id, user_id, target_mbox_id,
                                    user_email, user_email, init_status,
                                    [self, tag = std::move(tag), mail_id, target_mbox_id,
                                     mailbox_name = std::move(mailbox_name), user_email](bool) {
                                        if (!self || self->is_closed()) return;
                                        // 返回 APPENDUID
                                        uint64_t uidvalidity = target_mbox_id;
                                        std::string response = tag + " OK [APPENDUID "
                                            + std::to_string(uidvalidity) + " "
                                            + std::to_string(mail_id) + "] APPEND completed\r\n";
                                        self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                                        self->drain_buffered_commands();
                                        LOG_IMAP_INFO("APPEND: mail_id={}, mailbox={}, user={}",
                                                      mail_id, mailbox_name, user_email);
                                    });
                            });
                    });
            });
        });
}

// ---------- SEARCH ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_search(
    std::shared_ptr<SessionBase<ConnectionType>> session, bool is_uid)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // 解析搜索关键词（简单实现常用关键词）——不依赖 DB，先算
    std::string upper_args = session->get_last_command_args();
    std::transform(upper_args.begin(), upper_args.end(), upper_args.begin(), ::toupper);

    bool search_unseen = (upper_args.find("UNSEEN") != std::string::npos
                         || upper_args.find("NEW") != std::string::npos);
    bool search_seen = (upper_args.find("SEEN") != std::string::npos
                       && upper_args.find("UNSEEN") == std::string::npos
                       && upper_args.find("UNSEEN") == std::string::npos);
    bool search_deleted = (upper_args.find("DELETED") != std::string::npos
                          && upper_args.find("UNDELETED") == std::string::npos);

    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server database unavailable");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t mailbox_id = ctx->selected_mailbox_id;
    uint64_t user_id = ctx->user_id;

    TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
        conn, mailbox_id, user_id,
        [self, tag = std::move(tag), is_uid,
         search_unseen, search_seen, search_deleted,
         upper_args = std::move(upper_args)](
            bool ok, std::vector<MailboxMailInfo> mails) mutable {
            if (!self || self->is_closed()) return;
            if (!ok) {
                send_tagged(self, tag, "NO", "Server error");
                self->drain_buffered_commands();
                return;
            }

            // UID 范围条件（如 "UID SEARCH UID 2083...:*"）：只返回 UID 落在该区间的邮件。
            // 之前完全忽略此条件，导致 "UID SEARCH UID <last>:*" 总是返回整箱，
            // 客户端"最后同步 UID"永远无法推进 → 每次轮询都全量重同步。
            bool has_uid_range = false;
            uint64_t uid_lo = 0, uid_hi = UINT64_MAX;
            {
                // 遍历所有 "UID " 出现，取后面跟数字或 * 的 token 作为 UID 集。
                // 兼容 "UID SEARCH UID X:*"（第一个 "UID " 后是 SEARCH，跳过）和 "UID X:*"。
                size_t search_pos = 0;
                while (search_pos < upper_args.size()) {
                    size_t p = upper_args.find("UID ", search_pos);
                    if (p == std::string::npos) break;
                    size_t q = p + 4;
                    while (q < upper_args.size() && upper_args[q] == ' ') q++;
                    size_t e = q;
                    while (e < upper_args.size() &&
                           (std::isalnum(static_cast<unsigned char>(upper_args[e])) ||
                            upper_args[e] == ':' || upper_args[e] == '*'))
                        e++;
                    std::string tok = upper_args.substr(q, e - q);
                    if (!tok.empty() && tok != "SEARCH" &&
                        (tok[0] == '*' || std::isdigit(static_cast<unsigned char>(tok[0])))) {
                        size_t colon = tok.find(':');
                        if (colon != std::string::npos) {
                            std::string a = tok.substr(0, colon), b = tok.substr(colon + 1);
                            uid_lo = (a == "*") ? 0 : safe_stoull(a);
                            uid_hi = (b == "*") ? UINT64_MAX : safe_stoull(b);
                        } else if (tok != "*") {
                            uid_lo = uid_hi = safe_stoull(tok);
                        }
                        has_uid_range = true;
                        break;
                    }
                    search_pos = p + 4;
                }
            }

            std::string response = "* SEARCH";
            for (size_t i = 0; i < mails.size(); ++i) {
                const auto& m = mails[i];
                bool match = true;
                if (search_unseen) match = (m.status == 1);
                else if (search_seen) match = (m.status == 0);
                if (search_deleted) match = m.is_deleted;
                if (has_uid_range)
                    match = match && (m.mail_id >= uid_lo && m.mail_id <= uid_hi);

                if (match) {
                    // UID SEARCH 返回 mail_id，普通 SEARCH 返回 seq number
                    response += " " + std::to_string(is_uid ? m.mail_id : (i + 1));
                }
            }
            response += "\r\n";
            response += tag + " OK SEARCH completed\r\n";

            self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
            self->drain_buffered_commands();
        });
}

// ---------- UID ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_uid(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    std::string args = session->get_last_command_args();
    size_t space = args.find(' ');
    if (space == std::string::npos) {
        send_tagged(session, tag, "BAD", "UID requires subcommand");
        return;
    }

    std::string subcmd = args.substr(0, space);
    std::string subargs = args.substr(space + 1);
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::toupper);

    LOG_IMAP_INFO("UID subcmd={} args={}", subcmd, subargs);

    // UID FETCH/STORE/COPY: 把 UID 序列号映射为 mails 数组下标+1（CPS 查邮件列表）
    if (subcmd == "FETCH" || subcmd == "STORE" || subcmd == "COPY") {
        auto conn = std::make_shared<ScopedConnection>(
            this->acquire_connection(ctx->shard_index));
        if (!conn->is_valid()) {
            send_tagged(session, tag, "NO", "Server database unavailable");
            return;
        }
        session->set_paused(true);
        auto self = session->shared_from_this();
        uint64_t mailbox_id = ctx->selected_mailbox_id;
        uint64_t user_id = ctx->user_id;
        std::string scmd = subcmd;

        TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
            conn, mailbox_id, user_id,
            [this, self, tag = std::move(tag), scmd = std::move(scmd),
             subargs = std::move(subargs)](bool ok,
                 std::vector<MailboxMailInfo> mails) mutable {
                if (!self || self->is_closed()) return;
                if (!ok || mails.empty()) {
                    send_tagged(self, tag, "OK", scmd + " completed (empty)");
                    self->drain_buffered_commands();
                    return;
                }
                // 构建 UID → seq 映射
                std::unordered_map<uint64_t, uint64_t> uid_to_seq;
                for (size_t i = 0; i < mails.size(); ++i)
                    uid_to_seq[mails[i].mail_id] = i + 1;

                // 找到 args 中的空格（分割 序列号 和 属性）
                size_t sp = subargs.find(' ');
                std::string uid_set = (sp != std::string::npos) ? subargs.substr(0, sp) : subargs;
                std::string rest    = (sp != std::string::npos) ? subargs.substr(sp) : "";

                // UID set → seq set
                std::string seq_set;
                if (uid_set.find(',') != std::string::npos) {
                    // 逗号分隔的 UID 列表，逐个映射为 seq（保持原顺序）
                    std::istringstream us(uid_set);
                    std::string tok;
                    while (std::getline(us, tok, ',')) {
                        if (tok.empty()) continue;
                        auto it = uid_to_seq.find(safe_stoull(tok));
                        uint64_t s = (it != uid_to_seq.end()) ? it->second : 0;
                        if (s > 0) {
                            if (!seq_set.empty()) seq_set += ",";
                            seq_set += std::to_string(s);
                        }
                    }
                    if (seq_set.empty()) seq_set = "0";
                } else if (uid_set.find(':') != std::string::npos) {
                    // UID 范围：>= start 且 <= end（* 表示无穷）。
                    // 之前要求起止 UID 精确匹配某封邮件，客户端发 "UID <last>:*" 且 <last>
                    // 不是确切 UID 时映射成 0:* → 全量拉取，导致客户端永远重拉。
                    size_t c = uid_set.find(':');
                    std::string u1 = uid_set.substr(0, c), u2 = uid_set.substr(c + 1);
                    uint64_t start_uid = (u1 == "*") ? 0 : safe_stoull(u1);
                    uint64_t end_uid = (u2 == "*") ? UINT64_MAX : safe_stoull(u2);
                    std::string collected;
                    for (size_t i = 0; i < mails.size(); ++i) {
                        if (mails[i].mail_id >= start_uid && mails[i].mail_id <= end_uid) {
                            if (!collected.empty()) collected += ",";
                            collected += std::to_string(i + 1);
                        }
                    }
                    seq_set = collected.empty() ? "0" : collected;
                } else if (uid_set == "*") {
                    seq_set = "*";
                } else {
                    auto it = uid_to_seq.find(safe_stoull(uid_set));
                    seq_set = std::to_string(it != uid_to_seq.end() ? it->second : 0);
                }
                std::string mapped_args = seq_set + rest;

                auto* c = static_cast<ImapContext*>(self->get_context());
                c->is_uid_command = true;
                c->uid_overridden_args = mapped_args;

                if (scmd == "FETCH") {
                    handle_fetch(self, true);
                } else if (scmd == "STORE") {
                    handle_store(self);
                } else if (scmd == "COPY") {
                    handle_copy(self);
                } else {
                    send_tagged(self, tag, "BAD", "Unknown UID subcommand");
                    self->drain_buffered_commands();
                }

                c->is_uid_command = false;
                c->uid_overridden_args.clear();
                LOG_IMAP_INFO("UID subcmd done: {}", scmd);
            });
        return;
    }

    ctx->is_uid_command = true;
    ctx->uid_overridden_args = subargs;

    if (subcmd == "SEARCH") {
        handle_search(session, true);
    } else {
        send_tagged(session, tag, "BAD", "Unknown UID subcommand");
    }

    ctx->is_uid_command = false;
    ctx->uid_overridden_args.clear();
    LOG_IMAP_INFO("UID subcmd done: {}", subcmd);
}

// ---------- COPY / MOVE (共享实现) ----------
// 把当前选中的邮箱中的一组邮件复制/移动到目标邮箱
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_copy_move(
    std::shared_ptr<SessionBase<ConnectionType>> session,
    bool is_move)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(session, tag, "BAD", "No mailbox selected");
        return;
    }

    // 解析 "seq_set mailbox"
    auto args = (ctx && ctx->is_uid_command) ? ctx->uid_overridden_args : session->get_last_command_args();
    size_t sp = args.rfind(' ');
    if (sp == std::string::npos) {
        send_tagged(session, tag, "BAD", "COPY/MOVE requires sequence set and mailbox");
        return;
    }

    std::string seq_set = args.substr(0, sp);
    std::string target_name = args.substr(sp + 1);
    // trim quotes
    if (!target_name.empty() && target_name[0] == '"') {
        size_t end = target_name.find('"', 1);
        if (end != std::string::npos) target_name = target_name.substr(1, end - 1);
    }
    target_name = this->decode_mailbox_name(target_name);

    auto conn = std::make_shared<ScopedConnection>(
        this->acquire_connection(ctx->shard_index));
    if (!conn->is_valid()) {
        send_tagged(session, tag, "NO", "Server error");
        return;
    }
    session->set_paused(true);
    auto self = session->shared_from_this();
    uint64_t user_id = ctx->user_id;
    uint64_t source_mailbox_id = ctx->selected_mailbox_id;

    // CPS 链：找目标邮箱 → 源邮箱邮件列表 → 批量 INSERT IGNORE（单条 VALUES 多行）
    // → MOVE 再批量标记源邮箱 is_deleted=1
    TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
        conn, user_id, target_name,
        [self, conn, tag = std::move(tag), seq_set = std::move(seq_set),
         is_move, user_id, source_mailbox_id](uint64_t target_id) mutable {
            if (!self || self->is_closed()) return;
            if (target_id == 0) {
                send_tagged(self, tag, "NO", "COPY/MOVE failed: mailbox not found");
                self->drain_buffered_commands();
                return;
            }

            TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
                conn, source_mailbox_id, user_id,
                [self, conn, tag = std::move(tag), seq_set = std::move(seq_set),
                 is_move, user_id, source_mailbox_id, target_id](
                    bool ok, std::vector<MailboxMailInfo> mails) mutable {
                    if (!self || self->is_closed()) return;
                    if (!ok) {
                        send_tagged(self, tag, "NO", "Server error");
                        self->drain_buffered_commands();
                        return;
                    }

                    // 建立序号→mail_id 映射
                    std::vector<uint64_t> mail_ids;
                    if (seq_set == "*") {
                        for (const auto& m : mails) mail_ids.push_back(m.mail_id);
                    } else if (seq_set.find(':') != std::string::npos) {
                        size_t colon = seq_set.find(':');
                        uint64_t start = safe_stoull(seq_set.substr(0, colon));
                        uint64_t end_val = safe_stoull(seq_set.substr(colon + 1));
                        if (end_val > mails.size()) end_val = mails.size();
                        for (uint64_t s = start; s <= end_val && s <= mails.size(); ++s) {
                            mail_ids.push_back(mails[s - 1].mail_id);
                        }
                    } else {
                        uint64_t n = safe_stoull(seq_set);
                        if (n >= 1 && n <= mails.size())
                            mail_ids.push_back(mails[n - 1].mail_id);
                    }

                    if (mail_ids.empty()) {
                        send_tagged(self, tag, "OK", "COPY/MOVE completed (no messages)");
                        self->drain_buffered_commands();
                        return;
                    }

                    // 批量插入：INSERT IGNORE 单条 VALUES 多行 + ROW_COUNT 计实际插入
                    TraditionalImapsFsm<ConnectionType>::batch_insert_mailbox_async(
                        conn, mail_ids, target_id, user_id,
                        [self, conn, tag = std::move(tag), is_move,
                         user_id, source_mailbox_id, mail_ids = std::move(mail_ids)](
                            size_t copied) mutable {
                            if (!self || self->is_closed()) return;
                            auto reply = [self, tag = std::move(tag), is_move, copied]() mutable {
                                std::string cmd_name = is_move ? "MOVE" : "COPY";
                                std::string response = tag + " OK " + cmd_name
                                    + " completed (" + std::to_string(copied) + " messages)\r\n";
                                self->do_async_write(response, [](auto s, auto& ec) { if (!ec) s->do_async_read(); });
                                self->drain_buffered_commands();
                            };
                            if (is_move) {
                                // MOVE: 源邮箱标记 is_deleted=1（expunge 时物理删）
                                TraditionalImapsFsm<ConnectionType>::batch_mark_move_deleted_async(
                                    conn, mail_ids, user_id, source_mailbox_id,
                                    [reply = std::move(reply)](bool) mutable { reply(); });
                            } else {
                                reply();
                            }
                        });
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_copy(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    handle_copy_move(session, false);
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_move(
    std::shared_ptr<SessionBase<ConnectionType>> session) {
    handle_copy_move(session, true);
}

// ---------- IDLE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_idle(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx) {
        send_tagged(session, tag, "BAD", "Not authenticated");
        return;
    }

    ctx->idle_mode = true;

    // RFC 2177: send continuation
    session->do_async_write("+ idling\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending IDLE continuation: {}", ec.message());
                return;
            }
            // Continue reading — DONE command will be sent by client
            // NOTE: IDLE push notifications require a background polling
            // mechanism that is beyond the current scope. The IMAP server
            // accepts IDLE/DONE correctly; new-mail notifications (untagged
            // EXISTS) will be sent when the client exits IDLE and issues
            // a NOOP or re-selects the mailbox.
            s->do_async_read();
        }
    );
}

// ---------- DONE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_done(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (!ctx) return;

    ctx->idle_mode = false;

    std::string tag = ctx->current_tag;
    send_tagged(session, tag, "OK", "IDLE terminated");
}

// ---------- ERROR ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_error(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    auto err = session->get_last_command_args();
    send_tagged(session, tag, "BAD", err.empty() ? "Protocol error" : err);
}

// ---------- TIMEOUT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_timeout(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->do_async_write("* BYE TIMEOUT\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending TIMEOUT BYE: {}", ec.message());
                return;
            }
            s->close();
        }
    );
}

// ========== 辅助函数 ==========

// ====================================================================
// ImapsFsm DB helper —— CPS：async_query/async_execute 链
// ====================================================================
// 全部 static：可在 worker/异步回调里调用。底层 MySQL async_* 目前是默认
// 同步包装（回调同步触发），但调用方结构已异步就绪；将来接真异步 DB 时
// 这些调用方无需改动。conn 由调用方持有的 shared ScopedConnection 保活。

// 用户认证（仿 POP3 auth_user_async / SMTP auth 链）。调用方须在 worker
// 线程发起（bcrypt 几十~几百 ms 纯 CPU，不许在 io 线程算）。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::auth_user_async(
    const std::shared_ptr<router::IShardRouter>& shard_router,
    const std::shared_ptr<AuthCache>& auth_cache,
    const std::string& mail_address,
    const std::string& password,
    std::function<void(bool, uint64_t, int)> cb)
{
    if (!cb) return;
    LOG_AUTH_INFO("IMAP AUTH attempt: mail_address=[{}]", mail_address);

    int shard = 0;
    if (shard_router) {
        int r = shard_router->route(mail_address);
        if (r >= 0) shard = r;
    }

    // 快路径：缓存命中（含 status!=1 的负缓存）同步回调
    AuthCacheEntry ce;
    if (auth_cache && auth_cache->lookup(mail_address, ce)) {
        if (ce.status != 1) { cb(false, 0, shard); return; }
        bool ok = (ce.password_hash.size() >= 2 && ce.password_hash[0] == '$' && ce.password_hash[1] == '2')
                    ? bcrypt_verify(password, ce.password_hash)
                    : (ce.password_hash == password);
        cb(ok, ce.user_id, ce.shard);
        return;
    }

    if (!shard_router) { cb(false, 0, shard); return; }
    auto db_pool = shard_router->get_db_pool(static_cast<size_t>(shard));
    if (!db_pool) {
        LOG_AUTH_ERROR("No database pool for shard {}", shard);
        cb(false, 0, shard);
        return;
    }
    // conn 用 shared 保活：真异步接入后回调在 DB 线程触发，链中捕获不悬垂
    auto conn = std::make_shared<ScopedConnection>(db_pool->acquire_connection());
    if (!conn->is_valid()) {
        LOG_AUTH_ERROR("Failed to get database connection for shard {}", shard);
        cb(false, 0, shard);
        return;
    }

    (*conn)->async_query(db::sql::build_auth_user_query(), {mail_address},
        [auth_cache, mail_address, password, shard, conn,
         cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (!result || result->get_row_count() == 0) {
                LOG_AUTH_WARN("User not found: {}", mail_address);
                cb(false, 0, shard);
                return;
            }
            int status = static_cast<int>(safe_stoull(result->get_value(0, "status")));
            if (status != 1) {
                LOG_AUTH_WARN("User account disabled: {}", mail_address);
                cb(false, 0, shard);
                return;
            }
            std::string stored = result->get_value(0, "password");
            uint64_t user_id = safe_stoull(result->get_value(0, "id"));
            if (auth_cache) {
                auth_cache->store(mail_address, {stored, status, user_id, shard});
            }
            bool ok = false;
            if (stored.size() >= 2 && stored[0] == '$' && stored[1] == '2') {
                ok = bcrypt_verify(password, stored);
            } else {
                ok = (stored == password);
                if (ok) LOG_AUTH_WARN("User {} still using plaintext password", mail_address);
            }
            if (ok) {
                (*conn)->async_execute(db::sql::build_update_last_login(), {mail_address},
                    [cb = std::move(cb), user_id, shard](bool) { cb(true, user_id, shard); });
            } else {
                cb(false, 0, shard);
            }
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailboxes_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    std::function<void(bool, std::vector<std::tuple<uint64_t, std::string, int>>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, {}); return; }
    (*conn)->async_query(db::sql::build_imap_list_mailboxes(), {std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            std::vector<std::tuple<uint64_t, std::string, int>> mailboxes;
            if (!result) { cb(false, std::move(mailboxes)); return; }
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                uint64_t id = safe_stoull(result->get_value(i, "id"));
                std::string name = result->get_value(i, "name");
                int box_type = static_cast<int>(safe_stoull(result->get_value(i, "box_type")));
                mailboxes.emplace_back(id, name, box_type);
            }
            cb(true, std::move(mailboxes));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::find_mailbox_id_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    const std::string& mailbox_name, std::function<void(uint64_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    std::string name_utf8 = mailbox_name;
    if (mailbox_name.find('&') != std::string::npos) {
        std::string decoded = decode_imap_utf7(mailbox_name);
        if (!decoded.empty()) name_utf8 = decoded;
    }
    (*conn)->async_query(db::sql::build_imap_get_mailbox_by_name(),
        {std::to_string(user_id), name_utf8},
        [conn, user_id, name_utf8, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0) {
                cb(safe_stoull(result->get_value(0, "id")));
                return;
            }
            // INBOX 兜底（box_type=1，中文名邮箱可能直接查不到）
            std::string upper = name_utf8;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            if (upper != "INBOX") { cb(0); return; }
            (*conn)->async_query(db::sql::build_imap_get_inbox_id(),
                {std::to_string(user_id)},
                [cb = std::move(cb)](std::shared_ptr<IDBResult> r2) mutable {
                    if (r2 && r2->get_row_count() > 0) cb(safe_stoull(r2->get_value(0, "id")));
                    else cb(0);
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_mails_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(bool, std::vector<MailboxMailInfo>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, {}); return; }
    (*conn)->async_query(db::sql::build_imap_get_mailbox_mails(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            std::vector<MailboxMailInfo> mails;
            if (!result) { cb(false, std::move(mails)); return; }
            for (size_t i = 0; i < result->get_row_count(); ++i) {
                MailboxMailInfo info;
                info.mail_id = safe_stoull(result->get_value(i, "id"));
                info.sender = result->get_value(i, "sender");
                info.recipient = result->get_value(i, "recipient");
                info.subject = result->get_value(i, "subject");
                info.body_path = result->get_value(i, "body_path");
                info.is_starred = result->get_value(i, "is_starred") == "1";
                info.is_deleted = result->get_value(i, "is_deleted") == "1";
                info.is_important = result->get_value(i, "is_important") == "1";
                info.status = result->get_value(i, "status").empty() ? 0 : static_cast<int>(safe_stoull(result->get_value(i, "status")));
                info.send_time = static_cast<time_t>(safe_stoull(result->get_value(i, "send_time")));
                mails.push_back(std::move(info));
            }
            cb(true, std::move(mails));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_info_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
    std::function<void(bool, MailboxMailInfo)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false, MailboxMailInfo{}); return; }
    (*conn)->async_query(
        "SELECT id, subject, body_path, UNIX_TIMESTAMP(send_time) AS send_time FROM mails WHERE id = ?",
        {std::to_string(mail_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            MailboxMailInfo info;
            if (!result || result->get_row_count() == 0) { cb(false, info); return; }
            info.mail_id = safe_stoull(result->get_value(0, "id"));
            info.subject = result->get_value(0, "subject");
            info.body_path = result->get_value(0, "body_path");
            info.send_time = static_cast<time_t>(safe_stoull(result->get_value(0, "send_time")));
            cb(true, std::move(info));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_sender_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
    std::function<void(std::string)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(""); return; }
    (*conn)->async_query(
        "SELECT sender FROM mail_recipients WHERE mail_id = ? LIMIT 1",
        {std::to_string(mail_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0) cb(result->get_value(0, "sender"));
            else cb("");
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mail_recipients_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mail_id,
    std::function<void(std::vector<std::string>)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb({}); return; }
    (*conn)->async_query(
        "SELECT recipient FROM mail_recipients WHERE mail_id = ?",
        {std::to_string(mail_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            std::vector<std::string> recipients;
            if (result) {
                for (size_t i = 0; i < result->get_row_count(); ++i) {
                    recipients.push_back(result->get_value(i, "recipient"));
                }
            }
            cb(std::move(recipients));
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_user_email_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id,
    std::function<void(std::string)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(""); return; }
    (*conn)->async_query(
        "SELECT mail_address FROM users WHERE id = ?",
        {std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0) cb(result->get_value(0, "mail_address"));
            else cb("");
        });
}

// 邮件持久化（APPEND）。storage 写入是阻塞 I/O，调用方须在 worker 线程发起。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::create_mail_async(
    const std::shared_ptr<storage::IStorageProvider>& storage,
    std::shared_ptr<ScopedConnection> conn,
    const std::string& subject, const std::string& body_content,
    std::function<void(uint64_t, std::string, std::string)> cb)
{
    if (!cb) return;
    if (!conn || !conn->is_valid()) { cb(0, "", "DB connection failed"); return; }
    int64_t mail_id = algorithm::get_snowflake_generator().next_id();

    std::string body_path;
    if (storage) {
        std::string storage_key = storage->build_mail_body_key(static_cast<uint64_t>(mail_id));
        storage::IoError err;
        if (!storage->append_binary(storage_key, body_content.data(),
                                    body_content.size(), err)) {
            cb(0, "", "Storage error: " + err.message);
            return;
        }
        body_path = storage_key;
    } else {
        std::string base = "mail/";
        std::string fp = base + std::to_string(mail_id);
        std::ofstream out(fp, std::ios::binary);
        if (!out) { cb(0, "", "Cannot write " + fp); return; }
        out.write(body_content.data(), static_cast<std::streamsize>(body_content.size()));
        if (!out) { cb(0, "", "Write failed " + fp); return; }
        body_path = fp;
    }

    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    (*conn)->async_execute(db::sql::build_imap_append_mail_metadata(),
        {std::to_string(mail_id),
         subject.empty() ? "(无主题)" : subject,
         body_path, std::to_string(ts)},
        [conn, mail_id = static_cast<uint64_t>(mail_id), body_path = std::move(body_path),
         cb = std::move(cb)](bool ok) mutable {
            if (ok) cb(mail_id, std::move(body_path), "");
            else cb(0, "", "Insert mail record failed");
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::link_mail_to_mailbox_async(
    std::shared_ptr<ScopedConnection> conn,
    uint64_t mail_id, uint64_t user_id, uint64_t mailbox_id,
    const std::string& sender, const std::string& recipient,
    int status, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    int64_t rid = algorithm::get_snowflake_generator().next_id();
    (*conn)->async_execute(db::sql::build_imap_append_mail_recipient(),
        {std::to_string(rid), std::to_string(mail_id), sender, recipient, std::to_string(status)},
        [conn, mail_id, mailbox_id, user_id, cb = std::move(cb)](bool ok) mutable {
            if (!ok) { cb(false); return; }
            // mail_mailbox.id 是 AUTO_INCREMENT，省略由 DB 生成；
            // UNIQUE KEY uk_mail_box_user 防重复关联
            (*conn)->async_execute(
                "INSERT INTO mail_mailbox (mail_id, mailbox_id, user_id, is_starred, "
                "is_important, is_deleted, add_time) VALUES (?, ?, ?, 0, 0, 0, NOW())",
                {std::to_string(mail_id), std::to_string(mailbox_id), std::to_string(user_id)},
                [cb = std::move(cb)](bool ok2) mutable { cb(ok2); });
        });
}

// IMAP-UTF-7 解码（RFC 3501 §5.1.3）
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::decode_imap_utf7(const std::string& imap7)
{
    static const int rev[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };

    std::string result;
    size_t i = 0;
    while (i < imap7.size()) {
        char c = imap7[i];
        if (c == '&') {
            if (i + 1 < imap7.size() && imap7[i + 1] == '-') {
                result += '&';
                i += 2;
            } else {
                size_t end = imap7.find('-', i + 1);
                if (end == std::string::npos) { result += c; i++; continue; }
                std::string b64 = imap7.substr(i + 1, end - i - 1);
                i = end + 1;

                std::vector<uint8_t> bytes;
                uint32_t acc = 0;
                int bits = 0;
                for (char bc : b64) {
                    if (bc < 0 || bc >= 128) continue;
                    int v = rev[static_cast<int>(bc)];
                    if (v < 0) continue;
                    acc = (acc << 6) | static_cast<uint32_t>(v);
                    bits += 6;
                    if (bits >= 8) {
                        bits -= 8;
                        bytes.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
                    }
                }

                for (size_t j = 0; j + 1 < bytes.size(); j += 2) {
                    uint16_t unit = (static_cast<uint16_t>(bytes[j]) << 8) | bytes[j + 1];
                    if (unit >= 0xD800 && unit <= 0xDBFF && j + 3 < bytes.size()) {
                        uint16_t low = (static_cast<uint16_t>(bytes[j + 2]) << 8) | bytes[j + 3];
                        uint32_t cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                        result += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                        j += 2;
                    } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                        continue;
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

// 缓存感知的邮箱统计（CPS）+ single-flight：同一 (user,mailbox) 只允许一个回源
// 查询链在途。缓存 miss/stale 时，首个连接成为 flight owner 发起查询，其余并发
// 连接挂到 owner 的等待列表上；owner 完成后写回缓存并统一通知所有等待者，任何
// 连接都不会挂死。防止 N 个客户端并发 SELECT 同一邮箱时各自查库打爆数据库。
//
// 锁序：flight 锁 → 缓存锁（仅 recheck 短暂取）。owner 完成路径是 cache_put
// （取放缓存锁）→ 再取 flight 锁移除条目，两锁从不嵌套持有，无锁序反转。
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_stats_cached_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t user_id, uint64_t mailbox_id,
    std::function<void(MailboxCacheEntry, bool, bool)> cb)
{
    if (!cb) return;
    std::string key = mbox_cache_key(user_id, mailbox_id);

    // 1. 快路径：缓存命中且未过期，同步触发回调
    MailboxCacheEntry cached;
    bool had_entry = false, stale = false;
    if (m_mailboxStatsCache &&
        m_mailboxStatsCache->get(key, cached, stale)) {
        had_entry = true;
        if (!stale) {
            cb(cached, true, false);
            return;
        }
    }

    // 2. single-flight：拿 flight 锁。已有在途 → 挂等待回调，等 owner 完成统一通知；
    //    没有在途 → 复查缓存（owner 可能刚写完缓存还没来得及摘 flight；复查在
    //    flight 锁内做，消除"刚释放缓存锁数据才到"的窗口，也无需 cache+flight 二重锁）；
    //    仍 miss/stale → 创建 flight，自己同时是 owner 和第一个 waiter。
    std::shared_ptr<StatsFlight> flight;
    {
        std::lock_guard<std::mutex> lk(m_statsFlightsMutex);
        auto it = m_statsFlights.find(key);
        if (it != m_statsFlights.end()) {
            it->second->waiters.push_back(std::move(cb));
            return;
        }
        if (m_mailboxStatsCache) {
            MailboxCacheEntry recheck;
            bool recheck_stale = false;
            if (m_mailboxStatsCache->get(key, recheck, recheck_stale) && !recheck_stale) {
                cb(recheck, true, false);
                return;
            }
        }
        flight = std::make_shared<StatsFlight>();
        flight->waiters.push_back(std::move(cb));
        m_statsFlights[key] = flight;
    }

    // 3. owner：回源查询链 count → unseen → uidnext，完成后写缓存 + 通知所有等待者
    auto finish = [this, key, flight, had_entry, stale](MailboxCacheEntry entry) {
        if (m_mailboxStatsCache) {
            m_mailboxStatsCache->put(key, entry);
        }
        std::vector<std::function<void(MailboxCacheEntry, bool, bool)>> waiters;
        {
            std::lock_guard<std::mutex> lk(m_statsFlightsMutex);
            m_statsFlights.erase(key);
            if (flight) waiters = std::move(flight->waiters);
        }
        // 释放 flight 锁后才回调：等待者的 cb 可能重入本函数（drain 触发新 SELECT），
        // 持锁回调会非递归锁死锁。
        for (auto& w : waiters) {
            if (w) w(entry, had_entry, stale);
        }
    };

    get_mailbox_count_async(conn, mailbox_id, user_id,
        [conn, mailbox_id, user_id, finish = std::move(finish)](size_t exists) mutable {
            get_mailbox_unseen_count_async(conn, mailbox_id, user_id,
                [conn, mailbox_id, user_id, finish = std::move(finish), exists](size_t unseen) mutable {
                    get_mailbox_uidnext_async(conn, mailbox_id, user_id,
                        [mailbox_id, finish = std::move(finish), exists, unseen](uint64_t uidnext) mutable {
                            MailboxCacheEntry entry;
                            entry.exists = exists;
                            entry.unseen = unseen;
                            entry.uidnext = uidnext;
                            entry.uidvalidity = mailbox_id;
                            finish(entry);
                        });
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_count_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    (*conn)->async_query(db::sql::build_imap_select_status_total(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0)
                cb(static_cast<size_t>(safe_stoull(result->get_value(0, "cnt"))));
            else cb(0);
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_unseen_count_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    (*conn)->async_query(db::sql::build_imap_mailbox_unseen_count(),
        {std::to_string(user_id), std::to_string(mailbox_id), std::to_string(user_id)},
        [conn, cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (result && result->get_row_count() > 0)
                cb(static_cast<size_t>(safe_stoull(result->get_value(0, "cnt"))));
            else cb(0);
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::get_mailbox_uidnext_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(uint64_t)> cb)
{
    (void)user_id;   // mailbox_id 是 mailboxes 主键，全局唯一，无需 user_id 过滤
    if (!conn || !conn->is_valid()) { if (cb) cb(1); return; }
    // 原子推进高水位：行锁串行化并发/跨实例读者，GREATEST 防 expunge 后回退；
    // 推进后再读取（advance 是写，read 才是值）
    (*conn)->async_execute(db::sql::build_imap_uidnext_advance(),
        {std::to_string(mailbox_id), std::to_string(mailbox_id), std::to_string(mailbox_id)},
        [conn, mailbox_id, cb = std::move(cb)](bool) mutable {
            (*conn)->async_query(db::sql::build_imap_uidnext_read(),
                {std::to_string(mailbox_id)},
                [cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
                    if (result && result->get_row_count() > 0)
                        cb(safe_stoull(result->get_value(0, "uidnext")));
                    else cb(1);
                });
        });
}

// ---------- 批量 flag 更新（STORE）：单条 IN SQL 拍平 N 循环 ----------

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_seen_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    const std::string& recipient, int status, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_recipients SET status = " + std::to_string(status)
        + " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND recipient = '" + (*conn)->escape_string(recipient) + "'";
    (*conn)->async_execute(sql, [cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_deleted_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t user_id, uint64_t mailbox_id, int deleted, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_mailbox SET is_deleted = " + std::to_string(deleted)
        + " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND user_id = " + std::to_string(user_id)
        + " AND mailbox_id = " + std::to_string(mailbox_id);
    (*conn)->async_execute(sql, [cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_flagged_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t user_id, uint64_t mailbox_id, int flagged, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_mailbox SET is_starred = " + std::to_string(flagged)
        + " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND user_id = " + std::to_string(user_id)
        + " AND mailbox_id = " + std::to_string(mailbox_id);
    (*conn)->async_execute(sql, [cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::expunge_mailbox_async(
    std::shared_ptr<ScopedConnection> conn, uint64_t mailbox_id, uint64_t user_id,
    std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    (*conn)->async_execute(db::sql::build_imap_expunge_delete_mailbox(),
        {std::to_string(mailbox_id), std::to_string(user_id)},
        [cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

// ---------- COPY/MOVE 批量 ----------

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_insert_mailbox_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t target_id, uint64_t user_id, std::function<void(size_t)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(0); return; }
    if (mail_ids.empty()) { if (cb) cb(0); return; }
    // INSERT IGNORE：uk_mail_box_user 自动去重（无需逐封查重），id 由
    // AUTO_INCREMENT 生成。实际插入数用 SELECT ROW_COUNT() 读（dup 被
    // IGNORE 的不计入）。
    std::string values;
    bool first = true;
    for (uint64_t mid : mail_ids) {
        if (!first) values += ",";
        values += "(" + std::to_string(mid) + "," + std::to_string(target_id) + ","
               + std::to_string(user_id) + ",0,0,0,NOW())";
        first = false;
    }
    std::string sql = "INSERT IGNORE INTO mail_mailbox "
        "(mail_id, mailbox_id, user_id, is_starred, is_important, is_deleted, add_time) "
        "VALUES " + values;
    (*conn)->async_execute(sql,
        [conn, cb = std::move(cb)](bool ok) mutable {
            if (!ok) { if (cb) cb(0); return; }
            (*conn)->async_query(db::sql::build_select_row_count(),
                [cb = std::move(cb)](std::shared_ptr<IDBResult> r) mutable {
                    size_t copied = (r && r->get_row_count() > 0)
                        ? static_cast<size_t>(safe_stoull(r->get_value(0, "affected"))) : 0;
                    cb(copied);
                });
        });
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::batch_mark_move_deleted_async(
    std::shared_ptr<ScopedConnection> conn, const std::vector<uint64_t>& mail_ids,
    uint64_t user_id, uint64_t source_mailbox_id, std::function<void(bool)> cb)
{
    if (!conn || !conn->is_valid()) { if (cb) cb(false); return; }
    if (mail_ids.empty()) { if (cb) cb(true); return; }
    std::string sql = "UPDATE mail_mailbox SET is_deleted = 1"
        " WHERE mail_id IN (" + join_mail_ids(mail_ids) + ")"
        + " AND user_id = " + std::to_string(user_id)
        + " AND mailbox_id = " + std::to_string(source_mailbox_id);
    (*conn)->async_execute(sql, [cb = std::move(cb)](bool ok) mutable { if (cb) cb(ok); });
}

} // namespace mail_system

#endif // TRADITIONAL_IMAPS_FSM_TPP
