// MIME 解析单元测试 — 构造合法 MIME，验证：
//   parse_mime_tree             （SMTP 收信时的预解析，写 sidecar 的数据源）
//   build_bodystructure_tree    （IMAP BODYSTRUCTURE 生成，RFC 3501 §7.4.2）
//   extract_part_content        （IMAP BODY[n] 正文提取，原始编码返回）
//
// 覆盖历史修复的 bug 类：multipart 格式、非 multipart 根 length、charset 不带引号、
// folded header boundary、DKIM h= 误匹配、BODY[n] 空内容。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/mime_parser.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"

using namespace mail_system;

struct DummyConn {};
using Fsm = TraditionalImapsFsm<DummyConn>;

static int g_pass = 0, g_fail = 0;

static void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}
static void expect_str(const std::string& got, const std::string& want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got [%s] want [%s]\n", what, got.c_str(), want.c_str()); }
}
static void expect_contains(const std::string& hay, const std::string& needle, const char* what) {
    if (hay.find(needle) != std::string::npos) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: [%s] not found in [%s]\n", what, needle.c_str(), hay.c_str()); }
}
static void expect_num(uint64_t got, uint64_t want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got %llu want %llu\n", what,
        (unsigned long long)got, (unsigned long long)want); }
}

// ========== 用例 ==========

// 1. 单 part text/plain 7bit
static void test_single_text_plain() {
    const std::string raw =
        "From: a@test.com\r\n"
        "To: b@test.com\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Hello world\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "text", "type");
    expect_str(root.subtype, "plain", "subtype");
    expect_str(root.charset, "utf-8", "charset");
    expect_str(root.encoding, "7bit", "encoding");
    // 根 part length 必须覆盖整封邮件（之前只到 header 末尾导致 BODY[1] 空）
    expect_num(root.length, raw.size(), "root.length == raw.size()");
    expect_num(root.body_size, 13, "body_size (Hello world + CRLF)");
    expect_num(root.lines, 1, "lines");
    // BODY[1] 提取
    expect_str(Fsm::extract_part_content(raw, root), "Hello world", "BODY[1] content");
    // BODYSTRUCTURE
    std::string bs = Fsm::build_bodystructure_tree(root);
    expect_contains(bs, "(\"text\" \"plain\" (\"CHARSET\" \"utf-8\") NIL NIL \"7bit\" 13 1", "BS single-part");
}

// 2. 单 part text/html quoted-printable，charset 不带引号
static void test_single_html_qp_unquoted_charset() {
    const std::string raw =
        "Content-Type: text/html; charset=us-ascii\r\n"
        "Content-Transfer-Encoding: quoted-printable\r\n"
        "\r\n"
        "<!doctype html><p>Hello=20world</p>\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "text", "type");
    expect_str(root.subtype, "html", "subtype");
    expect_str(root.charset, "us-ascii", "unquoted charset");
    expect_str(root.encoding, "quoted-printable", "encoding");
    expect_num(root.length, raw.size(), "root.length");
    expect_str(Fsm::extract_part_content(raw, root),
               "<!doctype html><p>Hello=20world</p>", "BODY[1] QP content (raw)");
}

