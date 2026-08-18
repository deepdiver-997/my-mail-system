#include "mail_system/back/inbound/inbound_verifier.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/common/lru_cache.h"
#include "framework/server_config.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace mail_system {
namespace inbound {
namespace {

// ---------- DNS 记录缓存 TTL（阶段 2） ----------
// SPF/DMARC/DKIM 均为 DNS TXT 记录（非 X.509 证书），有 DNS TTL。
// 当前 IDnsResolver 不返回 TTL，故用固定 TTL + 负缓存短 TTL，到期自动重查，不会永久用旧值。
constexpr std::chrono::seconds kSpfTtl{300};      // SPF 记录：5 min
constexpr std::chrono::seconds kDmarcTtl{900};    // DMARC 记录：15 min
constexpr std::chrono::seconds kDkimTtl{3600};    // DKIM 公钥：1 h
constexpr std::chrono::seconds kNegativeTtl{60};  // 负缓存（无记录/失败）：1 min

// ---------- helpers ----------

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

void split(const std::string& s, char delim, std::vector<std::string>& out) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(trim(item));
    }
}

// Parse a simple tag=value map from a DKIM-Signature value.
// Tags are separated by ; values may contain =
std::unordered_map<std::string, std::string> parse_tags(const std::string& raw) {
    std::unordered_map<std::string, std::string> tags;
    std::string current;
    bool in_quotes = false;
    std::string tag_name;
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '"') { in_quotes = !in_quotes; continue; }
        if (ch == ';' && !in_quotes) {
            // end of tag
            std::string tag = trim(current);
            auto eq = tag.find('=');
            if (eq != std::string::npos) {
                tag_name = trim(tag.substr(0, eq));
                tags[tag_name] = trim(tag.substr(eq + 1));
            }
            current.clear();
            continue;
        }
        current += ch;
    }
    // last tag (no trailing ;)
    if (!current.empty()) {
        std::string tag = trim(current);
        auto eq = tag.find('=');
        if (eq != std::string::npos) {
            tag_name = trim(tag.substr(0, eq));
            tags[tag_name] = trim(tag.substr(eq + 1));
        }
    }
    return tags;
}

// Parse raw headers into lowercase-key -> value map
std::unordered_map<std::string, std::string> parse_headers_map(const std::string& raw_headers) {
    std::unordered_map<std::string, std::string> out;
    std::istringstream ss(raw_headers);
    std::string line;
    std::string cur_key;
    std::string cur_val;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break; // end of headers
        if (line[0] == ' ' || line[0] == '\t') {
            // continuation line
            if (!cur_key.empty()) {
                cur_val += " ";
                cur_val += trim(line);
            }
        } else {
            // new header
            if (!cur_key.empty()) {
                out[to_lower(cur_key)] = cur_val;
            }
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                cur_key = line.substr(0, colon);
                cur_val = trim(line.substr(colon + 1));
            }
        }
    }
    if (!cur_key.empty()) {
        out[to_lower(cur_key)] = cur_val;
    }
    return out;
}

// Get DKIM-Signature header values from raw headers.
std::vector<std::string> get_header_values(const std::string& raw_headers,
                                           const std::string& name_lower) {
    std::vector<std::string> values;
    std::istringstream ss(raw_headers);
    std::string line;
    std::string cur_name;
    std::string cur_val;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line[0] == ' ' || line[0] == '\t') {
            if (!cur_name.empty()) {
                cur_val += " " + trim(line);
            }
        } else {
            if (to_lower(cur_name) == name_lower && !cur_val.empty()) {
                values.push_back(cur_val);
            }
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                cur_name = line.substr(0, colon);
                cur_val = trim(line.substr(colon + 1));
            } else {
                cur_name.clear();
                cur_val.clear();
            }
        }
    }
    if (to_lower(cur_name) == name_lower && !cur_val.empty()) {
        values.push_back(cur_val);
    }
    return values;
}

} // namespace

// ========== InboundVerifier ==========

InboundVerifier::InboundVerifier(outbound::IDnsResolver& dns) : dns_(dns) {}

SpfResult InboundVerifier::check_spf_only(outbound::IDnsResolver& dns,
                                          const std::string& client_ip,
                                          const std::string& mail_from,
                                          const std::string& helo_domain) {
    InboundVerifier verifier(dns);
    return verifier.check_spf(client_ip, mail_from, helo_domain);
}

void InboundVerifier::check_spf_only_async(outbound::IDnsResolver& dns,
                                           const std::string& client_ip,
                                           const std::string& mail_from,
                                           const std::string& helo_domain,
                                           std::function<void(SpfResult)> cb) {
    // shared_ptr 持有 verifier，保证异步链期间 this 存活（dns 由调用方 server 持有）
    auto verifier = std::make_shared<InboundVerifier>(dns);
    verifier->check_spf_async(client_ip, mail_from, helo_domain,
        [verifier, cb = std::move(cb)](SpfResult r) mutable { cb(std::move(r)); });
}

void InboundVerifier::verify_all_async(outbound::IDnsResolver& dns,
                                       const std::string& client_ip,
                                       const std::string& mail_from,
                                       const std::string& helo_domain,
                                       const std::string& raw_headers,
                                       const std::string& raw_body,
                                       const ServerConfig& config,
                                       std::function<void(VerificationResult)> cb,
                                       const SpfResult* precomputed_spf) {
    auto verifier = std::make_shared<InboundVerifier>(dns);
    auto config_copy = config;
    auto pre = (precomputed_spf ? std::optional<SpfResult>(*precomputed_spf) : std::nullopt);
    auto vr = std::make_shared<VerificationResult>();

    std::function<void()> step_dkim, step_dmarc, step_done;
    step_done = [verifier, cb = std::move(cb), vr]() mutable { cb(std::move(*vr)); };

    step_dmarc = [verifier, vr, step_done, raw_headers, mail_from, config_copy]() {
        if (config_copy.inbound_dmarc_mode != "off") {
            std::string from_domain = extract_from_header_domain(raw_headers);
            std::string mf_domain = extract_domain(mail_from);
            verifier->check_dmarc_async(from_domain, vr->spf, vr->dkim, mf_domain,
                [vr, step_done, from_domain](DmarcResult dm) mutable {
                    vr->dmarc = std::move(dm);
                    vr->dmarc.header_from_domain = from_domain;
                    step_done();
                });
        } else {
            vr->dmarc.result = "none"; vr->dmarc.reason = "DMARC check disabled";
            step_done();
        }
    };

    step_dkim = [verifier, vr, step_dmarc, raw_headers, raw_body, config_copy]() {
        if (config_copy.inbound_dkim_mode != "off") {
            verifier->check_dkim_async(raw_headers, raw_body,
                [vr, step_dmarc](DkimResult dk) mutable { vr->dkim = std::move(dk); step_dmarc(); });
        } else {
            vr->dkim.result = "none"; vr->dkim.reason = "DKIM check disabled";
            step_dmarc();
        }
    };

    if (pre) {
        vr->spf = *pre; step_dkim();
    } else if (config_copy.inbound_spf_mode != "off") {
        verifier->check_spf_async(client_ip, mail_from, helo_domain,
            [vr, step_dkim](SpfResult r) mutable { vr->spf = std::move(r); step_dkim(); });
    } else {
        vr->spf = {"none", "SPF check disabled"}; step_dkim();
    }
}

