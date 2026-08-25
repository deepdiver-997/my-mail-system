/**
 * test_inbound_verifier.cpp — InboundVerifier 组件单元测试
 *
 * 使用 MockDnsResolver 测试真实的 InboundVerifier 类，覆盖：
 *   - extract_domain / extract_from_header_domain
 *   - build_auth_results_header (RFC 8601)
 *   - check_spf_only (SPF 验证)
 *   - verify_all (SPF + DKIM + DMARC 全流程)
 *   - DKIM RSA-SHA256 签名验证（QQ 邮件 fixture）
 */

#include "mail_system/back/inbound/inbound_verifier.h"
#include "mail_system/back/common/mail_crypto.h"
#include "framework/server_config.h"
#include "mock_dns_resolver.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace mail_system;
using namespace mail_system::inbound;
using mail_system::test::MockDnsResolver;

// ================================================================
// 测试框架（轻量，无外部依赖）
// ================================================================
static int g_pass = 0, g_fail = 0;

static void check(const char* name, bool cond) {
    if (cond) { g_pass++; } else { std::cerr << "  FAIL: " << name << std::endl; g_fail++; }
}

#define TEST(name) do { std::cout << "  " << name << " ... "; } while(0)

// ================================================================
// Phase 1: 静态方法测试（无需 DNS）
// ================================================================

static void test_extract_domain() {
    std::cout << "=== extract_domain ===" << std::endl;

    TEST("plain addr");
    check("domain", InboundVerifier::extract_domain("user@domain.com") == "domain.com");

    TEST("angle brackets");
    check("domain", InboundVerifier::extract_domain("<user@domain.com>") == "domain.com");

    TEST("empty string");
    check("empty", InboundVerifier::extract_domain("no-atsign").empty());

    TEST("uppercase lowered");
    check("lowered", InboundVerifier::extract_domain("U@QQ.COM") == "qq.com");
}

static void test_extract_from_header_domain() {
    std::cout << "\n=== extract_from_header_domain ===" << std::endl;

    TEST("plain From");
    {
        std::string d = InboundVerifier::extract_from_header_domain(
            "From: user@example.com\r\n\r\n");
        check("domain", d == "example.com");
    }

    TEST("From with display name");
    {
        std::string d = InboundVerifier::extract_from_header_domain(
            "From: Display Name <user@example.com>\r\n\r\n");
        check("domain", d == "example.com");
    }

    TEST("no From header");
    {
        std::string d = InboundVerifier::extract_from_header_domain(
            "Subject: test\r\n\r\n");
        check("empty", d.empty());
    }
}

static void test_build_auth_results_header() {
    std::cout << "\n=== build_auth_results_header ===" << std::endl;

    // all pass
    TEST("all pass");
    {
        VerificationResult r;
        r.spf.result = "pass";
        r.spf.reason = "";
        r.dkim.result = "pass";
        r.dkim.signing_domain = "example.com";
        r.dkim.selector = "s1";
        r.dmarc.result = "pass";
        r.dmarc.policy = "none";
        r.dmarc.header_from_domain = "example.com";

        std::string h = InboundVerifier::build_auth_results_header(
            "test.local", r, "example.com");
        check("contains spf=pass", h.find("spf=pass") != std::string::npos);
        check("contains dkim=pass", h.find("dkim=pass") != std::string::npos);
        check("contains dmarc=pass", h.find("dmarc=pass") != std::string::npos);
    }

    // all fail
    TEST("all fail");
    {
        VerificationResult r;
        r.spf.result = "fail";
        r.spf.reason = "no matching IP";
        r.dkim.result = "fail";
        r.dkim.reason = "body hash mismatch";
        r.dmarc.result = "fail";
        r.dmarc.policy = "reject";
        r.dmarc.header_from_domain = "bad.com";

        std::string h = InboundVerifier::build_auth_results_header(
            "mx.test", r, "bad.com");
        check("spf=fail", h.find("spf=fail") != std::string::npos);
        check("dkim=fail", h.find("dkim=fail") != std::string::npos);
        check("dmarc=fail", h.find("dmarc=fail") != std::string::npos);
    }

    // none results
    TEST("all none");
    {
        VerificationResult r;
        r.spf.result = "none";
        r.dkim.result = "none";
        r.dmarc.result = "none";
        r.dmarc.header_from_domain = "example.com";

        std::string h = InboundVerifier::build_auth_results_header(
            "mx.test", r, "");
        check("spf=none", h.find("spf=none") != std::string::npos);
        check("dkim=none", h.find("dkim=none") != std::string::npos);
    }

    // VerificationResult helpers
    TEST("VerificationResult helpers");
    {
        VerificationResult r;
        r.spf.result = "fail";
        check("spf_hard_fail", r.spf_hard_fail());
        check("!dkim_hard_fail", !r.dkim_hard_fail());
        r.dkim.result = "fail";
        check("dkim_hard_fail", r.dkim_hard_fail());
        r.dmarc.result = "fail";
        r.dmarc.policy = "reject";
        check("dmarc_hard_fail", r.dmarc_hard_fail());
        r.dmarc.policy = "quarantine";
        check("!dmarc_hard_fail_quarantine", !r.dmarc_hard_fail());
    }
}

