#ifndef MAIL_SYSTEM_IMAP_UTILS_H
#define MAIL_SYSTEM_IMAP_UTILS_H

// IMAP FSM 的纯工具函数（非模板）——从 traditional_imaps_fsm.tpp 拆出。
// 全部与 ConnectionType 无关：非模板 .cpp 只编译一次（原来 .tpp 每个
// ConnectionType 实例化都编译一遍），可独立单测。
//
// 含：RFC 3501 响应构建（timestamp/quoted-string/ENVELOPE/BODYSTRUCTURE/FLAGS）、
// modified UTF-7 编解码、序列号集/ID 列表助手。

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct MimePart;   // 定义在 common/mime_parser.h（.cpp 里 include）

namespace mail_system {
namespace imap_utils {

// RFC 3501 date-time: DD-Mon-YYYY HH:MM:SS +ZZZZ
std::string imap_timestamp(time_t t);

// RFC 3501 quoted-string（IMAP-UTF-7 编码名（& 开头）也要加引号）
std::string quote_string(const std::string& s);

// modified UTF-7 编解码（RFC 3501 §5.1.3）
std::string encode_mailbox_name(const std::string& name);
std::string decode_mailbox_name(const std::string& imap7);
std::string decode_imap_utf7(const std::string& imap7);

// 响应片段构建
std::string build_flags_string(int status, bool starred, bool deleted, bool important);
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
    const std::string& message_id);
std::string build_fetch_body_response(const std::string& body_content, size_t octets);

// BODYSTRUCTURE 构建（RFC 3501 §7.4.2）+ MIME part 正文提取
std::string build_bodystructure(const std::string& raw);
std::string build_bodystructure_tree(const MimePart& mp);
std::string extract_part_content(const std::string& raw, const MimePart& part);

// 展开逗号分隔的序列号集（"1" / "1:*" / "1,3,5" / "1:3,5" / "*"）→ (start,end)
// 闭区间列表（已 clamp 到 [1,total]）
void expand_seq_set(const std::string& seq_set, size_t total,
                    std::vector<std::pair<uint64_t, uint64_t>>& ranges);

// 数值 ID 列表 → "a,b,c"（批量 IN 语句用；全数值，无注入面）
std::string join_mail_ids(const std::vector<uint64_t>& ids);

} // namespace imap_utils
} // namespace mail_system

#endif // MAIL_SYSTEM_IMAP_UTILS_H