// 3. multipart/alternative，2 个 base64 子 part（QQ 邮件场景）
static void test_multipart_alternative_base64() {
    const std::string raw =
        "Content-Type: multipart/alternative; boundary=\"BOUND1\"\r\n"
        "\r\n"
        "--BOUND1\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "SGVsbG8gd29ybGQ=\r\n"
        "--BOUND1\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "PGgxPkhlbGxvPC9oMT4=\r\n"
        "--BOUND1--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "multipart", "root type");
    expect_str(root.subtype, "alternative", "root subtype");
    expect_str(root.boundary, "BOUND1", "root boundary");
    expect_num(root.subs.size(), 2, "sub count");
    if (root.subs.size() == 2) {
        expect_str(root.subs[0].type, "text", "sub0 type");
        expect_str(root.subs[0].subtype, "plain", "sub0 subtype");
        expect_str(root.subs[0].encoding, "base64", "sub0 encoding");
        expect_str(root.subs[1].type, "text", "sub1 type");
        expect_str(root.subs[1].subtype, "html", "sub1 subtype");
        expect_str(root.subs[1].encoding, "base64", "sub1 encoding");
        // BODY[1] 返回原始 base64（RFC 3501：客户端自行解码）
        expect_str(Fsm::extract_part_content(raw, root.subs[0]), "SGVsbG8gd29ybGQ=", "BODY[1] base64");
        expect_str(Fsm::extract_part_content(raw, root.subs[1]), "PGgxPkhlbGxvPC9oMT4=", "BODY[2] base64");
    }
    // BODYSTRUCTURE 必须：子 part 在前、subtype 在后（RFC 3501 §7.4.2）
    std::string bs = Fsm::build_bodystructure_tree(root);
    expect_true(bs.size() >= 2 && bs[0] == '(' && bs[1] == '(', "BS starts with (( (subparts first)");
    expect_contains(bs, "(\"text\" \"plain\" (\"CHARSET\" \"utf-8\") NIL NIL \"base64\"", "BS has text/plain subpart");
    expect_contains(bs, "(\"text\" \"html\" (\"CHARSET\" \"utf-8\") NIL NIL \"base64\"", "BS has text/html subpart");
    expect_contains(bs, "\"alternative\" (\"BOUNDARY\" \"BOUND1\")", "BS subtype+boundary last");
    expect_true(bs.find("(\"multipart\" \"alternative\"") == std::string::npos,
                "BS NOT in wrong single-part layout");
}

// 4. multipart/mixed，带 base64 附件
static void test_multipart_mixed_attachment() {
    const std::string raw =
        "Content-Type: multipart/mixed; boundary=SEP9\r\n"
        "\r\n"
        "--SEP9\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Body text\r\n"
        "--SEP9\r\n"
        "Content-Type: application/octet-stream; name=\"file.bin\"\r\n"
        "Content-Disposition: attachment; filename=\"file.bin\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "AAECAwQ=\r\n"
        "--SEP9--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "multipart", "root type");
    expect_str(root.subtype, "mixed", "root subtype");
    expect_str(root.boundary, "SEP9", "unquoted boundary");
    expect_num(root.subs.size(), 2, "sub count");
    if (root.subs.size() == 2) {
        expect_str(root.subs[1].name, "file.bin", "attachment name");
        expect_str(root.subs[1].encoding, "base64", "attachment encoding");
        expect_str(Fsm::extract_part_content(raw, root.subs[1]), "AAECAwQ=", "attachment body (base64)");
    }
    std::string bs = Fsm::build_bodystructure_tree(root);
    expect_contains(bs, "\"mixed\"", "BS subtype mixed");
}

// 5. 嵌套 multipart（mixed 内含 alternative）
static void test_nested_multipart() {
    const std::string raw =
        "Content-Type: multipart/mixed; boundary=OUTER1\r\n"
        "\r\n"
        "--OUTER1\r\n"
        "Content-Type: multipart/alternative; boundary=INNER1\r\n"
        "\r\n"
        "--INNER1\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "plain\r\n"
        "--INNER1\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "<b>html</b>\r\n"
        "--INNER1--\r\n"
        "--OUTER1\r\n"
        "Content-Type: application/pdf; name=\"doc.pdf\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "JVBERi0xLjQ=\r\n"
        "--OUTER1--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.subtype, "mixed", "root subtype");
    expect_num(root.subs.size(), 2, "root sub count");
    if (root.subs.size() == 2) {
        expect_str(root.subs[0].subtype, "alternative", "nested multipart subtype");
        expect_num(root.subs[0].subs.size(), 2, "nested sub count");
        if (root.subs[0].subs.size() == 2) {
            expect_str(root.subs[0].subs[0].subtype, "plain", "nested leaf1");
            expect_str(root.subs[0].subs[1].subtype, "html", "nested leaf2");
        }
        expect_str(root.subs[1].name, "doc.pdf", "attachment name");
    }
    // 嵌套 BODYSTRUCTURE：内层 alternative 也必须是子 part 在前
    std::string bs = Fsm::build_bodystructure_tree(root);
    expect_contains(bs, "\"alternative\" (\"BOUNDARY\" \"INNER1\")", "nested BS alternative");
}