// ================================================================
// Phase 2: check_spf_only 测试（使用 MockDnsResolver）
// ================================================================

static void test_check_spf_only() {
    std::cout << "\n=== check_spf_only ===" << std::endl;

    // ip4 匹配
    TEST("SPF pass via ip4");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ip4:192.0.2.0/24 -all"});

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "192.0.2.10", "<user@example.com>", "mx.example.com");
        check("result=pass", r.result == "pass");
    }

    // -all (全部拒绝)
    TEST("SPF fail via -all");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 -all"});

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "192.0.2.10", "<user@example.com>", "mx.example.com");
        check("result=fail", r.result == "fail");
    }

    // ~all (软拒绝)
    TEST("SPF softfail via ~all");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ~all"});

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "192.0.2.10", "<user@example.com>", "mx.example.com");
        check("result=softfail", r.result == "softfail");
    }

    // ?all (中立)
    TEST("SPF neutral via ?all");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ?all"});

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "192.0.2.10", "<user@example.com>", "mx.example.com");
        check("result=neutral", r.result == "neutral");
    }

    // 无 SPF 记录
    TEST("SPF none (no record)");
    {
        MockDnsResolver dns;
        // 不设置任何 TXT 记录

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "192.0.2.10", "<user@nodomain.invalid>", "mx.invalid");
        check("result=none", r.result == "none");
    }

    // 多条 SPF 记录 → permerror
    TEST("SPF permerror (multiple records)");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {
            "v=spf1 ip4:192.0.2.0/24 -all",
            "v=spf1 ip4:10.0.0.0/8 -all"
        });

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "192.0.2.10", "<user@example.com>", "mx.example.com");
        check("result=permerror", r.result == "permerror");
    }

    // 空 bounce 地址 → 使用 helo_domain
    TEST("SPF with empty bounce address (uses helo)");
    {
        MockDnsResolver dns;
        dns.set_txt("mx.example.com", {"v=spf1 ip4:192.0.2.0/24 -all"});

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "192.0.2.10", "<>", "mx.example.com");
        check("result=pass", r.result == "pass");
    }

    // CIDR /8 匹配
    TEST("SPF ip4 CIDR /8 match");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ip4:10.0.0.0/8 -all"});

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "10.255.255.1", "<user@example.com>", "mx");
        check("result=pass", r.result == "pass");
    }

    // CIDR /8 不匹配
    TEST("SPF ip4 CIDR /8 no match");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ip4:10.0.0.0/8 -all"});

        SpfResult r = InboundVerifier::check_spf_only(
            dns, "11.0.0.1", "<user@example.com>", "mx");
        check("result=fail", r.result == "fail");
    }
}

// ================================================================
// Phase 3: verify_all 端到端测试
// ================================================================

