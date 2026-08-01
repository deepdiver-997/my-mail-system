#ifndef MAIL_SYSTEM_OUTBOUND_CONFIG_H
#define MAIL_SYSTEM_OUTBOUND_CONFIG_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

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

    static OutboundConfig from_json(const nlohmann::json& j,
                                     const std::string& base_dir = "") {
        OutboundConfig cfg;

        auto resolve = [&](const std::string& path) {
            if (path.empty() || path[0] == '/' || base_dir.empty()) return path;
            return base_dir + "/" + path;
        };

        cfg.helo_domain         = j.value("helo_domain", cfg.helo_domain);
        cfg.mail_from_domain    = j.value("mail_from_domain", cfg.mail_from_domain);
        cfg.rewrite_header_from = j.value("rewrite_header_from", cfg.rewrite_header_from);
        cfg.max_attempts        = j.value("max_attempts", cfg.max_attempts);

        if (j.contains("dkim")) {
            auto& d = j["dkim"];
            cfg.dkim_enabled          = d.value("enabled", cfg.dkim_enabled);
            cfg.dkim_selector         = d.value("selector", cfg.dkim_selector);
            cfg.dkim_domain           = d.value("domain", cfg.dkim_domain);
            cfg.dkim_private_key_file = resolve(
                d.value("private_key_file", cfg.dkim_private_key_file));
        }

        if (j.contains("polling")) {
            auto& p = j["polling"];
            cfg.busy_sleep_ms     = p.value("busy_sleep_ms", cfg.busy_sleep_ms);
            cfg.backoff_base_ms   = p.value("backoff_base_ms", cfg.backoff_base_ms);
            cfg.backoff_max_ms    = p.value("backoff_max_ms", cfg.backoff_max_ms);
            cfg.backoff_shift_cap = p.value("backoff_shift_cap", cfg.backoff_shift_cap);
        }

        if (j.contains("ports") && j["ports"].is_array()) {
            cfg.ports.clear();
            for (auto& p : j["ports"])
                if (p.is_number())
                    cfg.ports.push_back(static_cast<uint16_t>(p.get<uint32_t>()));
        }

        if (j.contains("static_routes") && j["static_routes"].is_object()) {
            for (auto& [domain, route] : j["static_routes"].items()) {
                cfg.static_routes[domain] = {
                    route.value("host", "127.0.0.1"),
                    static_cast<uint16_t>(route.value("port", 25))
                };
            }
        }

        return cfg;
    }
};

} // namespace outbound
} // namespace mail_system
#endif
