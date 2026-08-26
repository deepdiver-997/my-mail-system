#ifndef MAIL_SYSTEM_INBOUND_VERIFIER_H
#define MAIL_SYSTEM_INBOUND_VERIFIER_H

#include "mail_system/back/mailServer/outbound/dns_resolver.h"
#include <chrono>
#include <functional>
#include <optional>
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

    // 同 verify_all，但 DKIM 正文从 body 文件流式读取（body_path 为含 header 的完整消息文件）
    // 避免把整封正文读回内存。SPF/DMARC 语义与 verify_all 完全一致。
    void verify_all_from_file(const std::string& client_ip,
                              const std::string& mail_from,
                              const std::string& helo_domain,
                              const std::string& raw_headers,
                              const std::string& body_path,
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

    // ---- 异步入口（直接用 IDnsResolver 原生异步接口，CPS 续传）----
    // 回调在 c-ares 线程触发；FSM 需用 connection->get_executor() post 回 io_context 后再操作 session。
    static void check_spf_only_async(outbound::IDnsResolver& dns,
                                     const std::string& client_ip,
                                     const std::string& mail_from,
                                     const std::string& helo_domain,
                                     std::function<void(SpfResult)> cb);

    // 完整校验（内存版）异步
    static void verify_all_async(outbound::IDnsResolver& dns,
                                 const std::string& client_ip,
                                 const std::string& mail_from,
                                 const std::string& helo_domain,
                                 const std::string& raw_headers,
                                 const std::string& raw_body,
                                 const ServerConfig& config,
                                 std::function<void(VerificationResult)> cb,
                                 const SpfResult* precomputed_spf = nullptr);

    // 清空共享 DNS 记录缓存（测试隔离 / 手动失效）
    static void clear_dns_cache();

    // 完整校验（body 文件流式版）异步 — 生产 DATA_END 用
    static void verify_all_from_file_async(outbound::IDnsResolver& dns,
                                           const std::string& client_ip,
                                           const std::string& mail_from,
                                           const std::string& helo_domain,
                                           const std::string& raw_headers,
                                           const std::string& body_path,
                                           const ServerConfig& config,
                                           std::function<void(VerificationResult)> cb,
                                           const SpfResult* precomputed_spf = nullptr);

    // 域名提取
    static std::string extract_domain(const std::string& addr);
    static std::string extract_from_header_domain(const std::string& headers);

private:
    struct SpfMechanism;   // 前向声明（定义见下），供 eval_spf_mechanisms_async 签名使用

    // SPF 验证（同步，内部用 SyncDnsWrapper；保留给单元测试）
    SpfResult check_spf(const std::string& client_ip,
                       const std::string& mail_from,
                       const std::string& helo_domain,
                       int depth = 0);

    // SPF 验证（异步 CPS：DNS 查询通过回调续传）
    void check_spf_async(const std::string& client_ip,
                         const std::string& mail_from,
                         const std::string& helo_domain,
                         std::function<void(SpfResult)> cb);

    // 异步评估 SPF 机制列表（从 idx 续传），cb 传 "match"/"no_match"/"temperror"/"permerror"
    void eval_spf_mechanisms_async(const std::vector<SpfMechanism>& mechs,
                                   size_t idx,
                                   const std::string& client_ip,
                                   const std::string& domain,
                                   int depth,
                                   std::function<void(std::string)> cb);

    // DKIM 验证（正文在内存字符串中）
    DkimResult check_dkim(const std::string& raw_headers,
                          const std::string& raw_body);

    // DKIM 验证（正文从 body 文件流式读取）
    DkimResult check_dkim_from_file(const std::string& raw_headers,
                                    const std::string& body_path);

    // DKIM 验证（异步 CPS：body hash 本地计算，仅 key DNS 查询异步）
    void check_dkim_async(const std::string& raw_headers,
                          const std::string& raw_body,
                          std::function<void(DkimResult)> cb);
    void check_dkim_from_file_async(const std::string& raw_headers,
                                    const std::string& body_path,
                                    std::function<void(DkimResult)> cb);

    // DMARC 验证（同步）
    DmarcResult check_dmarc(const std::string& from_domain,
                            const SpfResult& spf,
                            const DkimResult& dkim,
                            const std::string& mail_from_domain);

    // DMARC 验证（异步 CPS：_dmarc DNS 查询异步）
    void check_dmarc_async(const std::string& from_domain,
                           const SpfResult& spf,
                           const DkimResult& dkim,
                           const std::string& mail_from_domain,
                           std::function<void(DmarcResult)> cb);

    // 查 TXT（带 DNS 记录缓存：命中直接回调；未命中异步查 + 回填缓存）
    void get_txt_async(const std::string& key, std::chrono::seconds ttl,
                       std::function<void(std::vector<std::string>)> cb);

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
    bool verify_dkim_signature_from_file(const DkimSignature& sig,
                                         const std::string& raw_headers,
                                         const std::string& body_path,
                                         std::string& error_out);

    // 给定已计算好的 body hash（base64），完成 DNS 拉取 + header canonicalization + RSA 验签
    bool verify_dkim_signature_impl(const DkimSignature& sig,
                                    const std::string& raw_headers,
                                    const std::string& computed_bh,
                                    std::string& error_out);

    // 验签核心（pubkey 已从 DNS 获取；供异步 DKIM 复用，避免重复 DNS 代码路径）
    bool verify_dkim_with_pubkey(const DkimSignature& sig,
                                 const std::string& raw_headers,
                                 const std::string& computed_bh,
                                 const std::string& pubkey_b64,
                                 std::string& error_out);

    // 对齐检查
    static bool is_aligned(const std::string& auth_domain,
                          const std::string& from_domain);

    outbound::IDnsResolver& dns_;   // 原生异步 DNS 接口
};

} // namespace inbound
} // namespace mail_system

#endif // MAIL_SYSTEM_INBOUND_VERIFIER_H
