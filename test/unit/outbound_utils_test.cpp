// outbound 子模块剧情测试 — 覆盖产品真正在跑的 3 个纯组件：
//   - has_external_recipient         (pure logic, persistent_queue.cpp:390 调)
//   - ensure_mail_raw_payload_loaded  (file IO,     outbox_repository.cpp:150 调)
//   - build_outbound_message          (DKIM + 报文拼装, 邮件投递的最后一公里)
//
// 不测 OutboundServer / OutboxRepository::claim_batch / build_target_hosts
// — 那些是死代码（m_outboundServer 引用了但无人赋值），测了反而误导。
//
// DKIM 测试需要一个有效的 RSA 私钥；在测试 setup 时用 openssl 生成
// （首次跑稍慢约 200ms，之后命中 os page cache）。
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/outbound/outbound_config.h"
#include "mail_system/back/outbound/outbound_utils.h"
#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/outbound/mx_routing_utils.h"

using namespace mail_system;

namespace {

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

std::filesystem::path tmp_dir() {
    return std::filesystem::temp_directory_path() / "protorelay_outbound_test";
}

// 用 openssl 生成 1024-bit RSA 私钥（DKIM 测试需要）。已存在则复用。
// 如果系统没装 openssl，跳过 DKIM 路径相关测试。
std::string dkim_key_file() {
    auto path = tmp_dir() / "dkim_test.key";
    if (std::filesystem::exists(path)) return path.string();

    std::filesystem::create_directories(tmp_dir());
    std::string cmd = "openssl genrsa -out '" + path.string() + "' 1024 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) return {};
    return path.string();
}

} // namespace