// 6. folded Content-Type header（boundary 在续行）
static void test_folded_content_type() {
    const std::string raw =
        "DKIM-Signature: v=1; a=rsa-sha256; d=test.com;\r\n"
        "\tb=abc123\r\n"
        "Content-Type: multipart/alternative;\r\n"
        "\tboundary=\"FOLDED1\"\r\n"
        "\r\n"
        "--FOLDED1\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "folded test\r\n"
        "--FOLDED1--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "multipart", "type with folded header");
    expect_str(root.subtype, "alternative", "subtype with folded header");
    expect_str(root.boundary, "FOLDED1", "boundary from folded line");
    expect_num(root.subs.size(), 1, "sub count");
    if (root.subs.size() == 1) {
        expect_str(Fsm::extract_part_content(raw, root.subs[0]), "folded test", "BODY[1] folded");
    }
}

// 7. DKIM-Signature h= 含 "content-type:" 子串（不误匹配）
static void test_dkim_h_contains_content_type() {
    const std::string raw =
        "DKIM-Signature: v=1; a=rsa-sha256; h=content-type:date:from:subject;\r\n"
        "\tbh=abc;\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "dkim test\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "text", "type not confused by DKIM h=");
    expect_str(root.subtype, "plain", "subtype not confused by DKIM h=");
    expect_str(root.charset, "utf-8", "charset");
    expect_str(Fsm::extract_part_content(raw, root), "dkim test", "BODY[1]");
}

// 8. charset 带引号
static void test_quoted_charset() {
    const std::string raw =
        "Content-Type: text/plain; charset=\"gb2312\"\r\n"
        "\r\n"
        "quote test\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.charset, "gb2312", "quoted charset");
}

// 10. 不带引号的 boundary + 后面有带引号的 header（如 To: "test3"）
//     —— 之前 find('"') 向后搜会误匹配 To 的引号，把 boundary 提取成 "test3"
static void test_unquoted_boundary_with_quoted_to() {
    const std::string raw =
        "From: sender@feishu.cn\r\n"
        "To: \"test3\" <test3@scut.email>\r\n"
        "Subject: test\r\n"
        "Content-Type: multipart/alternative;\r\n"
        "\tboundary=77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41\r\n"
        "\r\n"
        "--77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello feishu\r\n"
        "--77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.boundary, "77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41",
               "unquoted boundary not confused by To: quote");
    expect_num(root.subs.size(), 1, "sub count");
    if (root.subs.size() == 1) {
        expect_str(Fsm::extract_part_content(raw, root.subs[0]), "hello feishu", "BODY[1]");
    }
}

// 9. Content-Disposition filename 提取
static void test_content_disposition_name() {
    const std::string raw =
        "Content-Type: image/png; name=pic.png\r\n"
        "Content-Disposition: attachment; filename=\"pic.png\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "iVBORw0KGgo=\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "image", "image type");
    expect_str(root.subtype, "png", "image subtype");
    expect_str(root.name, "pic.png", "filename from Content-Disposition");
}

int main() {
    test_single_text_plain();
    test_single_html_qp_unquoted_charset();
    test_multipart_alternative_base64();
    test_multipart_mixed_attachment();
    test_nested_multipart();
    test_folded_content_type();
    test_dkim_h_contains_content_type();
    test_quoted_charset();
    test_content_disposition_name();
    test_unquoted_boundary_with_quoted_to();

    std::printf("\n================================\n  Passed: %d  Failed: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
