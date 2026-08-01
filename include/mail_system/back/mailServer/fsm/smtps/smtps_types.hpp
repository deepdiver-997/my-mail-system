#ifndef SMTPS_FSM_H
#define SMTPS_FSM_H

#include "framework/server_config.h"
#include "mail_system/back/entities/mail.h"
#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace mail_system {

// SMTPS 状态枚举
enum class SmtpsState {
    INIT = 0,
    GREETING = 1,
    WAIT_EHLO = 2,
    WAIT_AUTH = 3,
    WAIT_AUTH_USERNAME = 4,
    WAIT_AUTH_PASSWORD = 5,
    WAIT_MAIL_FROM = 6,
    WAIT_RCPT_TO = 7,
    WAIT_DATA = 8,
    IN_MESSAGE = 9,
    WAIT_QUIT = 10,
    CLOSED = 11,
    COUNT = 12
};

// SMTPS 事件枚举
enum class SmtpsEvent {
    CONNECT = 0,
    EHLO = 1,
    AUTH = 2,
    MAIL_FROM = 3,
    RCPT_TO = 4,
    DATA = 5,
    DATA_END = 6,
    QUIT = 7,
    STARTTLS = 8,
    ERROR = 9,
    TIMEOUT = 10,
    COUNT = 11
};

// SMTPS 上下文
struct SmtpsContext {
    ListenerConfig listener_config;               // 当前连接所属监听器配置
    bool is_authenticated = false;               // AUTH 是否通过，决定后续命令是否允许
    bool plain_auth_expected = false;            // AUTH PLAIN 多步流程：等待客户端发送 base64 凭证
    std::string client_username;                 // AUTH 登录名；MAIL FROM 缺省时作为发件人
    std::string sender_address;                  // MAIL FROM 解析出的地址
    std::vector<std::string> recipient_addresses;// RCPT TO 收件人列表
    std::string mail_data;                       // 旧逻辑保留：完整邮件字符串缓冲（非流式时）
    std::string parsed_subject;                  // 从头部解析出的Subject
    std::string source_message_id;               // 从头部解析出的Message-ID

    // streaming / MIME parsing helpers
    bool header_parsed = false;                  // DATA 阶段是否已解析完邮件头
    bool streaming_enabled = false;              // 是否启用流式写盘（检测到大体或 multipart 后）
    bool multipart = false;                      // 邮件是否为 multipart/*
    bool has_attachment = false;                 // 是否检测到 Content-Disposition: attachment
    std::string boundary;                        // multipart 边界字符串
    std::string header_buffer;                   // 收集头部行直到遇到空行
    std::string line_buffer;                     // 行级缓冲，处理 CRLF 与前导点去除
    std::string text_body_buffer;                // 纯文本体累积（非流式或未触发刷盘时）
    size_t text_body_size = 0;                   // 已接收正文总字节数（逻辑计数）
    size_t buffered_body_size = 0;               // 已缓存在 text_body_buffer 的字节数
    bool body_limit_exceeded = false;            // 超过大小限制时标记，后续可能拒收
    std::string abort_reason;                    // 中断原因描述，便于日志

    std::string current_part_headers;            // 当前 MIME 部分的头部缓存
    bool in_part_header = false;                 // 是否仍在当前部分头部区域
    bool current_part_is_attachment = false;     // 当前部分是否为附件
    std::string current_part_encoding;           // 当前部分的 Content-Transfer-Encoding
    std::string current_part_mime;               // 当前部分的 Content-Type
    std::string current_attachment_filename;     // 当前附件文件名（来自 MIME 头）
    std::string current_attachment_path;         // 当前附件落盘路径
    std::ofstream current_attachment_stream;     // 备用流句柄（现以缓冲方式为主）
    std::string base64_remainder;                // Base64 断行余数，拼接下一块使用
    size_t current_attachment_size = 0;          // 当前附件累计写入大小
    std::vector<attachment> streamed_attachments;// 已完成的附件元数据，DATA_END 时搬运到 mail

    // 入站验证相关
    std::string ehlo_domain;                   // EHLO/HELO 客户端声明的域名
    bool is_trusted_server = false;            // EHLO 验证通过（PTR 匹配），auto 模式跳过 AUTH
    std::string auth_results_header;           // 验证后注入的 Authentication-Results 头
    bool verification_run = false;             // 本次事务是否已执行验证
    bool spf_checked = false;                  // SPF 已在 MAIL FROM 阶段验证
    std::string spf_result;                    // MAIL FROM 阶段的 SPF 结果
    std::string spf_reason;                    // SPF 失败原因
    std::string dkim_result;                   // DKIM 验证结果
    std::string dmarc_result;                  // DMARC 验证结果
    int shard_index = 0;                       // 由 shard router 在认证时分配

    // 附件缓冲区相关（采用与邮件相同的缓冲策略）
    size_t attachment_buffer_size = 0;           // 当前附件缓冲区大小
    std::unique_ptr<char[]> attachment_buffer;   // 附件数据缓冲指针
    size_t attachment_buffer_used = 0;           // 附件缓冲已使用字节数
    size_t attachment_buffer_expand_count = 0;   // 附件缓冲扩容次数，超过阈值转刷盘

    void clear() {
        is_authenticated = false;
        client_username.clear();
        sender_address.clear();
        recipient_addresses.clear();
        mail_data.clear();
        parsed_subject.clear();
        source_message_id.clear();
        header_parsed = false;
        streaming_enabled = false;
        multipart = false;
        has_attachment = false;
        text_body_size = 0;
        buffered_body_size = 0;
        body_limit_exceeded = false;
        abort_reason.clear();
        boundary.clear();
        header_buffer.clear();
        line_buffer.clear();
        text_body_buffer.clear();
        current_part_headers.clear();
        in_part_header = false;
        current_part_is_attachment = false;
        current_part_encoding.clear();
        current_part_mime.clear();
        current_attachment_filename.clear();
        current_attachment_path.clear();
        base64_remainder.clear();
        current_attachment_size = 0;
        streamed_attachments.clear();
        attachment_buffer_used = 0;
        attachment_buffer_expand_count = 0;
        ehlo_domain.clear();
        is_trusted_server = false;
        auth_results_header.clear();
        verification_run = false;
        spf_checked = false;
        spf_result.clear();
        spf_reason.clear();
        shard_index = 0;
        if (current_attachment_stream.is_open()) {
            current_attachment_stream.close();
        }
    }
};

} // namespace mail_system

#endif // SMTPS_FSM_H