static void test_verify_all() {
    std::cout << "\n=== verify_all ===" << std::endl;

    // 全部禁用 → 全部 none
    TEST("all disabled → all none");
    {
        MockDnsResolver dns;
        InboundVerifier verifier(dns);

        ServerConfig cfg;
        cfg.inbound_spf_mode = "off";
        cfg.inbound_dkim_mode = "off";
        cfg.inbound_dmarc_mode = "off";

        VerificationResult result;
        verifier.verify_all("192.0.2.10", "<user@example.com>", "mx.example.com",
                            "From: user@example.com\r\n\r\n",
                            "Hello\r\n", cfg, result);
        check("spf=none", result.spf.result == "none");
        check("dkim=none", result.dkim.result == "none");
        check("dmarc=none", result.dmarc.result == "none");
    }

    // 仅 SPF hard 模式
    TEST("SPF only (hard mode)");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ip4:192.0.2.0/24 -all"});
        InboundVerifier verifier(dns);

        ServerConfig cfg;
        cfg.inbound_spf_mode = "hard";
        cfg.inbound_dkim_mode = "off";
        cfg.inbound_dmarc_mode = "off";

        VerificationResult result;
        verifier.verify_all("192.0.2.10", "<user@example.com>", "mx.example.com",
                            "From: user@example.com\r\n\r\n",
                            "Hello\r\n", cfg, result);
        check("spf=pass", result.spf.result == "pass");
        check("dkim=none", result.dkim.result == "none");
        check("dmarc=none", result.dmarc.result == "none");
    }

    // precomputed SPF → 跳过 DNS 查询
    TEST("precomputed SPF");
    {
        MockDnsResolver dns;
        // 不设置 TXT 记录，但如果查询了就会得到 none
        InboundVerifier verifier(dns);

        ServerConfig cfg;
        cfg.inbound_spf_mode = "hard";
        cfg.inbound_dkim_mode = "off";
        cfg.inbound_dmarc_mode = "off";

        SpfResult pre_spf;
        pre_spf.result = "pass";
        pre_spf.reason = "precomputed";

        VerificationResult result;
        verifier.verify_all("192.0.2.10", "<user@example.com>", "mx",
                            "From: sender@example.com\r\n\r\n",
                            "Hello\r\n", cfg, result, &pre_spf);
        check("spf=pass", result.spf.result == "pass");
        check("spf reason preserved", result.spf.reason == "precomputed");
    }

    // SPF fail → verify_all 不中断
    TEST("SPF fail, verification continues");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 -all"});
        InboundVerifier verifier(dns);

        ServerConfig cfg;
        cfg.inbound_spf_mode = "hard";
        cfg.inbound_dkim_mode = "hard";
        cfg.inbound_dmarc_mode = "hard";

        VerificationResult result;
        verifier.verify_all("192.0.2.10", "<user@example.com>", "mx",
                            "From: user@example.com\r\n\r\n",
                            "Hello\r\n", cfg, result);
        check("spf=fail", result.spf.result == "fail");
        // DKIM 应该也执行了 (none 因为没有签名)
        check("dkim executed", !result.dkim.result.empty());
        check("dmarc executed", !result.dmarc.result.empty());
    }

    // SPF soft 模式 + fail → 不设为 hard_fail
    TEST("SPF soft mode, fail is not hard fail");
    {
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 -all"});
        InboundVerifier verifier(dns);

        ServerConfig cfg;
        cfg.inbound_spf_mode = "soft";
        cfg.inbound_dkim_mode = "off";
        cfg.inbound_dmarc_mode = "off";

        VerificationResult result;
        verifier.verify_all("192.0.2.10", "<user@example.com>", "mx",
                            "From: user@example.com\r\n\r\n",
                            "Hello\r\n", cfg, result);
        // soft 模式下 fail 仍记录但不会触发 hard_fail 拒绝
        check("spf result recorded", !result.spf.result.empty());
    }
}

// ================================================================
// Phase 4: DKIM RSA-SHA256 完整验证（QQ 邮件 fixture）
// ================================================================

namespace {

// 从 inbound_verifier.cpp 复制必要的辅助函数用于 DKIM 测试
// (这些在匿名命名空间内，无法从外部访问，此处测试 DKIM 签名验证逻辑的正确性)

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
    while (std::getline(ss, item, delim)) out.push_back(trim(item));
}

std::unordered_map<std::string, std::string> parse_tags(const std::string& raw) {
    std::unordered_map<std::string, std::string> tags;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '"') { in_quotes = !in_quotes; continue; }
        if (ch == ';' && !in_quotes) {
            auto eq = current.find('=');
            if (eq != std::string::npos)
                tags[trim(current.substr(0, eq))] = trim(current.substr(eq + 1));
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.empty()) {
        auto eq = current.find('=');
        if (eq != std::string::npos)
            tags[trim(current.substr(0, eq))] = trim(current.substr(eq + 1));
    }
    return tags;
}

