#pragma once

#include <string>
#include <ctime>

namespace mail_system {
namespace front {
namespace business {
namespace entities {

struct MailDto
{
    size_t id;                  // 主键
    std::string from;           // 发件人邮箱地址
    std::string to;             // 收件人邮箱地址
    std::string header;         // 邮件头
    std::string body;           // 邮件正文
    time_t send_time;           // 发送时间
    bool is_draft;              // 是否为草稿
    bool is_read;              // 是否已读
};

} // namespace entities
} // namespace business
} // namespace front
} // namespace mail_system