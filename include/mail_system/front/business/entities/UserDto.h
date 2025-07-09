#pragma once

#include <string>

namespace mail_system {
namespace front {
namespace business {
namespace entities {

struct UserDto
{
    size_t id;                  // 主键
    std::string username;       // 用户名
    std::string email;          // 邮箱地址
    std::string nickname;       // 昵称
    std::string avatar;         // 头像路径
};

} // namespace entities
} // namespace business
} // namespace front
} // namespace mail_system