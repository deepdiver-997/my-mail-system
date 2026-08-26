#ifndef MAIL_SYSTEM_MX_ROUTING_UTILS_H
#define MAIL_SYSTEM_MX_ROUTING_UTILS_H

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/mailServer/outbound/dns_resolver.h"
#include "mail_system/back/mailServer/outbound/outbound_config.h"
#include "mail_system/back/mailServer/outbound/outbox_repository.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace outbound {

// 判断收件人是否属于外部域（需要 DNS 解析 + 外部 SMTP 投递）
bool has_external_recipient(const mail& mail_data, const std::string& local_domain);

std::vector<std::string> build_target_hosts(const OutboxRecord& record,
                                            IDnsResolver* resolver,
                                            const std::unordered_map<std::string, OutboundConfig::StaticRoute>* static_routes = nullptr);

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_MX_ROUTING_UTILS_H
