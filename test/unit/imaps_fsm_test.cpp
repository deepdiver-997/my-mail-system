// IMAP FSM 单元测试 — MockConnection 零 I/O 验证状态转换和响应码
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include "mail_system/back/mailServer/imaps_server.h"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mock_connection.h"

#include <iostream>
#include <memory>
#include <string>
#include <spdlog/common.h>

using namespace mail_system;

// ========== 测试夹具 ==========

struct TestServer : ServerBase {
    TestServer(const ServerConfig& c,
               std::shared_ptr<ThreadPoolBase> io,
               std::shared_ptr<ThreadPoolBase> w,
               std::shared_ptr<router::IShardRouter> r)
        : ServerBase(c, io, w, nullptr) {
        m_shardRouter = std::move(r);
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
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalImapsFsm<MockConnection>> fsm;
    ServerConfig cfg;

    FsmTestFixture() {
        Logger::get_instance().init("imaps_fsm_test.log", 0, 1, spdlog::level::off, false, false);
        io_pool     = std::make_shared<IOThreadPool>(1);
        worker_pool = std::make_shared<BoostThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{});

        cfg.perf_mode     = true;
        cfg.apply_perf_mode();
        cfg.use_database   = false;
        cfg.system_domain  = "test.local";
        cfg.storage.local.mail_path       = "/tmp/imaps_fsm_test_mail";
        cfg.storage.local.attachment_path = "/tmp/imaps_fsm_test_att";
        system("mkdir -p /tmp/imaps_fsm_test_mail /tmp/imaps_fsm_test_att");

        server = std::make_shared<TestServer>(cfg, io_pool, worker_pool, router);

        fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(
            io_pool, worker_pool, router);

        // 预注入 auth 缓存
        for (int i = 0; i < 3; ++i) {
            std::string email = "user" + std::to_string(i) + "@test.local";
            AuthCacheEntry entry;
            entry.password_hash = "test123";
            entry.status = 1;
            fsm->m_authCache->inject(email, entry);
        }
    }

    ~FsmTestFixture() {
        worker_pool->stop();
        io_pool->stop();
        system("rm -rf /tmp/imaps_fsm_test_mail /tmp/imaps_fsm_test_att");
    }

    struct Handle {
        MockConnection* conn;
        std::shared_ptr<ImapsSession<MockConnection>> session;
    };

    Handle make_session(const std::string& preload_data = "") {
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn_ptr = conn_u.get();
        if (!preload_data.empty())
            conn_ptr->set_read_data(preload_data);

        auto session = std::make_shared<ImapsSession<MockConnection>>(
            server.get(), std::move(conn_u),
            fsm);

        return {conn_ptr, session};
    }
};

#define TEST(name) void test_##name(FsmTestFixture& fx)
#define HAS(str, sub) ((str).find(sub) != std::string::npos)

// ========== 基础命令 ==========

TEST(capability_response) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CAPABILITY, "A001");
    auto& w = h.conn->written();
    assert(HAS(w, "CAPABILITY IMAP4rev1"));
    std::cout << "  [PASS] capability_response" << std::endl;
}

TEST(noop_reply) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "C001";
    fx.fsm->process_event(h.session, ImapEvent::NOOP, "C001");
    auto& w = h.conn->written();
    assert(HAS(w, "C001 OK NOOP completed"));
    std::cout << "  [PASS] noop_reply" << std::endl;
}

TEST(login_success) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGIN, "A001");
    auto& w = h.conn->written();
    assert(HAS(w, "A001 OK LOGIN completed"));
    std::cout << "  [PASS] login_success" << std::endl;
}

TEST(login_wrong_password) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGIN, "A002");
    auto& w = h.conn->written();
    assert(HAS(w, "A002 NO LOGIN failed"));
    std::cout << "  [PASS] login_wrong_password" << std::endl;
}

TEST(login_many_failures_close) {
    // 预加载3个失败LOGIN，pipeline一次性处理
    auto h = fx.make_session(
        "A001 LOGIN user0@test.local wrong\r\n"
        "A002 LOGIN user0@test.local wrong\r\n"
        "A003 LOGIN user0@test.local wrong\r\n");
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    h.session->process_read();
    auto& w = h.conn->written();
    size_t cnt = 0, pos = 0;
    while ((pos = w.find("NO LOGIN failed", pos)) != std::string::npos) { ++cnt; ++pos; }
    assert(cnt == 2);
    assert(!h.conn->is_open());
    std::cout << "  [PASS] login_many_failures_close (NO count=" << cnt << ")" << std::endl;
}

TEST(logout_bye) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "B001";
    fx.fsm->process_event(h.session, ImapEvent::LOGOUT, "B001");
    auto& w = h.conn->written();
    assert(HAS(w, "BYE IMAP4rev1 Server logging out"));
    assert(HAS(w, "B001 OK LOGOUT completed"));
    std::cout << "  [PASS] logout_bye" << std::endl;
}

// ========== 未认证状态拒绝 ==========

TEST(select_without_login) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::SELECT, "D001");
    auto& w = h.conn->written();
    assert(HAS(w, "D001 NO") || HAS(w, "BAD"));
    std::cout << "  [PASS] select_without_login" << std::endl;
}

TEST(fetch_without_login) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::FETCH, "E001");
    auto& w = h.conn->written();
    assert(HAS(w, "E001 NO") || HAS(w, "BAD"));
    std::cout << "  [PASS] fetch_without_login" << std::endl;
}

// ========== UID FETCH crash repro ==========

TEST(uid_fetch_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->user_id = 1;
    ctx->mailbox_selected = true;
    ctx->selected_mailbox_id = 1;
    ctx->current_tag = "A001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));

    // 直接调用 UID → FETCH handler（模拟客户端发 UID FETCH 1:* (FLAGS)）
    fx.fsm->process_event(h.session, ImapEvent::UID, "A001");
    auto& w = h.conn->written();
    std::cout << "  [PASS] uid_fetch_no_db response=[" << w.substr(0, 80) << "]" << std::endl;
}

// ========== 命令顺序错误 ==========

TEST(invalid_command_in_state) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::STORE, "H001");
    auto& w = h.conn->written();
    assert(HAS(w, "H001 BAD") || HAS(w, "H001 NO"));
    std::cout << "  [PASS] invalid_command_in_state" << std::endl;
}

int main() {
    std::cout << "IMAP FSM Test Suite\n==================\n";

    try {
        FsmTestFixture fx;

        // ── 基础命令 ──
        test_capability_response(fx);
        test_login_success(fx);
        test_login_wrong_password(fx);
        test_login_many_failures_close(fx);
        test_noop_reply(fx);
        test_logout_bye(fx);

        // ── 未认证拒绝 ──
        test_select_without_login(fx);
        test_fetch_without_login(fx);

        // ── UID FETCH crash repro ──
        test_uid_fetch_no_db(fx);

        // ── 命令错误 ──
        test_invalid_command_in_state(fx);

        std::cout << "\nAll tests passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
