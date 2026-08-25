#ifndef MAIL_SYSTEM_OUTBOUND_CONFIG_H
#define MAIL_SYSTEM_OUTBOUND_CONFIG_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace outbound {

struct OutboundConfig {
    // === Identity / DKIM ===
    std::string helo_domain = "outbound.local";
    std::string mail_from_domain;
    bool rewrite_header_from = true;
    bool dkim_enabled = false;
    std::string dkim_selector = "default";
    std::string dkim_domain;
    std::string dkim_private_key_file;

    // === Delivery ===
    std::vector<uint16_t> ports = {25, 587, 465};
    size_t max_attempts = 8;

    // === Static MX routes (跳过 DNS, domain → host:port) ===
    struct StaticRoute { std::string host; uint16_t port = 25; };
    std::unordered_map<std::string, StaticRoute> static_routes;

    // === Polling backoff ===
    int busy_sleep_ms     = 20;
    int backoff_base_ms   = 50;
    int backoff_max_ms    = 1200;
    size_t backoff_shift_cap = 6;
};

} // namespace outbound
} // namespace mail_system
#endif