void InboundVerifier::verify_all_from_file_async(outbound::IDnsResolver& dns,
                                                 const std::string& client_ip,
                                                 const std::string& mail_from,
                                                 const std::string& helo_domain,
                                                 const std::string& raw_headers,
                                                 const std::string& body_path,
                                                 const ServerConfig& config,
                                                 std::function<void(VerificationResult)> cb,
                                                 const SpfResult* precomputed_spf) {
    auto verifier = std::make_shared<InboundVerifier>(dns);
    auto config_copy = config;
    auto pre = (precomputed_spf ? std::optional<SpfResult>(*precomputed_spf) : std::nullopt);
    auto vr = std::make_shared<VerificationResult>();

    std::function<void()> step_dkim, step_dmarc, step_done;
    step_done = [verifier, cb = std::move(cb), vr]() mutable { cb(std::move(*vr)); };

    step_dmarc = [verifier, vr, step_done, raw_headers, mail_from, config_copy]() {
        if (config_copy.inbound_dmarc_mode != "off") {
            std::string from_domain = extract_from_header_domain(raw_headers);
            std::string mf_domain = extract_domain(mail_from);
            verifier->check_dmarc_async(from_domain, vr->spf, vr->dkim, mf_domain,
                [vr, step_done, from_domain](DmarcResult dm) mutable {
                    vr->dmarc = std::move(dm);
                    vr->dmarc.header_from_domain = from_domain;
                    step_done();
                });
        } else {
            vr->dmarc.result = "none"; vr->dmarc.reason = "DMARC check disabled";
            step_done();
        }
    };

    step_dkim = [verifier, vr, step_dmarc, raw_headers, body_path, config_copy]() {
        if (config_copy.inbound_dkim_mode != "off") {
            verifier->check_dkim_from_file_async(raw_headers, body_path,
                [vr, step_dmarc](DkimResult dk) mutable { vr->dkim = std::move(dk); step_dmarc(); });
        } else {
            vr->dkim.result = "none"; vr->dkim.reason = "DKIM check disabled";
            step_dmarc();
        }
    };

    if (pre) {
        vr->spf = *pre; step_dkim();
    } else if (config_copy.inbound_spf_mode != "off") {
        verifier->check_spf_async(client_ip, mail_from, helo_domain,
            [vr, step_dkim](SpfResult r) mutable { vr->spf = std::move(r); step_dkim(); });
    } else {
        vr->spf = {"none", "SPF check disabled"}; step_dkim();
    }
}

void InboundVerifier::verify_all(const std::string& client_ip,
                                 const std::string& mail_from,
                                 const std::string& helo_domain,
                                 const std::string& raw_headers,
                                 const std::string& raw_body,
                                 const ServerConfig& config,
                                 VerificationResult& result,
                                 const SpfResult* precomputed_spf) {
    // SPF
    if (precomputed_spf) {
        result.spf = *precomputed_spf;
    } else if (config.inbound_spf_mode != "off") {
        result.spf = check_spf(client_ip, mail_from, helo_domain);
    } else {
        result.spf = {"none", "SPF check disabled"};
    }

    // DKIM — needs full message
    if (config.inbound_dkim_mode != "off") {
        std::string full_msg = raw_headers + "\r\n\r\n" + raw_body;
        result.dkim = check_dkim(full_msg, raw_body);
    } else {
        result.dkim.result = "none";
        result.dkim.reason = "DKIM check disabled";
    }

    // DMARC
    if (config.inbound_dmarc_mode != "off") {
        std::string from_domain = extract_from_header_domain(raw_headers);
        std::string mf_domain = extract_domain(mail_from);
        result.dmarc = check_dmarc(from_domain, result.spf, result.dkim, mf_domain);
        result.dmarc.header_from_domain = from_domain;
    } else {
        result.dmarc.result = "none";
        result.dmarc.reason = "DMARC check disabled";
    }
}

void InboundVerifier::verify_all_from_file(const std::string& client_ip,
                                           const std::string& mail_from,
                                           const std::string& helo_domain,
                                           const std::string& raw_headers,
                                           const std::string& body_path,
                                           const ServerConfig& config,
                                           VerificationResult& result,
                                           const SpfResult* precomputed_spf) {
    // SPF — 与 verify_all 一致
    if (precomputed_spf) {
        result.spf = *precomputed_spf;
    } else if (config.inbound_spf_mode != "off") {
        result.spf = check_spf(client_ip, mail_from, helo_domain);
    } else {
        result.spf = {"none", "SPF check disabled"};
    }

    // DKIM — 正文从 body 文件流式读取，不全量读回内存
    if (config.inbound_dkim_mode != "off") {
        result.dkim = check_dkim_from_file(raw_headers, body_path);
    } else {
        result.dkim.result = "none";
        result.dkim.reason = "DKIM check disabled";
    }

    // DMARC — 与 verify_all 一致
    if (config.inbound_dmarc_mode != "off") {
        std::string from_domain = extract_from_header_domain(raw_headers);
        std::string mf_domain = extract_domain(mail_from);
        result.dmarc = check_dmarc(from_domain, result.spf, result.dkim, mf_domain);
        result.dmarc.header_from_domain = from_domain;
    } else {
        result.dmarc.result = "none";
        result.dmarc.reason = "DMARC check disabled";
    }
}

// ========== SPF ==========

std::string InboundVerifier::extract_domain(const std::string& addr) {
    auto at = addr.find('@');
    if (at == std::string::npos) return {};
    std::string domain = addr.substr(at + 1);
    // Remove angle brackets if present
    if (!domain.empty() && domain.back() == '>') domain.pop_back();
    if (!domain.empty() && domain.front() == '<') domain = domain.substr(1);
    return to_lower(domain);
}

std::string InboundVerifier::extract_from_header_domain(const std::string& headers) {
    auto vals = get_header_values(headers, "from");
    if (vals.empty()) return {};
    // Extract domain from the From value: "user@domain" or "Name <user@domain>"
    const auto& from = vals[0];
    auto begin = from.find('<');
    auto end = from.find('>');
    std::string addr;
    if (begin != std::string::npos && end != std::string::npos) {
        addr = from.substr(begin + 1, end - begin - 1);
    } else {
        addr = from;
    }
    auto at = addr.find('@');
    if (at == std::string::npos) return {};
    return to_lower(addr.substr(at + 1));
}

std::vector<InboundVerifier::SpfMechanism>
InboundVerifier::parse_spf_record(const std::string& record) {
    std::vector<SpfMechanism> mechanisms;
    if (record.substr(0, 6) != "v=spf1") return mechanisms;

    std::string content = record.substr(6);
    std::vector<std::string> parts;
    split(content, ' ', parts);

    for (const auto& part : parts) {
        if (part.empty()) continue;
        SpfMechanism mech;
        std::string token = part;

        // Check qualifier
        char first = token[0];
        if (first == '+' || first == '-' || first == '~' || first == '?') {
            mech.qualifier = first;
            token = token.substr(1);
        } else {
            mech.qualifier = "+";
        }

        // Split mechanism and value
        auto colon = token.find(':');
        if (colon != std::string::npos) {
            mech.mechanism = to_lower(token.substr(0, colon));
            std::string val = token.substr(colon + 1);

            // Handle CIDR suffix
            auto slash = val.find('/');
            if (slash != std::string::npos) {
                mech.value = val.substr(0, slash);
                mech.cidr = val.substr(slash + 1);
            } else {
                mech.value = val;
            }
        } else {
            // Check for = (modifier without colon)
            auto eq = token.find('=');
            if (eq != std::string::npos) {
                mech.mechanism = to_lower(token.substr(0, eq));
                mech.value = token.substr(eq + 1);
            } else {
                // Check for CIDR on mechanism only (e.g., "ip4:1.2.3.0/24" or "all")
                mech.mechanism = to_lower(token);
            }
        }

        mechanisms.push_back(std::move(mech));
    }
    return mechanisms;
}

// Simple IPv4 match with optional CIDR
bool ip4_match(const std::string& client_ip, const std::string& network,
               const std::string& cidr_str) {
    // Very basic: exact match or prefix match
    if (cidr_str.empty()) {
        return client_ip == network;
    }
    int prefix = std::stoi(cidr_str);
    // Convert both to 32-bit integers for comparison
    auto ip_to_u32 = [](const std::string& ip) -> uint32_t {
        uint32_t result = 0;
        std::stringstream ss(ip);
        std::string octet;
        int shift = 24;
        while (std::getline(ss, octet, '.') && shift >= 0) {
            result |= (static_cast<uint32_t>(std::stoi(octet)) << shift);
            shift -= 8;
        }
        return result;
    };
    uint32_t client = ip_to_u32(client_ip);
    uint32_t net = ip_to_u32(network);
    uint32_t mask = (prefix == 0) ? 0 : (~0u << (32 - prefix));
    return (client & mask) == (net & mask);
}