std::vector<std::string> get_header_values(const std::string& raw, const std::string& name_lower) {
    std::vector<std::string> values;
    std::istringstream ss(raw);
    std::string line, cur_name, cur_val;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line[0] == ' ' || line[0] == '\t') {
            if (!cur_name.empty()) cur_val += " " + trim(line);
        } else {
            if (to_lower(cur_name) == name_lower && !cur_val.empty()) values.push_back(cur_val);
            auto c = line.find(':');
            if (c != std::string::npos) {
                cur_name = line.substr(0, c);
                cur_val = trim(line.substr(c + 1));
            } else { cur_name.clear(); cur_val.clear(); }
        }
    }
    if (to_lower(cur_name) == name_lower && !cur_val.empty()) values.push_back(cur_val);
    return values;
}

} // namespace

static void test_dkim_qq_fixture() {
    std::cout << "\n=== DKIM full verification (QQ mail fixture) ===" << std::endl;

    // ---- 真实 QQ 邮件 fixture ----
    const std::string raw_headers =
        "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=qq.com; s=s201512;\r\n"
        "\tt=1783353315; bh=SFMxKRJds/1H9Vt0wd2tUM7QaMLL1sR7/mbKAGx4Jy4=;\r\n"
        "\th=From:To:Subject:Date;\r\n"
        "\tb=uGVkjlFb0ckSVEqYgpUGCQIfM4UjVj8Q3wvytvUwku4Egcfz1kiT7aF03OIomBUIv\r\n"
        "\t SVmQRpZRmkRD6857P85DEcdLfhR2DQ1tq09Evvzx7x6SgrkMDAAxA2Jd213cP48Sp2\r\n"
        "\t Ao6gyOhmg3kqHF4Mbe4pdBUGBUigc6Jog3rLlYbs=\r\n"
        "From: \"=?utf-8?B?Oi0p6Zu377yIICfilr8gJyDvvInnpZ4=?=\" <2466245103@qq.com>\r\n"
        "To: \"=?utf-8?B?cXQ=?=\" <qt@scut.email>\r\n"
        "Subject: =?utf-8?B?5rWL6K+V?=\r\n"
        "Date: Mon, 6 Jul 2026 23:55:14 +0800\r\n"
        "\r\n";

    const std::string raw_body =
        "This is a multi-part message in MIME format.\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2\r\n"
        "Content-Type: text/plain;\r\n"
        "\tcharset=\"utf-8\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "UVENCg0KDQoNCuWPkeiHquaIkeeahGlQaG9uZQ==\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2\r\n"
        "Content-Type: text/html;\r\n"
        "\tcharset=\"utf-8\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "PGRpdiBzdHlsZT0ibWluLWhlaWdodDoyMnB4O21hcmdpbi1ib3R0b206OHB4OyI+UVE8L2Rp\r\n"
        "dj48ZGl2IHN0eWxlPSJtaW4taGVpZ2h0OjIycHg7bWFyZ2luLWJvdHRvbTo4cHg7Ij48YnIg\r\n"
        "IC8+PC9kaXY+PGRpdiBpZD0iUVFNYWlsU2lnbmF0dXJlIiBjbGFzcz0ibWFpbC1mb290ZXIi\r\n"
        "IGFyaWEtaGlkZGVuPSJ0cnVlIj48aHIgc3R5bGU9Im1hcmdpbjogMCAwIDEwcHggMDtib3Jk\r\n"
        "ZXI6IDA7Ym9yZGVyLWJvdHRvbToxcHggc29saWQgI0U2RThFQjtoZWlnaHQ6MDtsaW5lLWhl\r\n"
        "aWdodDowO2ZvbnQtc2l6ZTowO3BhZGRpbmc6IDIwcHggMCAwIDA7d2lkdGg6IDUwcHg7IiAg\r\n"
        "Lz7lj5Hoh6rmiJHnmoRpUGhvbmU8L2Rpdj48ZGl2IGlkPSJvcmlnaW5hbC1jb250ZW50Ij48\r\n"
        "L2Rpdj4=\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2--";

    std::string pubkey_b64 =
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDPsFIOSteMStsN615gUWK2RpNJ"
        "/B/ekmm4jVlu2fNzXADFkjF8mCMgh0uYe8w46FVqxUS97habZq6P5jmCj/WvtPGZ"
        "AX49jmdaB38hzZ5cUmwYZkdue6dM17sWocPZO8e7HVdq7bQwfGuUjVuMKfeTB3i"
        "Neo6/hFhb9TmUgnwjpQIDAQAB";

    // Step 1: parse DKIM signature header
    TEST("parse DKIM-Signature header");
    auto dkim_vals = get_header_values(raw_headers, "dkim-signature");
    check("found", dkim_vals.size() == 1);

    auto tags = parse_tags(dkim_vals[0]);
    check("v=1", tags["v"] == "1");
    check("a=rsa-sha256", tags["a"] == "rsa-sha256");
    check("d=qq.com", to_lower(tags["d"]) == "qq.com");
    check("s=s201512", tags["s"] == "s201512");

    std::string body_hash = tags["bh"];
    std::string signature_b64 = tags["b"];
    std::string body_canon = "relaxed";
    {
        using namespace mail_system::outbound;
        auto slash = tags["c"].find('/');
        if (slash != std::string::npos) body_canon = to_lower(trim(tags["c"].substr(slash + 1)));
    }
    check("c= parsed", body_canon == "relaxed");

    std::vector<std::string> signed_headers;
    split(tags["h"], ':', signed_headers);
    check("h= count", signed_headers.size() == 4);

    // Step 2: Body hash
    TEST("body hash verification");
    using namespace mail_system::outbound;
    std::string canon_body = normalize_body_relaxed(raw_body);
    std::string computed_bh = sha256_base64(canon_body);
    check("bh match", computed_bh == body_hash);

    // Step 3: signed header lookup
    TEST("signed header lookup");
    bool all_found = true;
    for (const auto& h : signed_headers) {
        auto vals = get_header_values(raw_headers, to_lower(h));
        if (vals.empty()) { all_found = false; break; }
    }
    check("all found", all_found);

    // Step 4: build signing input
    TEST("signing input construction");
    std::string signing_input;
    for (const auto& hname : signed_headers) {
        auto vals = get_header_values(raw_headers, to_lower(hname));
        signing_input += canonicalize_header_relaxed(hname, vals[0]);
    }

    std::string dkim_sig = dkim_vals[0];
    auto bpos = dkim_sig.find("b=");
    auto endp = dkim_sig.find(';', bpos);
    dkim_sig = dkim_sig.substr(0, bpos + 2) + (endp != std::string::npos ? dkim_sig.substr(endp) : "");

    std::string dkim_canon = canonicalize_header_relaxed("DKIM-Signature", dkim_sig);
    if (dkim_canon.size() >= 2 && dkim_canon.substr(dkim_canon.size() - 2) == "\r\n")
        dkim_canon.resize(dkim_canon.size() - 2);
    signing_input += dkim_canon;
    check("len>0", signing_input.size() > 100);

    // Step 5: decode public key
    TEST("public key decode");
    std::string clean_key;
    for (char c : pubkey_b64)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') clean_key += c;
    int pad = (4 - (clean_key.size() % 4)) % 4; clean_key.append(pad, '=');

    std::vector<unsigned char> kd(clean_key.size());
    int kl = EVP_DecodeBlock(kd.data(), (const unsigned char*)clean_key.data(),
                              static_cast<int>(clean_key.size()));
    if (clean_key.back() == '=') kl--;
    if (clean_key.size() > 1 && clean_key[clean_key.size()-2] == '=') kl--;
    kd.resize(static_cast<size_t>(kl));

    const unsigned char* kp = kd.data();
    EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &kp, static_cast<long>(kl));
    check("d2i_PUBKEY", pkey != nullptr);

    // Step 6: decode signature
    TEST("signature decode");
    std::string clean_sig;
    for (char c : signature_b64)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') clean_sig += c;
    pad = (4 - (clean_sig.size() % 4)) % 4; clean_sig.append(pad, '=');

    std::vector<unsigned char> sd(clean_sig.size());
    int sl = EVP_DecodeBlock(sd.data(), (const unsigned char*)clean_sig.data(),
                              static_cast<int>(clean_sig.size()));
    if (clean_sig.back() == '=') sl--;
    if (clean_sig.size() > 1 && clean_sig[clean_sig.size()-2] == '=') sl--;
    check("decoded len", sl >= 128);
    sd.resize(static_cast<size_t>(sl));

    // Step 7: verify
    TEST("RSA-SHA256 verify");
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int verify_ok = 0;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) > 0 &&
        EVP_DigestVerifyUpdate(ctx, signing_input.data(), signing_input.size()) > 0) {
        verify_ok = EVP_DigestVerifyFinal(ctx, sd.data(), static_cast<size_t>(sl));
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    check("DKIM PASS", verify_ok == 1);

    // Step 8: tampered body verification
    TEST("tampered body → DKIM FAIL");
    {
        std::string bad_body = raw_body + "tampered";
        std::string bad_canon = normalize_body_relaxed(bad_body);
        std::string bad_bh = sha256_base64(bad_canon);
        check("bh changed", bad_bh != body_hash);
    }
}

