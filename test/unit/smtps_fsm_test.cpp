// 入站 SMTP FSM 单元测试 — MockConnection 零 I/O 验证状态转换和响应码
#undef NDEBUG
#include <cassert>  // 在 #undef NDEBUG 后重新包含，确保 assert 生效
#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/server_config.h"
#include "mail_system/back/thread_pool/io_thread_pool.h"
#include "mail_system/back/thread_pool/boost_thread_pool.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mock_connection.h"

#include <iostream>
#include <memory>
#include <string>
#include <spdlog/common.h>

using namespace mail_system;
namespace pq = persist_storage;

// ========== 测试夹具 ==========

struct TestServer : ServerBase {
    TestServer(const ServerConfig& c,
               std::shared_ptr<ThreadPoolBase> io,
               std::shared_ptr<ThreadPoolBase> w,
               std::shared_ptr<router::IShardRouter> r,
               std::shared_ptr<pq::PersistentQueue> q)
        : ServerBase(c, io, w, nullptr) {
        m_shardRouter = std::move(r);
        m_persistentQueue = std::move(q);
    }
    void handle_accept(std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>&&,
                       const boost::system::error_code&, ListenerConfig) override {}
    void handle_tcp_accept(std::unique_ptr<boost::asio::ip::tcp::socket>&&,
                           const boost::system::error_code&, ListenerConfig) override {}
    bool should_reject_connection(std::string&, const std::string&) const override { return false; }
};

struct FsmTestFixture {
    std::shared_ptr<IOThreadPool> io_pool;
    std::shared_ptr<BoostThreadPool> worker_pool;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<pq::PersistentQueue> persist_q;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalSmtpsFsm<MockConnection>> fsm;
    ServerConfig cfg;

    FsmTestFixture() {
        Logger::get_instance().init("smtps_fsm_test.log", 0, 1, spdlog::level::off, false, false);
        io_pool     = std::make_shared<IOThreadPool>(1);
        worker_pool = std::make_shared<BoostThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{});

        persist_q = std::make_shared<pq::PersistentQueue>(router, worker_pool);
        pq::PersistentQueuePressureConfig pc;
        pc.max_inflight_mails           = 1000000;
        pc.min_available_memory_mb      = 0;
        pc.min_db_available_connections = 0;
        persist_q->set_pressure_config(pc);
        persist_q->set_local_domain("test.local");

        cfg.perf_mode          = true;
        cfg.apply_perf_mode();
        cfg.use_database       = false;
        cfg.inbound_ack_mode   = InboundAckMode::AFTER_ENQUEUE;
        cfg.system_domain      = "test.local";
        cfg.mail_storage_path  = "/tmp/smtps_fsm_test_mail";
        cfg.attachment_storage_path = "/tmp/smtps_fsm_test_att";
        cfg.storage.local.mail_path       = "/tmp/smtps_fsm_test_mail";
        cfg.storage.local.attachment_path = "/tmp/smtps_fsm_test_att";

        system("mkdir -p /tmp/smtps_fsm_test_mail /tmp/smtps_fsm_test_att");

        server = std::make_shared<TestServer>(cfg, io_pool, worker_pool, router, persist_q);

        fsm = std::make_shared<TraditionalSmtpsFsm<MockConnection>>(
            io_pool, worker_pool, persist_q, router);

        // 预注入 auth 缓存 (user0~user4@test.local, 密码 test123)
        for (int i = 0; i < 5; ++i) {
            std::string email = "user" + std::to_string(i) + "@test.local";
            AuthCacheEntry entry;
            entry.password_hash = "test123";
            entry.status = 1;
            fsm->m_authCache->inject(email, entry);
        }
    }

    ~FsmTestFixture() {
        persist_q->shutdown();
        worker_pool->stop();
        io_pool->stop();
        system("rm -rf /tmp/smtps_fsm_test_mail /tmp/smtps_fsm_test_att");
    }

    // 创建 session，初始化 INIT 状态，返回 session + raw conn 指针
    struct Handle {
        MockConnection* conn;
        std::shared_ptr<SmtpsSession<MockConnection>> session;
    };

    Handle make_session(const std::string& preload_data = "",
                        InboundAuthPolicy auth_policy = InboundAuthPolicy::OFF) {
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn_ptr = conn_u.get();
        if (!preload_data.empty())
            conn_ptr->set_read_data(preload_data);

        auto session = std::make_shared<SmtpsSession<MockConnection>>(
            server.get(), std::move(conn_u),
            fsm);

        ListenerConfig lc;
        lc.type       = ListenerType::TCP;
        lc.port       = 25;
        lc.auth_policy = auth_policy;
        session->set_listener_config(lc);
        session->set_current_state(static_cast<int>(SmtpsState::INIT));
        session->set_next_event(static_cast<int>(SmtpsEvent::CONNECT));

        return {conn_ptr, session};
    }

    // 调用 process_read 启动 FSM（需预加载数据确保不会因 EOF 而提前关闭）
    void start(Handle& h) {
        h.session->process_read();
    }
};

// ========== 测试宏 ==========
#define TEST(name) \
    void test_##name(FsmTestFixture& fx)

