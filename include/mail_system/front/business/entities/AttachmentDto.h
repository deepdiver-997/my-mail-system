#pragma once

#include <string>

namespace mail_system {
namespace front {
namespace business {
namespace entities {

struct AttachmentDto
{
    size_t id;                  // 主键
    size_t mail_id;             // 所属邮件ID
    std::string filename;       // 文件名
    std::string filepath;       // 文件路径
    size_t filesize;            // 文件大小
};

} // namespace entities
} // namespace business
} // namespace front
} // namespace mail_system