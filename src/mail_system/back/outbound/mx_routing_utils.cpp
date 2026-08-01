#include "mail_system/back/outbound/mx_routing_utils.h"
#include "mail_system/back/common/logger.h"

#include <unordered_set>

namespace mail_system {
namespace outbound {
namespace {

std::string extract_domain(const std::string& email) {
    const auto at_pos = email.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= email.size()) {
        return {};
    }
    return email.substr(at_pos + 1);
}

std::string trim_trailing_dot(std::string host) {
    while (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    return host;
}

bool is_ipv6_literal(const std::string& address) {
    return address.find(':') != std::string::npos;
}

} // namespace

bool has_external_recipient(const mail& mail_data, const std::string& local_domain) {
    for (const auto& recipient : mail_data.to) {
        const auto domain = extract_domain(recipient);
        if (!domain.empty() && domain != local_domain) {
            LOG_OUTBOUND_DEBUG("Recipient {} is external (domain: {})", recipient, domain);
            return true;
        }
    }
    return false;
}

std::vector<std::string> build_target_hosts(const OutboxRecord& record,
                                            IDnsResolver* resolver) {
    std::vector<std::string> hosts_v4;
    std::vector<std::string> hosts_v6;
    std::unordered_set<std::string> unique_hosts;
    const auto domain = extract_domain(record.recipient);

    if (resolver && !domain.empty()) {
        SyncDnsWrapper sync(*resolver);
        auto mx_records = sync.resolve_mx(domain);
        for (const auto& mx : mx_records) {
            auto mx_host = trim_trailing_dot(mx.host);
            if (mx_host.empty()) {
                continue;
            }

            auto addresses = sync.resolve_host_addresses(mx_host);
            for (const auto& addr : addresses) {
                if (!addr.empty() && unique_hosts.insert(addr).second) {
                    if (is_ipv6_literal(addr)) {
                        hosts_v6.push_back(addr);
                    } else {
                        hosts_v4.push_back(addr);
                    }
                }
            }
        }

        // RFC 5321 implicit MX fallback: when no MX hosts were found,
        // treat the recipient domain itself as the mail host target.
        if (hosts_v4.empty() && hosts_v6.empty()) {
            auto addresses = sync.resolve_host_addresses(domain);
            for (const auto& addr : addresses) {
                if (!addr.empty() && unique_hosts.insert(addr).second) {
                    if (is_ipv6_literal(addr)) {
                        hosts_v6.push_back(addr);
                    } else {
                        hosts_v4.push_back(addr);
                    }
                }
            }
        }
    }

    std::vector<std::string> hosts;
    hosts.reserve(hosts_v4.size() + hosts_v6.size());
    hosts.insert(hosts.end(), hosts_v4.begin(), hosts_v4.end());
    hosts.insert(hosts.end(), hosts_v6.begin(), hosts_v6.end());

    return hosts;
}

} // namespace outbound
} // namespace mail_system