#define HAS(str, sub) ((str).find(sub) != std::string::npos)

// ========== Happy Path ==========

TEST(greeting_220) {
    auto h = fx.make_session();
    fx.fsm->process_event(h.session, SmtpsEvent::CONNECT, "");
    auto& w = h.conn->written();
    assert(HAS(w, "220 SMTPS Server"));
    std::cout << "  [PASS] greeting_220: " << w.substr(0, w.find('\r')) << std::endl;
}

TEST(ehlo_capabilities) {
    // 预加载 EHLO，CONNECT→greeting→do_async_read→吃 EHLO→EHLO handler
    auto h = fx.make_session("EHLO mail.test.local\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "220 SMTPS Server"));
    assert(HAS(w, "250-mail.test.local Hello"));
    assert(HAS(w, "250-SIZE"));
    assert(HAS(w, "250-8BITMIME"));
    assert(HAS(w, "STARTTLS"));       // TCP 连接会宣告 STARTTLS
    assert(HAS(w, "250 SMTPUTF8"));
    std::cout << "  [PASS] ehlo_capabilities" << std::endl;
}

TEST(ehlo_with_auth_policy_on) {
    auto h = fx.make_session("EHLO test\r\n", InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "AUTH LOGIN PLAIN"));  // auth_policy=ON 时宣告 AUTH
    std::cout << "  [PASS] ehlo_with_auth_policy_on" << std::endl;
}

TEST(ehlo_with_auth_policy_off) {
    auto h = fx.make_session("EHLO test\r\n", InboundAuthPolicy::OFF);
    fx.start(h);
    auto& w = h.conn->written();
    assert(!HAS(w, "AUTH LOGIN PLAIN")); // auth_policy=OFF 不宣告 AUTH
    std::cout << "  [PASS] ehlo_with_auth_policy_off" << std::endl;
}

TEST(mail_from_ok) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@test.local>\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "250 Ok"));
    std::cout << "  [PASS] mail_from_ok" << std::endl;
}

TEST(rcpt_to_ok) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "250 Ok"));
    std::cout << "  [PASS] rcpt_to_ok" << std::endl;
}

TEST(data_354) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "354 Start mail input"));
    std::cout << "  [PASS] data_354" << std::endl;
}

TEST(full_delivery_pipeline) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: sender@test.local\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: FSM test\r\n"
        "\r\n"
        "hello smtp fsm\r\n"
        ".\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "220 SMTPS Server"));
    assert(HAS(w, "250-test Hello"));
    assert(HAS(w, "250 Ok"));       // MAIL FROM + RCPT TO
    assert(HAS(w, "354 Start mail input"));
    assert(HAS(w, "250 "));         // message accepted (queue ID)
    assert(HAS(w, "221 Bye"));      // QUIT
    std::cout << "  [PASS] full_delivery_pipeline" << std::endl;
}

TEST(multiple_rcpt) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@test.local>\r\n"
        "RCPT TO:<rcpt1@test.local>\r\n"
        "RCPT TO:<rcpt2@test.local>\r\n"
        "RCPT TO:<rcpt3@test.local>\r\n"
        "QUIT\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    // 每个 RCPT TO 都返回 250 Ok
    size_t cnt = 0;
    size_t pos = 0;
    while ((pos = w.find("250 Ok", pos)) != std::string::npos) { ++cnt; ++pos; }
    assert(cnt >= 4); // MAIL FROM + 3x RCPT TO
    std::cout << "  [PASS] multiple_rcpt (250 Ok count=" << cnt << ")" << std::endl;
}

TEST(quit_221) {
    auto h = fx.make_session("QUIT\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "221 Bye"));
    std::cout << "  [PASS] quit_221" << std::endl;
}

TEST(re_ehlo_in_wait_auth) {
    // EHLO 后再次 EHLO — 应重置状态
    auto h = fx.make_session(
        "EHLO first\r\n"
        "EHLO second\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "250-first Hello"));
    assert(HAS(w, "250-second Hello"));
    std::cout << "  [PASS] re_ehlo_in_wait_auth" << std::endl;
}

// ========== Error Path ==========

TEST(invalid_command_sequence) {
    // 不发 EHLO 直接 MAIL FROM
    auto h = fx.make_session("MAIL FROM:<x@y>\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "500 Error"));
    std::cout << "  [PASS] invalid_command_sequence" << std::endl;
}

TEST(mail_from_bad_syntax) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:bad-syntax\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "501 Syntax error"));
    std::cout << "  [PASS] mail_from_bad_syntax" << std::endl;
}

TEST(rcpt_to_bad_syntax) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@test.local>\r\n"
        "RCPT TO:bad\r\n");
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "501 Syntax error"));
    std::cout << "  [PASS] rcpt_to_bad_syntax" << std::endl;
}

// ========== AUTH Flow ==========

TEST(auth_required_rejected) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<sender@test.local>\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "530 5.7.1 Authentication required"));
    std::cout << "  [PASS] auth_required_rejected" << std::endl;
}

