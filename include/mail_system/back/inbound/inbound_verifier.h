#ifndef MAIL_SYSTEM_INBOUND_VERIFIER_H
#define MAIL_SYSTEM_INBOUND_VERIFIER_H

#include "mail_system/back/outbound/dns_resolver.h"
#include <string>
#include <vector>

namespace mail_system {

// Forward declarations
struct ServerConfig;

namespace inbound {

struct SpfResult {
    std::string result;  // pass/fail/neutral/softfail/temperror/permerror/none
    std::string reason;
};

struct DkimResult {
    std::string result;       // pass/fail/neutral/temperror/permerror/none
    std::string reason;
    std::string selector;     // from s= tag
    std::string signing_domain; // from d= tag
};

struct DmarcResult {
    std::string result;  // pass/fail/none/temperror/permerror
    std::string reason;
    std::string policy;  // p=none/quarantine/reject
    std::string header_from_domain;  // From 头域名，用于 Authentication-Results
};

struct VerificationResult {
    SpfResult spf;
    DkimResult dkim;
    DmarcResult dmarc;

    bool spf_hard_fail() const {
        return spf.result == "fail";
    }
    bool dkim_hard_fail() const {
        return dkim.result == "fail";
    }
    bool dmarc_hard_fail() const {
        return dmarc.result == "fail" && dmarc.policy == "reject";
    }
};

class InboundVerifier {
public:
    explicit InboundVerifier(outbound::IDnsResolver& dns);

    // 运行全套验证（SPF + DKIM + DMARC），结果存入 result
    // precomputed_spf 非空时跳过 SPF 查询，直接复用（用于 MAIL FROM 阶段已查过的场景）
    void verify_all(const std::string& client_ip,
                    const std::string& mail_from,
                    const std::string& helo_domain,
                    const std::string& raw_headers,
                    const std::string& raw_body,
                    const ServerConfig& config,
                    VerificationResult& result,
                    const SpfResult* precomputed_spf = nullptr);

    // 构建 Authentication-Results 头（RFC 8601 格式）
    static std::string build_auth_results_header(
        const std::string& authserv_id,
        const VerificationResult& result,
        const std::string& mail_from_domain);

    // 仅执行 SPF 验证（用于 MAIL FROM 阶段提前拒绝，其余返回 none）
    static SpfResult check_spf_only(outbound::IDnsResolver& dns,
                                    const std::string& client_ip,
                                    const std::string& mail_from,
                                    const std::string& helo_domain);

    // 域名提取
    static std::string extract_domain(const std::string& addr);
    static std::string extract_from_header_domain(const std::string& headers);

private:
    // SPF 验证
    SpfResult check_spf(const std::string& client_ip,
                       const std::string& mail_from,
                       const std::string& helo_domain,
                       int depth = 0);

    // DKIM 验证
    DkimResult check_dkim(const std::string& raw_headers,
                          const std::string& raw_body);

    // DMARC 验证
    DmarcResult check_dmarc(const std::string& from_domain,
                            const SpfResult& spf,
                            const DkimResult& dkim,
                            const std::string& mail_from_domain);

    // SPF 记录解析辅助
    struct SpfMechanism {
        std::string qualifier;  // +, -, ~, ?
        std::string mechanism;  // ip4, ip6, a, mx, ptr, include, all, redirect, exp
        std::string value;
        std::string cidr;
    };
    std::vector<SpfMechanism> parse_spf_record(const std::string& record);
    std::string eval_spf_mechanism(const SpfMechanism& mech,
                                   const std::string& client_ip,
                                   const std::string& domain,
                                   int depth);

    // 头部分析
    struct DkimSignature {
        std::string version;
        std::string algorithm;
        std::string domain;
        std::string selector;
        std::string body_hash;
        std::string signature;
        std::string body_canon = "simple";   // c= tag body algo (simple/relaxed)
        std::string header_canon = "simple"; // c= tag header algo (simple/relaxed)
        std::vector<std::string> signed_headers;
        std::string raw_value;
    };
    std::vector<DkimSignature> parse_dkim_signatures(const std::string& raw_headers);
    bool verify_dkim_signature(const DkimSignature& sig,
                               const std::string& raw_headers,
                               const std::string& raw_body,
                               std::string& error_out);

    // 对齐检查
    static bool is_aligned(const std::string& auth_domain,
                          const std::string& from_domain);

    outbound::SyncDnsWrapper dns_;
};

} // namespace inbound
} // namespace mail_system

#endif // MAIL_SYSTEM_INBOUND_VERIFIER_H
