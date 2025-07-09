#pragma once

#include <string>

namespace mail_system {
namespace front {
namespace business {
namespace entities {

struct MailboxDto
{
    size_t id;                  // 主键
    size_t user_id;             // 所属用户ID
    std::string name;           // 邮箱名称
    bool is_default;            // 是否为默认邮箱
};

} // namespace entities
} // namespace business
} // namespace front
} // namespace mail_system