std::string InboundVerifier::eval_spf_mechanism(const SpfMechanism& mech,
                                                const std::string& client_ip,
                                                const std::string& domain,
                                                int depth) {
    const auto& m = mech.mechanism;

    if (m == "ip4") {
        return ip4_match(client_ip, mech.value, mech.cidr) ? "match" : "no_match";
    }

    if (m == "ip6") {
        // Simplified: just exact match for now
        return (client_ip == mech.value) ? "match" : "no_match";
    }

    if (m == "a") {
        std::string target = mech.value.empty() ? domain : mech.value;
        auto addrs = outbound::SyncDnsWrapper(dns_).resolve_host_addresses(target);
        for (const auto& a : addrs) {
            if (a == client_ip) return "match";
        }
        return "no_match";
    }

    if (m == "mx") {
        std::string target = mech.value.empty() ? domain : mech.value;
        auto mx_records = outbound::SyncDnsWrapper(dns_).resolve_mx(target);
        for (const auto& mx : mx_records) {
            auto addrs = outbound::SyncDnsWrapper(dns_).resolve_host_addresses(mx.host);
            for (const auto& a : addrs) {
                if (a == client_ip) return "match";
            }
        }
        return "no_match";
    }

    if (m == "include") {
        if (depth >= 10) return "permerror"; // RFC 7208: max 10 include levels
        std::string target = mech.value;
        auto txt_records = outbound::SyncDnsWrapper(dns_).resolve_txt(target);
        std::string spf_record;
        for (const auto& rec : txt_records) {
            if (rec.find("v=spf1") == 0) {
                spf_record = rec;
                break;
            }
        }
        if (spf_record.empty()) return "temperror";

        // Evaluate the included SPF record
        auto sub_mechs = parse_spf_record(spf_record);
        for (const auto& sub : sub_mechs) {
            if (sub.mechanism == "all" || sub.mechanism == "redirect") {
                // Will be handled; just evaluate inline
            }
            std::string sub_result = eval_spf_mechanism(sub, client_ip, target, depth + 1);
            if (sub_result == "match") return "match";
        }
        return "no_match";
    }

    if (m == "redirect") {
        std::string target = mech.value;
        auto txt_records = outbound::SyncDnsWrapper(dns_).resolve_txt(target);
        std::string spf_record;
        for (const auto& rec : txt_records) {
            if (rec.find("v=spf1") == 0) {
                spf_record = rec;
                break;
            }
        }
        if (spf_record.empty()) return "temperror";
        auto sub_mechs = parse_spf_record(spf_record);
        for (const auto& sub : sub_mechs) {
            std::string sub_result = eval_spf_mechanism(sub, client_ip, target, depth + 1);
            if (sub_result == "match") return sub.mechanism == "all" ? "match" : "match";
        }
        return "no_match";
    }

    if (m == "all") {
        return "match";
    }

    if (m == "exp" || m == "redirect" || m == "ptr") {
        return "no_match"; // not fully supported, silently skip
    }

    return "no_match";
}

SpfResult InboundVerifier::check_spf(const std::string& client_ip,
                                     const std::string& mail_from,
                                     const std::string& helo_domain,
                                     int /*depth*/) {
    SpfResult result;

    // Determine domain to check
    std::string domain;
    if (mail_from.empty() || mail_from == "<>") {
        domain = helo_domain;
    } else {
        domain = extract_domain(mail_from);
    }
    if (domain.empty()) {
        result.result = "none";
        result.reason = "no valid domain for SPF check";
        return result;
    }

    // Query TXT records（同步方法用 SyncDnsWrapper 临时包装）
    auto txt_records = outbound::SyncDnsWrapper(dns_).resolve_txt(domain);
    std::string spf_record;
    for (const auto& rec : txt_records) {
        if (rec.find("v=spf1") == 0) {
            if (!spf_record.empty()) {
                // Multiple SPF records → permerror
                result.result = "permerror";
                result.reason = "multiple SPF records for " + domain;
                return result;
            }
            spf_record = rec;
        }
    }

    if (spf_record.empty()) {
        result.result = "none";
        result.reason = "no SPF record for " + domain;
        return result;
    }

    // Parse and evaluate
    auto mechanisms = parse_spf_record(spf_record);
    for (const auto& mech : mechanisms) {
        if (mech.mechanism == "exp") continue; // skip explanation modifier

        std::string eval = eval_spf_mechanism(mech, client_ip, domain, 0);

        if (eval == "match") {
            // Map qualifier to result
            if (mech.qualifier == "+") { result.result = "pass"; }
            else if (mech.qualifier == "-") { result.result = "fail"; result.reason = "SPF hard fail"; }
            else if (mech.qualifier == "~") { result.result = "softfail"; result.reason = "SPF soft fail"; }
            else if (mech.qualifier == "?") { result.result = "neutral"; }
            return result;
        }
        if (eval == "temperror") {
            result.result = "temperror";
            result.reason = "SPF temporary error evaluating " + mech.mechanism;
            return result;
        }
        if (eval == "permerror") {
            result.result = "permerror";
            result.reason = "SPF permanent error evaluating " + mech.mechanism;
            return result;
        }
    }

    // No mechanism matched
    result.result = "neutral";
    result.reason = "no SPF mechanism matched";
    return result;
}

// ---- 异步 SPF（CPS：DNS 查询通过回调续传，回调在 c-ares 线程触发）----

// 共享 DNS 记录缓存（跨 InboundVerifier 实例，供不同邮件的校验复用）
// 复用通用线程安全 LruCache（shared_mutex+mutex 双锁 + LRU 淘汰），value 携带自定义 TTL
// （SPF/DMARC/DKIM/负缓存 TTL 不同，LruCache 的 m_ttl 仅作 fallback）
struct DnsCacheValue {
    std::vector<std::string> records;
    std::chrono::steady_clock::time_point expires;
};
static LruCache<std::string, DnsCacheValue>& inbound_dns_cache() {
    static constexpr size_t kCacheCapacity = 4096;
    static LruCache<std::string, DnsCacheValue> cache(kCacheCapacity, std::chrono::seconds(300));
    return cache;
}

void InboundVerifier::clear_dns_cache() {
    inbound_dns_cache().clear();
}

void InboundVerifier::get_txt_async(const std::string& key, std::chrono::seconds ttl,
                                    std::function<void(std::vector<std::string>)> cb) {
    bool stale = false;
    DnsCacheValue v;
    if (inbound_dns_cache().get(key, v, stale) &&
        std::chrono::steady_clock::now() < v.expires) {
        cb(v.records);   // 命中且未过期
        return;
    }
    dns_.async_resolve_txt(key,
        [key, ttl, cb = std::move(cb)](std::vector<std::string> records) mutable {
            auto exp = std::chrono::steady_clock::now() + (records.empty() ? kNegativeTtl : ttl);
            inbound_dns_cache().put(key, DnsCacheValue{records, exp});
            cb(std::move(records));
        });
}