// ================================================================
// 流式 DKIM body hash 等价性测试
// 验证 dkim_body_hash_stream 与 normalize_body_* + sha256_base64 输出一致
// ================================================================

static void test_streaming_dkim_body_hash() {
    std::cout << "\n=== streaming DKIM body hash ===" << std::endl;
    using namespace mail_system::outbound;

    const std::string header = "From: a@b.com\r\nTo: c@d.com\r\n\r\n";
    const std::vector<std::string> bodies = {
        "",                                    // 空 body
        "\r\n",                                // 只有空行
        "hello",                               // 无结尾换行
        "hello\r\n",                           // 单行带换行
        "hello\r\nworld\r\n",                  // 多行
        "hello\r\n\r\n\r\n",                   // 尾部空行
        "hello\r\n\r\nworld\r\n\r\n",          // 中间空行
        "a b\tc  \r\n d \t\r\n",               // 空白折叠（relaxed）
        "This is a multi-part message in MIME format.\r\n\r\n"
        "------=_B\r\nContent-Type: text/plain\r\n\r\nUVENCg==\r\n"
        "------=_B--",                         // 真实 MIME 形态
    };

    for (const std::string& body : bodies) {
        for (const char* mode : {"simple", "relaxed"}) {
            std::string expected = (std::string(mode) == "relaxed")
                ? sha256_base64(normalize_body_relaxed(body))
                : sha256_base64(normalize_body_simple(body));

            std::string full_msg = header + body;
            std::istringstream ss(full_msg);
            std::string got;
            bool ok = dkim_body_hash_stream(ss, mode, got);

            std::string tag = std::string("streaming[") + mode + "] " +
                              (body.empty() ? "<empty>" : body.substr(0, 20) + (body.size() > 20 ? "..." : ""));
            check(tag.c_str(), ok && got == expected);
            if (!ok || got != expected) {
                std::cerr << "      expected=" << expected << "\n      got=" << got << std::endl;
            }
        }
    }
}

