#ifndef MAIL_SYSTEM_OUTBOUND_UTILS_H
#define MAIL_SYSTEM_OUTBOUND_UTILS_H

#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/outbound/outbound_config.h"
#include "mail_system/back/entities/mail.h"

#include <string>

namespace mail_system {
namespace outbound {

bool ensure_mail_raw_payload_loaded(mail& mail_data);

// Build RFC5322 wire message and optionally include DKIM-Signature.
std::string build_outbound_message(const OutboxRecord& record,
                                   const mail* hot_mail,
                                   const std::string& header_from,
                                   const OutboundConfig& identity_config,
                                   bool* dkim_applied,
                                   std::string* dkim_error,
                                   std::string* message_id_out = nullptr);

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_OUTBOUND_UTILS_H