void InboundVerifier::check_spf_async(const std::string& client_ip,
                                      const std::string& mail_from,
                                      const std::string& helo_domain,
                                      std::function<void(SpfResult)> cb) {
    std::string domain;
    if (mail_from.empty() || mail_from == "<>") {
        domain = helo_domain;
    } else {
        domain = extract_domain(mail_from);
    }
    if (domain.empty()) {
        cb({"none", "no valid domain for SPF check"});
        return;
    }

    get_txt_async(domain, kSpfTtl,
        [this, client_ip, domain, cb = std::move(cb)](std::vector<std::string> txt_records) mutable {
            SpfResult result;
            std::string spf_record;
            for (const auto& rec : txt_records) {
                if (rec.find("v=spf1") == 0) {
                    if (!spf_record.empty()) {
                        cb({"permerror", "multiple SPF records for " + domain});
                        return;
                    }
                    spf_record = rec;
                }
            }
            if (spf_record.empty()) {
                cb({"none", "no SPF record for " + domain});
                return;
            }
            auto mechanisms = parse_spf_record(spf_record);
            eval_spf_mechanisms_async(mechanisms, 0, client_ip, domain, 0,
                [cb = std::move(cb)](std::string eval) mutable {
                    SpfResult r;
                    if (eval == "pass")       { r.result = "pass"; }
                    else if (eval == "fail")  { r.result = "fail"; r.reason = "SPF hard fail"; }
                    else if (eval == "softfail") { r.result = "softfail"; r.reason = "SPF soft fail"; }
                    else if (eval == "neutral")  { r.result = "neutral"; r.reason = "no SPF mechanism matched"; }
                    else if (eval == "temperror"){ r.result = "temperror"; r.reason = "SPF temporary error"; }
                    else if (eval == "permerror"){ r.result = "permerror"; r.reason = "SPF permanent error"; }
                    else                         { r.result = "neutral"; r.reason = "no SPF mechanism matched"; }
                    cb(std::move(r));
                });
        });
}

void InboundVerifier::eval_spf_mechanisms_async(
    const std::vector<SpfMechanism>& mechs, size_t idx,
    const std::string& client_ip, const std::string& domain, int depth,
    std::function<void(std::string)> cb)
{
    if (idx >= mechs.size()) { cb("neutral"); return; }
    const auto& mech = mechs[idx];

    // 单个机制评估（本地机制同步返回；DNS 机制走异步回调）
    // 捕获 this：递归续传 eval_spf_mechanisms_async（verifier 由 check_spf_async 的 shared_ptr 持有）
    auto continue_after = [this, mechs, idx, client_ip, domain, depth,
                           cb = std::move(cb)](std::string eval) mutable {
        if (eval == "match") {
            if (mechs[idx].qualifier == "+") cb("pass");
            else if (mechs[idx].qualifier == "-") cb("fail");
            else if (mechs[idx].qualifier == "~") cb("softfail");
            else cb("neutral");
            return;
        }
        if (eval == "temperror") { cb("temperror"); return; }
        if (eval == "permerror") { cb("permerror"); return; }
        // no_match → 评估下一个机制
        this->eval_spf_mechanisms_async(
            mechs, idx + 1, client_ip, domain, depth, std::move(cb));
    };

    const auto& m = mech.mechanism;
    if (m == "ip4") {
        continue_after(ip4_match(client_ip, mech.value, mech.cidr) ? "match" : "no_match");
        return;
    }
    if (m == "ip6") {
        continue_after((client_ip == mech.value) ? "match" : "no_match");
        return;
    }
    if (m == "a") {
        std::string target = mech.value.empty() ? domain : mech.value;
        dns_.async_resolve_host(target,
            [client_ip, continue_after = std::move(continue_after)](std::vector<std::string> addrs) mutable {
                for (const auto& a : addrs) if (a == client_ip) { continue_after("match"); return; }
                continue_after("no_match");
            });
        return;
    }
    if (m == "mx") {
        std::string target = mech.value.empty() ? domain : mech.value;
        dns_.async_resolve_mx(target,
            [this, client_ip, continue_after = std::move(continue_after)](std::vector<outbound::MxRecord> mxs) mutable {
                // 用 shared_ptr 持有 continue_after 与递归步进器，避免多级 move 陷阱
                auto cont = std::make_shared<std::function<void(std::string)>>(std::move(continue_after));
                auto addrs = std::make_shared<std::vector<std::string>>();
                auto mxs_s = std::make_shared<std::vector<outbound::MxRecord>>(std::move(mxs));
                auto check_one = std::make_shared<std::function<void(size_t)>>();
                *check_one = [this, client_ip, addrs, mxs_s, cont, check_one](size_t i) mutable {
                    if (i >= mxs_s->size()) { (*cont)("no_match"); return; }
                    dns_.async_resolve_host((*mxs_s)[i].host,
                        [client_ip, addrs, mxs_s, i, cont, check_one](std::vector<std::string> h) mutable {
                            addrs->insert(addrs->end(), h.begin(), h.end());
                            for (const auto& a : *addrs) if (a == client_ip) { (*cont)("match"); return; }
                            (*check_one)(i + 1);   // 继续下一个 MX
                        });
                };
                (*check_one)(0);
            });
        return;
    }
    if (m == "include") {
        if (depth >= 10) { continue_after("permerror"); return; }
        std::string target = mech.value;
        get_txt_async(target, kSpfTtl,
            [this, client_ip, target, depth, continue_after = std::move(continue_after)](std::vector<std::string> txt_records) mutable {
                std::string spf_record;
                for (const auto& rec : txt_records) {
                    if (rec.find("v=spf1") == 0) { spf_record = rec; break; }
                }
                if (spf_record.empty()) { continue_after("temperror"); return; }
                auto sub_mechs = parse_spf_record(spf_record);
                eval_spf_mechanisms_async(sub_mechs, 0, client_ip, target, depth + 1,
                    [continue_after = std::move(continue_after)](std::string sub_eval) mutable {
                        // include 命中条件：子 SPF 为 pass；fail/softfail/neutral 视为不匹配继续外层
                        if (sub_eval == "pass") continue_after("match");
                        else if (sub_eval == "temperror" || sub_eval == "permerror") continue_after(sub_eval);
                        else continue_after("no_match");
                    });
            });
        return;
    }
    if (m == "redirect") {
        std::string target = mech.value;
        get_txt_async(target, kSpfTtl,
            [this, client_ip, target, depth, continue_after = std::move(continue_after)](std::vector<std::string> txt_records) mutable {
                std::string spf_record;
                for (const auto& rec : txt_records) {
                    if (rec.find("v=spf1") == 0) { spf_record = rec; break; }
                }
                if (spf_record.empty()) { continue_after("temperror"); return; }
                auto sub_mechs = parse_spf_record(spf_record);
                // redirect 结果直接作为最终结果（透传子 SPF 评估结果）
                eval_spf_mechanisms_async(sub_mechs, 0, client_ip, target, depth + 1,
                    [continue_after = std::move(continue_after)](std::string sub_eval) mutable {
                        continue_after(sub_eval);
                    });
            });
        return;
    }
    if (m == "all") {
        continue_after("match");
        return;
    }
    // exp/ptr 等未支持机制 → 不匹配
    continue_after("no_match");
}

// ========== DKIM ==========

std::vector<InboundVerifier::DkimSignature>
InboundVerifier::parse_dkim_signatures(const std::string& raw_headers) {
    std::vector<DkimSignature> sigs;
    auto values = get_header_values(raw_headers, "dkim-signature");

    for (const auto& val : values) {
        auto tags = parse_tags(val);
        DkimSignature sig;
        sig.raw_value = val;

        auto it_v = tags.find("v");
        sig.version = (it_v != tags.end()) ? it_v->second : "";
        auto it_a = tags.find("a");
        sig.algorithm = (it_a != tags.end()) ? to_lower(it_a->second) : "";
        auto it_d = tags.find("d");
        sig.domain = (it_d != tags.end()) ? to_lower(it_d->second) : "";
        auto it_s = tags.find("s");
        sig.selector = (it_s != tags.end()) ? it_s->second : "";
        auto it_bh = tags.find("bh");
        sig.body_hash = (it_bh != tags.end()) ? it_bh->second : "";
        auto it_b = tags.find("b");
        sig.signature = (it_b != tags.end()) ? it_b->second : "";

        auto it_h = tags.find("h");
        if (it_h != tags.end()) {
            split(it_h->second, ':', sig.signed_headers);
        }

        // 解析 c= 标签（规范化算法）: "simple/simple", "relaxed/relaxed" 等
        auto it_c = tags.find("c");
        if (it_c != tags.end()) {
            auto slash = it_c->second.find('/');
            if (slash != std::string::npos) {
                sig.header_canon = to_lower(outbound::trim_ascii_ws(it_c->second.substr(0, slash)));
                sig.body_canon   = to_lower(outbound::trim_ascii_ws(it_c->second.substr(slash + 1)));
            } else {
                sig.header_canon = sig.body_canon = to_lower(outbound::trim_ascii_ws(it_c->second));
            }
        }

        if (!sig.domain.empty() && !sig.selector.empty() && !sig.signature.empty()) {
            sigs.push_back(std::move(sig));
        }
    }
    return sigs;
}