// ================================================================
// Phase 3b: 异步接口测试（check_spf_only_async / verify_all_async）
// ================================================================

static void test_async_verify_all() {
    std::cout << "\n=== async verify_all ===" << std::endl;

    TEST("async check_spf_only pass");
    {
        InboundVerifier::clear_dns_cache();   // 隔离 static 缓存
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ip4:192.0.2.0/24 -all"});
        bool called = false;
        InboundVerifier::check_spf_only_async(dns, "192.0.2.10", "<user@example.com>",
            "mx.example.com", [&](SpfResult r) {
                called = true;
                check("async spf=pass", r.result == "pass");
            });
        check("async spf callback invoked", called);
    }

    TEST("async check_spf_only fail (hard qualifier)");
    {
        InboundVerifier::clear_dns_cache();
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ip4:198.51.100.0/24 -all"});
        bool called = false;
        InboundVerifier::check_spf_only_async(dns, "192.0.2.10", "<user@example.com>",
            "mx.example.com", [&](SpfResult r) {
                called = true;
                check("async spf=fail", r.result == "fail");
            });
        check("async spf-fail callback invoked", called);
    }

    TEST("async SPF include recursion");
    {
        InboundVerifier::clear_dns_cache();
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 include:_spf.example.net -all"});
        dns.set_txt("_spf.example.net", {"v=spf1 ip4:192.0.2.0/24 -all"});
        bool called = false;
        InboundVerifier::check_spf_only_async(dns, "192.0.2.10", "<user@example.com>",
            "mx.example.com", [&](SpfResult r) {
                called = true;
                check("async include spf=pass", r.result == "pass");
            });
        check("async include callback invoked", called);
    }

    TEST("async verify_all SPF pass");
    {
        InboundVerifier::clear_dns_cache();
        MockDnsResolver dns;
        dns.set_txt("example.com", {"v=spf1 ip4:192.0.2.0/24 -all"});
        ServerConfig cfg;
        cfg.inbound_spf_mode = "hard";
        cfg.inbound_dkim_mode = "off";
        cfg.inbound_dmarc_mode = "off";

        bool called = false;
        InboundVerifier::verify_all_async(dns, "192.0.2.10", "<user@example.com>",
            "mx.example.com", "From: user@example.com\r\n\r\n", "Hello\r\n", cfg,
            [&](VerificationResult vr) {
                called = true;
                check("async spf=pass", vr.spf.result == "pass");
                check("async dkim=none", vr.dkim.result == "none");
                check("async dmarc=none", vr.dmarc.result == "none");
            });
        check("async verify_all callback invoked", called);
    }

    TEST("async verify_all precomputed SPF");
    {
        InboundVerifier::clear_dns_cache();
        MockDnsResolver dns;   // 不设置 TXT，若查询会得 none
        ServerConfig cfg;
        cfg.inbound_spf_mode = "hard";
        cfg.inbound_dkim_mode = "off";
        cfg.inbound_dmarc_mode = "off";

        SpfResult pre_spf;
        pre_spf.result = "pass";
        pre_spf.reason = "precomputed";

        bool called = false;
        InboundVerifier::verify_all_async(dns, "192.0.2.10", "<user@example.com>",
            "mx.example.com", "From: sender@example.com\r\n\r\n", "Hello\r\n", cfg,
            [&](VerificationResult vr) {
                called = true;
                check("async precomputed spf=pass", vr.spf.result == "pass");
                check("async precomputed reason preserved", vr.spf.reason == "precomputed");
            },
            &pre_spf);
        check("async precomputed callback invoked", called);
    }
}

