#ifndef SMTP_UTILS_H
#define SMTP_UTILS_H

#include <string>
#include <unordered_map>
#include <regex>
#include <sstream>
#include <vector>
#include <memory>
#include <functional>
#include <future>

// 前向声明
namespace mail_system {
    struct SmtpsContext;
}

namespace mail_system {
namespace algorithm {

// 工具函数声明
std::string to_lower(std::string s);
std::string trim(const std::string& str);
std::string sanitize_filename(const std::string& name);
std::string ensure_trailing_slash(std::string path);

// MIME 解析相关函数声明
std::string extract_boundary(const std::string& content_type);
std::unordered_map<std::string, std::string> parse_headers_map(const std::string& header_block);
std::string get_header_value(const std::unordered_map<std::string, std::string>& headers, const std::string& key);

// Base64 编解码
std::string decode_base64(const std::string& input);

// 邮件解析函数（从 SmtpsSession 移过来）
void analyze_top_header(SmtpsContext& ctx);

void handle_part_header_parsed(SmtpsContext& ctx);

void handle_multipart_line(SmtpsContext& ctx, const std::string& line);

void process_message_data(SmtpsContext& ctx, const std::string& data);

void cleanup_streamed_attachments(SmtpsContext& ctx);

} // namespace algorithm
} // namespace mail_system

#endif // SMTP_UTILS_H