bool InboundVerifier::verify_dkim_signature(const DkimSignature& sig,
                                            const std::string& raw_headers,
                                            const std::string& raw_body,
                                            std::string& error_out) {
    std::string canonical_body = (sig.body_canon == "relaxed")
        ? outbound::normalize_body_relaxed(raw_body)
        : outbound::normalize_body_simple(raw_body);
    std::string computed_bh = outbound::sha256_base64(canonical_body);
    return verify_dkim_signature_impl(sig, raw_headers, computed_bh, error_out);
}

bool InboundVerifier::verify_dkim_signature_from_file(const DkimSignature& sig,
                                                      const std::string& raw_headers,
                                                      const std::string& body_path,
                                                      std::string& error_out) {
    std::ifstream body_file(body_path, std::ios::binary);
    if (!body_file.is_open()) {
        error_out = "cannot open DKIM body file: " + body_path;
        LOG_INBOUND_WARN("DKIM: cannot open body file {}", body_path);
        return false;
    }
    std::string computed_bh;
    if (!outbound::dkim_body_hash_stream(body_file, sig.body_canon, computed_bh)) {
        error_out = "streaming DKIM body hash failed";
        return false;
    }
    return verify_dkim_signature_impl(sig, raw_headers, computed_bh, error_out);
}

bool InboundVerifier::verify_dkim_signature_impl(const DkimSignature& sig,
                                                 const std::string& raw_headers,
                                                 const std::string& computed_bh,
                                                 std::string& error_out) {
    try {
    // 1. Fetch public key via DNS
    std::string key_domain = sig.selector + "._domainkey." + sig.domain;
    auto txt_records = outbound::SyncDnsWrapper(dns_).resolve_txt(key_domain);

    std::string pubkey_b64;
    for (const auto& rec : txt_records) {
        LOG_INBOUND_DEBUG("DKIM TXT record ({} bytes): {}", rec.size(), rec);
        // DKIM key record looks like: "k=rsa; p=MIGfMA0..."
        auto tags = parse_tags(rec);
        auto it_k = tags.find("k");
        std::string ktype = (it_k != tags.end()) ? to_lower(it_k->second) : "rsa";
        if (ktype != "rsa") continue;
        auto it_p = tags.find("p");
        if (it_p != tags.end() && !it_p->second.empty()) {
            pubkey_b64 = it_p->second;
            LOG_INBOUND_DEBUG("DKIM p= value ({} bytes, mod4={})",
                            pubkey_b64.size(), pubkey_b64.size() % 4);
            break;
        }
    }

    if (pubkey_b64.empty()) {
        error_out = "no DKIM public key found in DNS for " + key_domain;
        return false;
    }

    // 2. Verify body hash（computed_bh 由调用方计算：字符串版或流式文件版）
    LOG_INBOUND_DEBUG("DKIM body hash: expected={}.. computed={}.. canon={}",
                     sig.body_hash.substr(0, 8), computed_bh.substr(0, 8),
                     sig.body_canon);
    if (computed_bh != sig.body_hash) {
        error_out = "DKIM body hash mismatch (canon=" + sig.body_canon + ")";
        LOG_INBOUND_WARN("DKIM bh mismatch: domain={}, selector={}, canon={}, exp={}, got={}",
                         sig.domain, sig.selector, sig.body_canon,
                         sig.body_hash.substr(0, 16), computed_bh.substr(0, 16));
        return false;
    }

    // 3. Build signing input
    LOG_INBOUND_DEBUG("DKIM step3: building signing input, headers={} h_count={}",
                    raw_headers.size(), sig.signed_headers.size());
    // Parse raw headers into a map for canonicalization
    auto header_map = parse_headers_map(raw_headers);

    std::string signing_input;
    for (const auto& hname : sig.signed_headers) {
        auto it = header_map.find(to_lower(hname));
        if (it == header_map.end()) {
            continue; // Header not present in message — skip (DKIM allows this)
        }
        auto orig_vals = get_header_values(raw_headers, to_lower(hname));
        if (orig_vals.empty()) continue;
        signing_input += outbound::canonicalize_header_relaxed(hname, orig_vals[0]);
    }

    // Add DKIM-Signature header (h= list was signed before b= was set)
    // We need to remove the b= tag from the raw value for canonicalization
    std::string dkim_for_signing = sig.raw_value;
    auto b_pos = dkim_for_signing.find("b=");
    if (b_pos != std::string::npos) {
        // Find end of b= value (next semicolon or end of string)
        auto end_pos = dkim_for_signing.find(';', b_pos);
        if (end_pos != std::string::npos) {
            dkim_for_signing = dkim_for_signing.substr(0, b_pos + 2) +
                               dkim_for_signing.substr(end_pos);
        } else {
            dkim_for_signing = dkim_for_signing.substr(0, b_pos + 2);
        }
    }
    // DKIM-Signature is always the last header — it MUST NOT have a trailing CRLF
    // (the signing process computes the hash WITHOUT the trailing CRLF of this header)
    std::string dkim_canon = outbound::canonicalize_header_relaxed("DKIM-Signature", dkim_for_signing);
    if (dkim_canon.size() >= 2 && dkim_canon.substr(dkim_canon.size() - 2) == "\r\n") {
        dkim_canon.resize(dkim_canon.size() - 2);
    }
    signing_input += dkim_canon;

    // 4. Verify RSA-SHA256 signature
    LOG_INBOUND_DEBUG("DKIM step4: decoding key, key_b64_len={}", pubkey_b64.size());
    // Decode base64 public key → DER → EVP_PKEY
    std::string clean_key;
    clean_key.reserve(pubkey_b64.size());
    for (char ch : pubkey_b64) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            clean_key += ch;
    }

    // Add missing base64 padding (DNS TXT records often strip trailing =)
    int missing_pad = (4 - (clean_key.size() % 4)) % 4;
    clean_key.append(missing_pad, '=');

    int pubkey_len = static_cast<int>(clean_key.size());
    std::vector<unsigned char> pubkey_decoded(pubkey_len);
    int decoded = EVP_DecodeBlock(pubkey_decoded.data(),
                                  reinterpret_cast<const unsigned char*>(clean_key.data()),
                                  pubkey_len);
    if (decoded <= 0) {
        error_out = "failed to base64-decode DKIM public key (len="
                  + std::to_string(pubkey_len) + ", orig="
                  + std::to_string(pubkey_len - missing_pad) + ")";
        LOG_INBOUND_WARN("DKIM base64 decode failed: domain={}, selector={}, key_len={}",
                         sig.domain, sig.selector, pubkey_len - missing_pad);
        return false;
    }
    // OpenSSL EVP_DecodeBlock includes padding bytes in output, adjust
    if (pubkey_len > 0 && clean_key.back() == '=') decoded--;
    if (pubkey_len > 1 && clean_key[pubkey_len - 2] == '=') decoded--;
    pubkey_decoded.resize(static_cast<size_t>(decoded));

    LOG_INBOUND_DEBUG("DKIM key decoded={} bytes, calling d2i_PUBKEY...", decoded);
    Logger::get_instance().flush();
    const unsigned char* key_ptr = pubkey_decoded.data();
    EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &key_ptr, static_cast<long>(decoded));
    LOG_INBOUND_DEBUG("DKIM d2i_PUBKEY returned pkey={}", (void*)pkey);
    Logger::get_instance().flush();
    if (!pkey) {
        error_out = "failed to parse DKIM public key (DER, decoded_len="
                  + std::to_string(decoded) + ")";
        return false;
    }

    // Decode base64 signature (strip whitespace from folded header lines)
    LOG_INBOUND_DEBUG("DKIM step5: decode sig, sig_b64_raw={}", sig.signature.size());
    std::string clean_sig;
    clean_sig.reserve(sig.signature.size());
    for (char ch : sig.signature) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            clean_sig += ch;
    }
    int missing_sig_pad = (4 - (clean_sig.size() % 4)) % 4;
    clean_sig.append(missing_sig_pad, '=');

    int sig_b64_len = static_cast<int>(clean_sig.size());
    std::vector<unsigned char> sig_decoded(sig_b64_len);
    int sig_decoded_len = EVP_DecodeBlock(sig_decoded.data(),
                                          reinterpret_cast<const unsigned char*>(clean_sig.data()),
                                          sig_b64_len);
    if (sig_decoded_len <= 0) {
        EVP_PKEY_free(pkey);
        error_out = "failed to base64-decode DKIM signature (sig_len="
                  + std::to_string(sig_b64_len) + ", err=" + std::to_string(sig_decoded_len) + ")";
        LOG_INBOUND_WARN("DKIM sig decode failed: domain={}, selector={}, sig_len={}, ret={}",
                        sig.domain, sig.selector, sig_b64_len, sig_decoded_len);
        return false;
    }
    if (sig_b64_len > 0 && clean_sig.back() == '=') sig_decoded_len--;
    if (sig_b64_len > 1 && clean_sig[clean_sig.size() - 2] == '=') sig_decoded_len--;
    LOG_INBOUND_DEBUG("DKIM sig decoded: raw={} adjusted={}", sig_b64_len > 0 ? std::to_string(sig_b64_len) : "0", sig_decoded_len);

    // Verify
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    bool ok = false;
    do {
        if (EVP_DigestVerifyInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
            error_out = "EVP_DigestVerifyInit failed";
            break;
        }
        if (EVP_DigestVerifyUpdate(md_ctx, signing_input.data(), signing_input.size()) <= 0) {
            error_out = "EVP_DigestVerifyUpdate failed";
            break;
        }
        int verify_result = EVP_DigestVerifyFinal(md_ctx, sig_decoded.data(),
                                                   static_cast<size_t>(sig_decoded_len));
        if (verify_result == 1) {
            ok = true;
            LOG_INBOUND_INFO("DKIM VERIFY PASS: domain={}, selector={}", sig.domain, sig.selector);
            Logger::get_instance().flush();
        } else {
            error_out = "DKIM signature verification failed";
            LOG_INBOUND_WARN("DKIM VERIFY FAIL: domain={}, selector={}, sig_len={}",
                            sig.domain, sig.selector, sig_decoded_len);
            Logger::get_instance().flush();
        }
    } while (false);

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;

    } catch (const std::exception& e) {
        error_out = std::string("DKIM verify exception: ") + e.what();
        LOG_INBOUND_WARN("DKIM EXCEPTION: domain={}, selector={}, what={}",
                        sig.domain, sig.selector, e.what());
        Logger::get_instance().flush();
        return false;
    } catch (...) {
        error_out = "DKIM verify unknown exception";
        LOG_INBOUND_WARN("DKIM UNKNOWN EXCEPTION: domain={}, selector={}",
                        sig.domain, sig.selector);
        Logger::get_instance().flush();
        return false;
    }
}