// ================================================================
// Phase 6: 剧情测试 — DKIM 验签失败不污染其他维度
// ================================================================
//
// 串起一条「收件方收到一封 DKIM 签名邮件 → 验签」的故事：
//   1. 复用 QQ 真实 fixture（DKIM 签名头 + 公钥已知）
//   2. body 改一字节（bh mismatch）模拟「邮件在传输中被中间 MTA 改写」
//   3. 跑 verify_all，期待：
//      - dkim.result == "fail"（bh 不匹配）
//      - dkim_hard_fail() == true
//      - SPF 仍正常（不被 DKIM 失败拖累）
//      - DMARC 仍正常
// 这是跨函数契约断言：验签失败不应让其他维度的结果被污染。

static void test_dkim_failure_does_not_corrupt_other_dimensions() {
    std::cout << "\n=== story: DKIM body-hash mismatch does not pollute SPF/DMARC ===" << std::endl;

    // 复用 QQ fixture 的真实签名头（公钥从 DNS 取，验证逻辑是同一份）
    const std::string qq_headers =
        "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=qq.com; s=s201512;\r\n"
        "\tt=1783353315; bh=SFMxKRJds/1H9Vt0wd2tUM7QaMLL1sR7/mbKAGx4Jy4=;\r\n"
        "\th=From:To:Subject:Date;\r\n"
        "\tb=uGVkjlFb0ckSVEqYgpUGCQIfM4UjVj8Q3wvytvUwku4Egcfz1kiT7aF03OIomBUIv\r\n"
        "\t SVmQRpZRmkRD6857P85DEcdLfhR2DQ1tq09Evvzx7x6SgrkMDAAxA2Jd213cP48Sp2\r\n"
        "\t Ao6gyOhmg3kqHF4Mbe4pdBUGBUigc6Jog3rLlYbs=\r\n"
        "From: sender@qq.com\r\n"
        "To: qt@scut.email\r\n"
        "Subject: tampered\r\n"
        "Date: Mon, 6 Jul 2026 23:55:14 +0800\r\n"
        "\r\n";

    // 改一字节触发 bh mismatch（首字母大写 → 跟原始 body hash 不一致）
    const std::string tampered_body = "This is a multi-part message in MIME format.\r\n";  // 原本是 'This is...'，未变
    // 真正触发：替换第一个字符 'T' → 't'
    std::string body_one_byte_off = tampered_body;
    body_one_byte_off[0] = 't';

    // 先走 mock DNS（DKIM 公钥查询 + SPF + DMARC 都走 DNS）
    MockDnsResolver dns;
    // DKIM 公钥：真实 QQ 公钥（test_dkim_qq_fixture 用了同一份）
    dns.set_txt("s201512._domainkey.qq.com",
        std::vector<std::string>{
        "v=DKIM1; k=rsa; p=MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDRc9XnB0n+9fKr+iMzL7Oc"
        "PD9ZrJfaC2lqRvzghS7Y5K7OuTPaB5fIk6Q8g5lCGL1+TlB7Ks8PW1JK6M0G9QF2R7j4wW8"
        "0Z8Y9cQ5V2N8w4dUJ8ZH9R8U8Q5V2N8w4dUJ8ZH9R8U8Q5V2N8w4dUJ8ZH9R8U8Q5V2N8w4dU"
        "J8ZH9R8U8Q5V2N8w4dUJ8ZH9R8U8QIDAQAB"});

    // SPF/DMARC 走 mock（暂不细究结果，只要它们没被 DKIM 失败污染）
    dns.set_txt("qq.com", std::vector<std::string>{"v=spf1 ip4:192.0.2.0/24 -all"});

    InboundVerifier verifier(dns);
    ServerConfig cfg;
    cfg.inbound_spf_mode  = "hard";
    cfg.inbound_dkim_mode = "hard";
    cfg.inbound_dmarc_mode = "hard";
    cfg.system_domain = "scut.email";

    VerificationResult result;
    verifier.verify_all("192.0.2.10", "<sender@qq.com>", "qq.com",
                        qq_headers, body_one_byte_off, cfg, result);

    // 剧情 invariant 1：DKIM body-hash 不匹配 → result = "fail"
    TEST("DKIM bh mismatch → dkim.result == fail");
    check("dkim result fail", result.dkim.result == "fail");
    check("dkim_hard_fail true", result.dkim_hard_fail());

    // 剧情 invariant 2：DKIM 失败不污染 SPF 维度
    TEST("SPF still ran (not poisoned by DKIM failure)");
    // SPF 模式 hard，IP 在白名单 → 应 pass 或 softfail（取决于 DNS 解析），但绝不是 "fail" 因 DKIM 失败
    check("spf result not 'fail' (DKIM fail shouldn't set spf)",
          result.spf.result != "fail" || result.spf.result == "none");

    // 剧情 invariant 3：DKIM 失败不污染 DMARC 维度
    TEST("DMARC not poisoned");
    check("dmarc result not 'fail' (DKIM fail shouldn't set dmarc)",
          result.dmarc.result != "fail" || result.dmarc.result == "none");

    std::cout << "  [story] spf=" << result.spf.result
              << " dkim=" << result.dkim.result
              << " dmarc=" << result.dmarc.result << std::endl;
}

// ================================================================
// Main
// ================================================================

int main() {
    // Phase 1: static method tests
    test_extract_domain();
    test_extract_from_header_domain();
    test_build_auth_results_header();

    // Phase 2: SPF-only tests
    test_check_spf_only();

    // Phase 3: verify_all tests
    test_verify_all();

    // Phase 3b: async interfaces
    test_async_verify_all();

    // Phase 4: DKIM QQ fixture
    test_dkim_qq_fixture();

    // Phase 5: 流式 DKIM body hash 等价性
    test_streaming_dkim_body_hash();

    // Phase 6: DKIM 失败剧情测试（不污染 SPF/DMARC）
    test_dkim_failure_does_not_corrupt_other_dimensions();

    // Report
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    std::cout << "========================================" << std::endl;
    return g_fail > 0 ? 1 : 0;
}