int main() {
    std::printf("outbound_utils_test\n");
    auto dir = tmp_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // ============== Section 1: has_external_recipient ==============
    // 决定邮件走「本地落盘」还是「外部投递」分支 — 邮件路由的开关
    {
        mail m;
        std::printf("  -- has_external_recipient --\n");

        // 1.1: 全 internal → false
        m.to = {"alice@scut.email", "bob@scut.email"};
        expect_true(!outbound::has_external_recipient(m, "scut.email"),
                    "1.1: all internal recipients -> not external");

        // 1.2: 全 external → true
        m.to = {"alice@gmail.com", "bob@qq.com"};
        expect_true(outbound::has_external_recipient(m, "scut.email"),
                    "1.2: all external -> external");

        // 1.3: 混合（任一外部即 true）— 实际产品：本地用户转发给外部用户
        m.to = {"alice@scut.email", "bob@gmail.com"};
        expect_true(outbound::has_external_recipient(m, "scut.email"),
                    "1.3: mixed list with one external -> external");

        // 1.4: 空 to 列表 → false（不投递也不视为外部）
        m.to = {};
        expect_true(!outbound::has_external_recipient(m, "scut.email"),
                    "1.4: empty to list -> not external");

        // 1.5: 非法地址（无 @）→ false（无法解析域 = 不算外部）
        m.to = {"not-an-email"};
        expect_true(!outbound::has_external_recipient(m, "scut.email"),
                    "1.5: malformed address (no @) -> not external");

        // 1.6: `a@` 形式（@ 后无域）→ false
        m.to = {"a@", "b@"};
        expect_true(!outbound::has_external_recipient(m, "scut.email"),
                    "1.6: 'a@' (empty domain after @) -> not external");
    }

    // ============== Section 2: ensure_mail_raw_payload_loaded ==============
    // 落盘后 body 字段被清空（节省内存），投递前需要按 body_path 重新加载
    {
        std::printf("  -- ensure_mail_raw_payload_loaded --\n");

        // 2.1: body 已非空 → true，不读文件，body 保持原值
        mail m;
        m.body = "already in memory";
        m.body_path = "/nonexistent/should/not/be/read";
        expect_true(outbound::ensure_mail_raw_payload_loaded(m),
                    "2.1: body non-empty returns true (no file read)");
        expect_true(m.body == "already in memory",
                    "2.1: body unchanged when already loaded");

        // 2.2: body 空 + body_path 空 → false（无源可加载）
        m.body.clear();
        m.body_path.clear();
        expect_true(!outbound::ensure_mail_raw_payload_loaded(m),
                    "2.2: empty body + empty path -> false");

        // 2.3: body 空 + body_path 指向不存在文件 → false
        m.body.clear();
        m.body_path = "/this/path/does/not/exist/at/all";
        expect_true(!outbound::ensure_mail_raw_payload_loaded(m),
                    "2.3: missing file path -> false");

        // 2.4: body 空 + 有效 body_path → true, body 被填上文件内容
        auto body_file = dir / "body.txt";
        std::ofstream(body_file) << "loaded from disk\r\n";
        m.body.clear();
        m.body_path = body_file.string();
        expect_true(outbound::ensure_mail_raw_payload_loaded(m),
                    "2.4: valid body_path -> true");
        expect_true(m.body == "loaded from disk\r\n",
                    "2.4: body populated with file content");
    }

    // ============== Section 3: build_outbound_message ==============
    // 投递的最后一公里：把 mail 对象 → 符合 RFC 5321 的 wire 格式（含 dot-stuffing、
    // 可选 DKIM 签名）。这是剧情，不是单元测试。
    {
        std::printf("  -- build_outbound_message --\n");

        // 共享 fixture
        outbound::OutboundConfig cfg;        // DKIM off
        outbound::OutboxRecord rec;
        rec.id        = 42;
        rec.mail_id   = 1001;
        rec.sender    = "alice@scut.email";
        rec.recipient = "bob@gmail.com";
        rec.body_path = (dir / "wire_body.txt").string();

        // 3.1: hot_mail 有 body → 用 hot_mail->body，**不**读文件
        {
            std::ofstream(rec.body_path) << "FILE_BODY_SHOULD_NOT_APPEAR";
            mail hot;
            hot.body = "From: alice@scut.email\r\nTo: bob@gmail.com\r\nSubject: hi\r\n\r\nHOT_BODY\r\n";
            bool dkim_applied = false;
            std::string dkim_err, mid;
            std::string wire = outbound::build_outbound_message(
                rec, &hot, "alice@scut.email", cfg,
                &dkim_applied, &dkim_err, &mid);
            expect_true(wire.find("HOT_BODY") != std::string::npos,
                        "3.1: hot_mail body used in wire format");
            expect_true(wire.find("FILE_BODY_SHOULD_NOT_APPEAR") == std::string::npos,
                        "3.1: body_path NOT read when hot_mail->body present");
            expect_true(wire.find("\r\n.\r\n") != std::string::npos,
                        "3.1: wire ends with CRLF.CRLF (DATA terminator)");
            expect_true(!dkim_applied, "3.1: DKIM off -> dkim_applied=false");
        }

        // 3.2: hot_mail 有空 body + 有效 body_path → 退化到读文件（剧情：内存里被清空后回填）
        {
            mail hot;        // hot.body == ""
            std::ofstream(rec.body_path)
                << "From: alice@scut.email\r\nTo: bob@gmail.com\r\nSubject: hi\r\n\r\nFALLBACK_BODY\r\n";
            bool dkim_applied = false;
            std::string dkim_err, mid;
            std::string wire = outbound::build_outbound_message(
                rec, &hot, "alice@scut.email", cfg,
                &dkim_applied, &dkim_err, &mid);
            expect_true(wire.find("FALLBACK_BODY") != std::string::npos,
                        "3.2: empty hot body falls back to body_path file");
        }

        // 3.3: hot_mail 空 body + body_path 空 → 退到 synthesized minimal message
        {
            mail hot;
            rec.body_path.clear();
            bool dkim_applied = false;
            std::string dkim_err, mid;
            std::string wire = outbound::build_outbound_message(
                rec, &hot, "alice@scut.email", cfg,
                &dkim_applied, &dkim_err, &mid);
            expect_true(wire.find("Subject: Outbound relay test") != std::string::npos,
                        "3.3: synthesized message has default subject");
            expect_true(wire.find("relayed by outbound client, outbox_id=42") != std::string::npos,
                        "3.3: synthesized body mentions outbox_id from record");
            expect_true(wire.find("\r\n.\r\n") != std::string::npos,
                        "3.3: synthesized message still ends with DATA terminator");
        }

        // 3.4: dot-stuffing — RFC 5321 §4.5.2：以 "." 开头的行必须前缀化 "."
        {
            mail hot;
            hot.body =
                "From: a@scut.email\r\n"
                "To: b@gmail.com\r\n"
                "Subject: dot\r\n"
                "\r\n"
                "line one\r\n"
                ".starts with dot\r\n"          // 必须 dot-stuff 成 ".."
                "line three\r\n";
            bool dkim_applied = false;
            std::string dkim_err, mid;
            std::string wire = outbound::build_outbound_message(
                rec, &hot, "a@scut.email", cfg, &dkim_applied, &dkim_err, &mid);
            expect_true(wire.find("..starts with dot") != std::string::npos,
                        "3.4: line starting with '.' gets '..' prefix");
            expect_true(wire.find("\n.startswith with dot") == std::string::npos,
                        "3.4: original '.starts with dot' (not stuffed) NOT in wire");
        }

        // 3.5: 已有 Message-ID 时，out 为空时不重新生成
        {
            mail hot;
            hot.body =
                "From: a@scut.email\r\n"
                "To: b@gmail.com\r\n"
                "Subject: mid\r\n"
                "Message-ID: <existing.id@scut.email>\r\n"
                "\r\n"
                "body\r\n";
            std::string mid_out;
            bool dkim_applied = false;
            std::string dkim_err;
            outbound::build_outbound_message(
                rec, &hot, "a@scut.email", cfg, &dkim_applied, &dkim_err, &mid_out);
            expect_true(mid_out == "<existing.id@scut.email>",
                        "3.5: existing Message-ID preserved, not auto-generated");
        }

        // 3.6: DKIM off → wire 头部无 DKIM-Signature
        {
            mail hot;
            hot.body = "From: a@scut.email\r\nTo: b@gmail.com\r\nSubject: x\r\n\r\nbody\r\n";
            bool dkim_applied = false;
            std::string dkim_err;
            std::string wire = outbound::build_outbound_message(
                rec, &hot, "a@scut.email", cfg, &dkim_applied, &dkim_err, nullptr);
            expect_true(wire.find("DKIM-Signature") == std::string::npos,
                        "3.6: DKIM off -> no DKIM-Signature in wire");
            expect_true(!dkim_applied, "3.6: dkim_applied=false");
        }

        // 3.7: DKIM on + 缺配置 → dkim_error 填充，mail 仍正常生成（graceful degradation）
        {
            outbound::OutboundConfig dkim_cfg;
            dkim_cfg.dkim_enabled = true;
            dkim_cfg.dkim_selector = "default";
            dkim_cfg.dkim_domain = "scut.email";
            dkim_cfg.dkim_private_key_file.clear();   // 缺失关键配置

            mail hot;
            hot.body = "From: a@scut.email\r\nTo: b@gmail.com\r\nSubject: x\r\n\r\nbody\r\n";
            bool dkim_applied = true;   // 预设值，看是否被清掉
            std::string dkim_err;
            std::string wire = outbound::build_outbound_message(
                rec, &hot, "a@scut.email", dkim_cfg, &dkim_applied, &dkim_err, nullptr);
            expect_true(!dkim_applied,
                        "3.7: missing DKIM config -> dkim_applied reset to false");
            expect_true(!dkim_err.empty(),
                        "3.7: dkim_error populated with reason");
            expect_true(wire.find("\r\n.\r\n") != std::string::npos,
                        "3.7: mail still built and well-formed (graceful degradation)");
        }

        // 3.8: DKIM on + 有效 key → wire 含 DKIM-Signature 且能被验证
        {
            std::string key = dkim_key_file();
            if (!key.empty()) {
                outbound::OutboundConfig dkim_cfg;
                dkim_cfg.dkim_enabled = true;
                dkim_cfg.dkim_selector = "test";
                dkim_cfg.dkim_domain = "scut.email";
                dkim_cfg.dkim_private_key_file = key;

                mail hot;
                hot.body =
                    "From: a@scut.email\r\n"
                    "To: b@gmail.com\r\n"
                    "Subject: signed\r\n"
                    "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n"
                    "Message-ID: <signed@scut.email>\r\n"
                    "MIME-Version: 1.0\r\n"
                    "Content-Type: text/plain; charset=UTF-8\r\n"
                    "Content-Transfer-Encoding: 8bit\r\n"
                    "Reply-To: a@scut.email\r\n"
                    "\r\n"
                    "signed body\r\n";
                bool dkim_applied = false;
                std::string dkim_err;
                std::string wire = outbound::build_outbound_message(
                    rec, &hot, "a@scut.email", dkim_cfg, &dkim_applied, &dkim_err, nullptr);
                expect_true(dkim_applied,
                            "3.8: DKIM on + valid key -> dkim_applied=true");
                expect_true(dkim_err.empty() || dkim_err.find("missing") == std::string::npos,
                            "3.8: no missing-config error");
                expect_true(wire.find("DKIM-Signature:") != std::string::npos,
                            "3.8: wire contains DKIM-Signature header");
                expect_true(wire.find("d=scut.email") != std::string::npos,
                            "3.8: DKIM-Signature carries d=scut.email");
                expect_true(wire.find("s=test;") != std::string::npos,
                            "3.8: DKIM-Signature carries s=test");
                expect_true(wire.find("a=rsa-sha256") != std::string::npos,
                            "3.8: DKIM-Signature uses rsa-sha256");
                expect_true(wire.find("bh=") != std::string::npos,
                            "3.8: DKIM-Signature contains body hash (bh=)");
                expect_true(wire.find("b=") != std::string::npos,
                            "3.8: DKIM-Signature contains signature value (b=)");
            } else {
                std::printf("  [skip 3.8: openssl not available for DKIM key gen]\n");
            }
        }
    }

    std::filesystem::remove_all(dir);
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