// 验签核心（pubkey 已从 DNS 获取）—— 供异步 DKIM 复用，避免复制 DNS 代码路径
bool InboundVerifier::verify_dkim_with_pubkey(const DkimSignature& sig,
                                              const std::string& raw_headers,
                                              const std::string& computed_bh,
                                              const std::string& pubkey_b64,
                                              std::string& error_out) {
    try {
    // body hash 校验（computed_bh 由调用方计算：字符串版或流式文件版）
    LOG_INBOUND_DEBUG("DKIM body hash: expected={}.. computed={}.. canon={}",
                     sig.body_hash.substr(0, 8), computed_bh.substr(0, 8),
                     sig.body_canon);
    if (computed_bh != sig.body_hash) {
        error_out = "DKIM body hash mismatch (canon=" + sig.body_canon + ")";
        LOG_INBOUND_WARN("DKIM bh mismatch: domain={}, selector={}, canon={}, exp={}, got={}",
                         sig.domain, sig.selector, sig.body_canon,
                         sig.body_hash.substr(0, 16), computed_bh.substr(0, 16));
        return false;
    }

    // Build signing input
    LOG_INBOUND_DEBUG("DKIM step3: building signing input, headers={} h_count={}",
                    raw_headers.size(), sig.signed_headers.size());
    auto header_map = parse_headers_map(raw_headers);
    std::string signing_input;
    for (const auto& hname : sig.signed_headers) {
        auto it = header_map.find(to_lower(hname));
        if (it == header_map.end()) continue;  // Header not present — skip (DKIM allows)
        auto orig_vals = get_header_values(raw_headers, to_lower(hname));
        if (orig_vals.empty()) continue;
        signing_input += outbound::canonicalize_header_relaxed(hname, orig_vals[0]);
    }
    // DKIM-Signature 是最后一个头，b= 置空后再规范化（去掉尾部 CRLF）
    std::string dkim_for_signing = sig.raw_value;
    auto b_pos = dkim_for_signing.find("b=");
    if (b_pos != std::string::npos) {
        auto end_pos = dkim_for_signing.find(';', b_pos);
        if (end_pos != std::string::npos) {
            dkim_for_signing = dkim_for_signing.substr(0, b_pos + 2) +
                               dkim_for_signing.substr(end_pos);
        } else {
            dkim_for_signing = dkim_for_signing.substr(0, b_pos + 2);
        }
    }
    std::string dkim_canon = outbound::canonicalize_header_relaxed("DKIM-Signature", dkim_for_signing);
    if (dkim_canon.size() >= 2 && dkim_canon.substr(dkim_canon.size() - 2) == "\r\n") {
        dkim_canon.resize(dkim_canon.size() - 2);
    }
    signing_input += dkim_canon;

    // RSA-SHA256 验签
    LOG_INBOUND_DEBUG("DKIM step4: decoding key, key_b64_len={}", pubkey_b64.size());
    std::string clean_key;
    clean_key.reserve(pubkey_b64.size());
    for (char ch : pubkey_b64) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') clean_key += ch;
    }
    int missing_pad = (4 - (clean_key.size() % 4)) % 4;
    clean_key.append(missing_pad, '=');
    int pubkey_len = static_cast<int>(clean_key.size());
    std::vector<unsigned char> pubkey_decoded(pubkey_len);
    int decoded = EVP_DecodeBlock(pubkey_decoded.data(),
                                  reinterpret_cast<const unsigned char*>(clean_key.data()),
                                  pubkey_len);
    if (decoded <= 0) {
        error_out = "failed to base64-decode DKIM public key (len="
                  + std::to_string(pubkey_len) + ")";
        return false;
    }
    if (pubkey_len > 0 && clean_key.back() == '=') decoded--;
    if (pubkey_len > 1 && clean_key[pubkey_len - 2] == '=') decoded--;
    pubkey_decoded.resize(static_cast<size_t>(decoded));
    const unsigned char* key_ptr = pubkey_decoded.data();
    EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &key_ptr, static_cast<long>(decoded));
    if (!pkey) {
        error_out = "failed to parse DKIM public key (DER, decoded_len="
                  + std::to_string(decoded) + ")";
        return false;
    }

    std::string clean_sig;
    clean_sig.reserve(sig.signature.size());
    for (char ch : sig.signature) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') clean_sig += ch;
    }
    int missing_sig_pad = (4 - (clean_sig.size() % 4)) % 4;
    clean_sig.append(missing_sig_pad, '=');
    int sig_b64_len = static_cast<int>(clean_sig.size());
    std::vector<unsigned char> sig_decoded(sig_b64_len);
    int sig_decoded_len = EVP_DecodeBlock(sig_decoded.data(),
                                          reinterpret_cast<const unsigned char*>(clean_sig.data()),
                                          sig_b64_len);
    if (sig_decoded_len <= 0) {
        EVP_PKEY_free(pkey);
        error_out = "failed to base64-decode DKIM signature (sig_len="
                  + std::to_string(sig_b64_len) + ")";
        return false;
    }
    if (sig_b64_len > 0 && clean_sig.back() == '=') sig_decoded_len--;
    if (sig_b64_len > 1 && clean_sig[clean_sig.size() - 2] == '=') sig_decoded_len--;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    bool ok = false;
    do {
        if (EVP_DigestVerifyInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
            error_out = "EVP_DigestVerifyInit failed";
            break;
        }
        if (EVP_DigestVerifyUpdate(md_ctx, signing_input.data(), signing_input.size()) <= 0) {
            error_out = "EVP_DigestVerifyUpdate failed";
            break;
        }
        int verify_result = EVP_DigestVerifyFinal(md_ctx, sig_decoded.data(),
                                                   static_cast<size_t>(sig_decoded_len));
        if (verify_result == 1) {
            ok = true;
            LOG_INBOUND_INFO("DKIM VERIFY PASS: domain={}, selector={}", sig.domain, sig.selector);
            Logger::get_instance().flush();
        } else {
            error_out = "DKIM signature verification failed";
            LOG_INBOUND_WARN("DKIM VERIFY FAIL: domain={}, selector={}, sig_len={}",
                            sig.domain, sig.selector, sig_decoded_len);
            Logger::get_instance().flush();
        }
    } while (false);
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
    } catch (const std::exception& e) {
        error_out = std::string("DKIM verify exception: ") + e.what();
        return false;
    } catch (...) {
        error_out = "DKIM verify unknown exception";
        return false;
    }
}