TEST(auth_login_username_prompt) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "334 VXNlcm5hbWU6")); // base64("Username:")
    std::cout << "  [PASS] auth_login_username_prompt" << std::endl;
}

TEST(auth_login_full_flow) {
    // AUTH LOGIN → username (base64) → password (base64) → success
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"     // base64("user0@test.local")
        "dGVzdDEyMw==\r\n",                   // base64("test123")
        InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "334 VXNlcm5hbWU6"));       // Username prompt
    assert(HAS(w, "334 UGFzc3dvcmQ6"));       // Password prompt
    assert(HAS(w, "235 Authentication successful"));
    std::cout << "  [PASS] auth_login_full_flow" << std::endl;
}

TEST(auth_login_wrong_password) {
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"     // base64("user0@test.local")
        "d3JvbmdwYXNz\r\n",                   // base64("wrongpass")
        InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "535 Authentication failed"));
    std::cout << "  [PASS] auth_login_wrong_password" << std::endl;
}

TEST(auth_failures_exceed_close) {
    // 3次密码错误 → 连接被关闭（不发535）
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"     // user0@test.local
        "d3JvbmdwYXNz\r\n"                   // wrong pass - attempt 1
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"
        "d3JvbmdwYXNz\r\n"                   // attempt 2
        "AUTH LOGIN\r\n"
        "dXNlcjBAdGVzdC5sb2NhbA==\r\n"
        "d3JvbmdwYXNz\r\n",                  // attempt 3 → close
        InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();

    size_t cnt = 0;
    size_t pos = 0;
    while ((pos = w.find("535 Authentication failed", pos)) != std::string::npos)
        { ++cnt; ++pos; }
    assert(cnt == 2);
    assert(!h.conn->is_open()); // 第3次直接关闭连接

    std::cout << "  [PASS] auth_failures_exceed_close (535 count=" << cnt << ")" << std::endl;
}

TEST(auth_plain_single_step) {
    // AUTH PLAIN with inline token: \0user0@test.local\0test123
    std::string plain;
    plain += '\0'; plain += "user0@test.local"; plain += '\0'; plain += "test123";
    std::string token = mail_system::outbound::base64_encode(
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN " + token + "\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "235 Authentication successful"));
    std::cout << "  [PASS] auth_plain_single_step" << std::endl;
}

TEST(auth_plain_two_step) {
    std::string plain2;
    plain2 += '\0'; plain2 += "user0@test.local"; plain2 += '\0'; plain2 += "test123";
    auto h = fx.make_session(
        "EHLO test\r\n"
        "AUTH PLAIN\r\n"
        + mail_system::outbound::base64_encode(
            reinterpret_cast<const unsigned char*>(plain2.data()), plain2.size()) + "\r\n",
        InboundAuthPolicy::ON);
    fx.start(h);
    auto& w = h.conn->written();
    assert(HAS(w, "334 "));                      // continue prompt
    assert(HAS(w, "235 Authentication successful"));
    std::cout << "  [PASS] auth_plain_two_step" << std::endl;
}

// ========== STARTTLS ==========

TEST(starttls) {
    std::string captured;
    auto conn_u = std::make_unique<MockConnection>();
    conn_u->capture_to(&captured);
    conn_u->set_read_data("EHLO test\r\nSTARTTLS\r\n");

    auto session = std::make_shared<SmtpsSession<MockConnection>>(
        fx.server.get(), std::move(conn_u),
        fx.fsm);

    ListenerConfig lc;
    lc.auth_policy = InboundAuthPolicy::OFF;
    session->set_listener_config(lc);
    session->set_current_state(static_cast<int>(SmtpsState::INIT));
    session->set_next_event(static_cast<int>(SmtpsEvent::CONNECT));
    session->process_read();

    // connection 已被 STARTTLS handler 释放，用 captured 断言
    assert(HAS(captured, "220 Ready to start TLS"));
    std::cout << "  [PASS] starttls" << std::endl;
}

int main() {
    std::cout << "Inbound SMTP FSM Test Suite\n===========================\n";

    try {
        FsmTestFixture fx;

        // ── Happy Path ──
        test_greeting_220(fx);
        test_ehlo_capabilities(fx);
        test_ehlo_with_auth_policy_on(fx);
        test_ehlo_with_auth_policy_off(fx);
        test_mail_from_ok(fx);
        test_rcpt_to_ok(fx);
        test_data_354(fx);
        test_full_delivery_pipeline(fx);
        test_multiple_rcpt(fx);
        test_quit_221(fx);
        test_re_ehlo_in_wait_auth(fx);

        // ── Error Path ──
        test_invalid_command_sequence(fx);
        test_mail_from_bad_syntax(fx);
        test_rcpt_to_bad_syntax(fx);

        // ── AUTH Flow ──
        test_auth_required_rejected(fx);
        test_auth_login_username_prompt(fx);
        test_auth_login_full_flow(fx);
        test_auth_login_wrong_password(fx);
        test_auth_failures_exceed_close(fx);
        test_auth_plain_single_step(fx);
        test_auth_plain_two_step(fx);

        // ── STARTTLS ──
        test_starttls(fx);

        std::cout << "\nAll tests passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