// 从 TXT 记录提取 rsa DKIM 公钥（p= 值）
static std::string extract_dkim_pubkey(const std::vector<std::string>& txt_records) {
    for (const auto& rec : txt_records) {
        auto tags = parse_tags(rec);
        auto it_k = tags.find("k");
        std::string ktype = (it_k != tags.end()) ? to_lower(it_k->second) : "rsa";
        if (ktype != "rsa") continue;
        auto it_p = tags.find("p");
        if (it_p != tags.end() && !it_p->second.empty()) return it_p->second;
    }
    return {};
}

// ---- 异步 DKIM（CPS：body hash 本地计算，仅 key DNS 查询异步）----

void InboundVerifier::check_dkim_async(const std::string& raw_headers,
                                       const std::string& raw_body,
                                       std::function<void(DkimResult)> cb) {
    DkimResult result;
    result.result = "none";
    auto sigs = parse_dkim_signatures(raw_headers);
    if (sigs.empty()) { result.reason = "no DKIM-Signature header found"; cb(result); return; }

    auto try_sig = std::make_shared<std::function<void(size_t)>>();
    *try_sig = [this, sigs, raw_headers, raw_body, result,
                try_sig, cb = std::move(cb)](size_t idx) mutable {
        if (idx >= sigs.size()) {
            if (result.result == "none") result.result = "fail";
            if (result.reason.empty()) result.reason = "no valid DKIM signature";
            cb(std::move(result));
            *try_sig = nullptr;   // 打破自引用环
            return;
        }
        const auto& sig = sigs[idx];
        if (sig.algorithm != "rsa-sha256") { (*try_sig)(idx + 1); return; }

        std::string canonical_body = (sig.body_canon == "relaxed")
            ? outbound::normalize_body_relaxed(raw_body)
            : outbound::normalize_body_simple(raw_body);
        std::string computed_bh = outbound::sha256_base64(canonical_body);

        std::string key_domain = sig.selector + "._domainkey." + sig.domain;
        get_txt_async(key_domain, kDkimTtl,
            [this, sig, raw_headers, computed_bh, key_domain, idx, result,
             cb, try_sig](std::vector<std::string> txt_records) mutable {
                std::string pubkey_b64 = extract_dkim_pubkey(txt_records);
                if (pubkey_b64.empty()) {
                    if (result.reason.empty()) result.reason = "no DKIM public key found in DNS for " + key_domain;
                    (*try_sig)(idx + 1);
                    return;
                }
                std::string error;
                if (verify_dkim_with_pubkey(sig, raw_headers, computed_bh, pubkey_b64, error)) {
                    result.result = "pass";
                    result.reason = "";
                    result.selector = sig.selector;
                    result.signing_domain = sig.domain;
                    cb(std::move(result));
                    *try_sig = nullptr;
                    return;
                }
                if (result.reason.empty()) result.reason = error;
                (*try_sig)(idx + 1);
            });
    };
    (*try_sig)(0);
}

void InboundVerifier::check_dkim_from_file_async(const std::string& raw_headers,
                                                 const std::string& body_path,
                                                 std::function<void(DkimResult)> cb) {
    DkimResult result;
    result.result = "none";
    auto sigs = parse_dkim_signatures(raw_headers);
    if (sigs.empty()) { result.reason = "no DKIM-Signature header found"; cb(result); return; }

    auto try_sig = std::make_shared<std::function<void(size_t)>>();
    *try_sig = [this, sigs, raw_headers, body_path, result,
                try_sig, cb = std::move(cb)](size_t idx) mutable {
        if (idx >= sigs.size()) {
            if (result.result == "none") result.result = "fail";
            if (result.reason.empty()) result.reason = "no valid DKIM signature";
            cb(std::move(result));
            *try_sig = nullptr;
            return;
        }
        const auto& sig = sigs[idx];
        if (sig.algorithm != "rsa-sha256") { (*try_sig)(idx + 1); return; }

        std::string computed_bh;
        {
            std::ifstream body_file(body_path, std::ios::binary);
            if (!body_file.is_open()) {
                if (result.reason.empty()) result.reason = "cannot open DKIM body file: " + body_path;
                (*try_sig)(idx + 1);
                return;
            }
            if (!outbound::dkim_body_hash_stream(body_file, sig.body_canon, computed_bh)) {
                if (result.reason.empty()) result.reason = "streaming DKIM body hash failed";
                (*try_sig)(idx + 1);
                return;
            }
        }

        std::string key_domain = sig.selector + "._domainkey." + sig.domain;
        get_txt_async(key_domain, kDkimTtl,
            [this, sig, raw_headers, computed_bh, key_domain, idx, result,
             cb, try_sig](std::vector<std::string> txt_records) mutable {
                std::string pubkey_b64 = extract_dkim_pubkey(txt_records);
                if (pubkey_b64.empty()) {
                    if (result.reason.empty()) result.reason = "no DKIM public key found in DNS for " + key_domain;
                    (*try_sig)(idx + 1);
                    return;
                }
                std::string error;
                if (verify_dkim_with_pubkey(sig, raw_headers, computed_bh, pubkey_b64, error)) {
                    result.result = "pass";
                    result.reason = "";
                    result.selector = sig.selector;
                    result.signing_domain = sig.domain;
                    cb(std::move(result));
                    *try_sig = nullptr;
                    return;
                }
                if (result.reason.empty()) result.reason = error;
                (*try_sig)(idx + 1);
            });
    };
    (*try_sig)(0);
}

DkimResult InboundVerifier::check_dkim(const std::string& raw_headers,
                                       const std::string& raw_body) {
    DkimResult result;
    result.result = "none";

    auto sigs = parse_dkim_signatures(raw_headers);
    if (sigs.empty()) {
        result.result = "none";
        result.reason = "no DKIM-Signature header found";
        return result;
    }

    for (auto& sig : sigs) {
        if (sig.algorithm != "rsa-sha256") {
            continue; // unsupported algorithm, try next
        }

        std::string error;
        if (verify_dkim_signature(sig, raw_headers, raw_body, error)) {
            result.result = "pass";
            result.reason = "";
            result.selector = sig.selector;
            result.signing_domain = sig.domain;
            return result;
        }

        // Store first failure reason
        if (result.reason.empty()) {
            result.reason = error;
        }
    }

    // If we got here, no signature passed
    if (result.result == "none") {
        result.result = "fail";
    }
    if (result.reason.empty()) {
        result.reason = "no valid DKIM signature";
    }
    return result;
}

DkimResult InboundVerifier::check_dkim_from_file(const std::string& raw_headers,
                                                 const std::string& body_path) {
    DkimResult result;
    result.result = "none";

    auto sigs = parse_dkim_signatures(raw_headers);
    if (sigs.empty()) {
        result.result = "none";
        result.reason = "no DKIM-Signature header found";
        return result;
    }

    for (auto& sig : sigs) {
        if (sig.algorithm != "rsa-sha256") {
            continue; // unsupported algorithm, try next
        }

        std::string error;
        if (verify_dkim_signature_from_file(sig, raw_headers, body_path, error)) {
            result.result = "pass";
            result.reason = "";
            result.selector = sig.selector;
            result.signing_domain = sig.domain;
            return result;
        }

        // Store first failure reason
        if (result.reason.empty()) {
            result.reason = error;
        }
    }

    // If we got here, no signature passed
    if (result.result == "none") {
        result.result = "fail";
    }
    if (result.reason.empty()) {
        result.reason = "no valid DKIM signature";
    }
    return result;
}

// ========== DMARC ==========

bool InboundVerifier::is_aligned(const std::string& auth_domain,
                                 const std::string& from_domain) {
    if (auth_domain.empty() || from_domain.empty()) return false;
    // Strict alignment: exact match
    if (auth_domain == from_domain) return true;
    // Relaxed: one is a subdomain of the other (organizational domain match)
    // Check if the longer string ends with "." + shorter_string
    auto ends_with_dot = [](const std::string& s, const std::string& suffix) -> bool {
        if (s.size() <= suffix.size() + 1) return false;
        return s[s.size() - suffix.size() - 1] == '.' &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (auth_domain.size() > from_domain.size()) {
        return ends_with_dot(auth_domain, from_domain);
    }
    return ends_with_dot(from_domain, auth_domain);
}

DmarcResult InboundVerifier::check_dmarc(const std::string& from_domain,
                                         const SpfResult& spf,
                                         const DkimResult& dkim,
                                         const std::string& mail_from_domain) {
    DmarcResult result;
    result.result = "none";
    result.policy = "none";

    if (from_domain.empty()) {
        result.reason = "no From domain for DMARC check";
        return result;
    }

    // Query DMARC record
    std::string dmarc_domain = "_dmarc." + from_domain;
    auto txt_records = outbound::SyncDnsWrapper(dns_).resolve_txt(dmarc_domain);

    std::string dmarc_record;
    for (const auto& rec : txt_records) {
        if (rec.find("v=DMARC1") == 0) {
            dmarc_record = rec;
            break;
        }
    }

    if (dmarc_record.empty()) {
        result.result = "none";
        result.reason = "no DMARC record for " + from_domain;
        return result;
    }

    auto tags = parse_tags(dmarc_record);
    auto it_p = tags.find("p");
    result.policy = (it_p != tags.end()) ? to_lower(it_p->second) : "none";

    // Check SPF alignment
    bool spf_aligned = is_aligned(mail_from_domain, from_domain);
    bool spf_ok = (spf.result == "pass") && spf_aligned;

    // Check DKIM alignment
    bool dkim_aligned = is_aligned(dkim.signing_domain, from_domain);
    bool dkim_ok = (dkim.result == "pass") && dkim_aligned;

    if (spf_ok || dkim_ok) {
        result.result = "pass";
        result.reason = spf_ok ? "SPF aligned and passed" : "DKIM aligned and passed";
    } else {
        // Apply DMARC policy
        if (result.policy == "reject") {
            result.result = "fail";
            result.reason = "DMARC policy is reject, no aligned auth passed";
        } else if (result.policy == "quarantine") {
            result.result = "fail";
            result.reason = "DMARC policy is quarantine, no aligned auth passed";
        } else {
            result.result = "none";
            result.reason = "DMARC policy is none";
        }
    }

    return result;
}

void InboundVerifier::check_dmarc_async(const std::string& from_domain,
                                        const SpfResult& spf,
                                        const DkimResult& dkim,
                                        const std::string& mail_from_domain,
                                        std::function<void(DmarcResult)> cb) {
    DmarcResult result;
    result.result = "none";
    result.policy = "none";
    if (from_domain.empty()) {
        result.reason = "no From domain for DMARC check";
        cb(result);
        return;
    }

    std::string dmarc_domain = "_dmarc." + from_domain;
    get_txt_async(dmarc_domain, kDmarcTtl,
        [this, from_domain, spf, dkim, mail_from_domain, cb = std::move(cb)](std::vector<std::string> txt_records) mutable {
            DmarcResult r;
            r.result = "none";
            r.policy = "none";
            std::string dmarc_record;
            for (const auto& rec : txt_records) {
                if (rec.find("v=DMARC1") == 0) { dmarc_record = rec; break; }
            }
            if (dmarc_record.empty()) {
                r.reason = "no DMARC record for " + from_domain;
                cb(std::move(r));
                return;
            }

            auto tags = parse_tags(dmarc_record);
            auto it_p = tags.find("p");
            r.policy = (it_p != tags.end()) ? to_lower(it_p->second) : "none";

            // SPF/DKIM alignment + 策略评估（与同步 check_dmarc 一致）
            bool spf_aligned = is_aligned(mail_from_domain, from_domain);
            bool spf_ok = (spf.result == "pass") && spf_aligned;
            bool dkim_aligned = is_aligned(dkim.signing_domain, from_domain);
            bool dkim_ok = (dkim.result == "pass") && dkim_aligned;

            if (spf_ok || dkim_ok) {
                r.result = "pass";
                r.reason = spf_ok ? "SPF aligned and passed" : "DKIM aligned and passed";
            } else {
                if (r.policy == "reject") {
                    r.result = "fail";
                    r.reason = "DMARC policy is reject, no aligned auth passed";
                } else if (r.policy == "quarantine") {
                    r.result = "fail";
                    r.reason = "DMARC policy is quarantine, no aligned auth passed";
                } else {
                    r.result = "none";
                    r.reason = "DMARC policy is none";
                }
            }
            cb(std::move(r));
        });
}

// ========== Auth Results Header ==========

std::string InboundVerifier::build_auth_results_header(
    const std::string& authserv_id,
    const VerificationResult& result,
    const std::string& mail_from_domain) {
    std::string hdr = "Authentication-Results: " + authserv_id;

    // SPF
    hdr += "; spf=" + result.spf.result;
    if (!mail_from_domain.empty()) {
        hdr += " smtp.mailfrom=" + mail_from_domain;
    }

    // DKIM
    hdr += "; dkim=" + result.dkim.result;
    if (!result.dkim.signing_domain.empty()) {
        hdr += " header.d=" + result.dkim.signing_domain;
    }
    if (!result.dkim.selector.empty()) {
        hdr += " header.s=" + result.dkim.selector;
    }

    // DMARC
    if (result.dmarc.result != "none" || !result.dmarc.reason.empty()) {
        hdr += "; dmarc=" + result.dmarc.result;
        if (!result.dmarc.header_from_domain.empty()) {
            hdr += " header.from=" + result.dmarc.header_from_domain;
        }
    }

    return hdr;
}

} // namespace inbound
} // namespace mail_system
