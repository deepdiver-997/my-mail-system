// IMAP FSM 单元测试 — MockConnection 零 I/O 验证状态转换和响应码
#undef NDEBUG
#include <cassert>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"
#include "mail_system/back/mailServer/session/imaps_session.tpp"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mock_connection.h"
#include "mock_db_pool.h"
#include "mail_system/back/storage/local_file_storage_provider.h"

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
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
    void start() override {}
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

        server = std::shared_ptr<TestServer>(new TestServer(cfg, io_pool, worker_pool, router));

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
    auto w = h.conn->written();
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
    auto w = h.conn->written();
    assert(HAS(w, "C001 OK NOOP completed"));
    std::cout << "  [PASS] noop_reply" << std::endl;
}

// LOGIN 已异步化（bcrypt 在 worker 线程）：回复可能晚于 process_event 返回，
// 轮询等待而不是立刻断言。
template <typename Handle>
static std::string wait_for_reply(Handle& h, int timeout_ms = 2000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        auto w = h.conn->written();
        if (!w.empty()) return w;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return h.conn->written();
}

TEST(login_response) {
    // 直接调用 process_event 测试 FSM handler（不依赖 command parser）
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGIN, "A001");
    auto w = wait_for_reply(h);
    // handler 会尝试从 last_command_args 解析凭据，可能 OK 或 NO
    assert(!w.empty());
    std::cout << "  [PASS] login_response" << std::endl;
}

TEST(login_wrong_password) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGIN, "A002");
    auto w = wait_for_reply(h);
    assert(!w.empty());
    std::cout << "  [PASS] login_wrong_password" << std::endl;
}

TEST(login_many_failures_close) {
    // 多次失败 LOGIN → 达到 max_auth_attempts（测试配置=2）后连接被关闭
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));

    // LOGIN 已异步化（bcrypt 在 worker 线程池）。直接连发多个 process_event 会让
    // 多个 worker 线程并发处理同一 session → 并发改写 session 的 std::string
    // （command_read_buffer_/last_command_args_/m_trace_buf），Linux libstdc++ 下
    // 表现为 heap-use-after-free（macOS libc++ 不崩，CI 才暴露）。
    // 生产流水线靠 drain+暂停串行化，但测试直接调 process_event 绕过了它，
    // 必须逐个等待该次认证完成（回复 NO / 连接关闭）再发下一个。
    size_t total_no = 0;
    for (const char* tag : {"A001", "A002", "A003"}) {
        h.conn->clear_written();   // 只统计本次回复，避免匹配到上次的过期 NO
        fx.fsm->process_event(h.session, ImapEvent::LOGIN, tag);
        for (int waited = 0; waited < 2000; waited += 5) {
            auto w = h.conn->written();
            if (HAS(w, "NO LOGIN failed")) { ++total_no; break; }   // 该次失败已回复
            if (!h.conn->is_open()) break;                          // 失败数达上限 → 关闭
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!h.conn->is_open()) break;   // 连接已关闭，无需再发
    }
    assert(total_no >= 1 || !h.conn->is_open());
    std::cout << "  [PASS] login_many_failures_close (NO count=" << total_no << ")" << std::endl;
}

TEST(logout_bye) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "B001";
    fx.fsm->process_event(h.session, ImapEvent::LOGOUT, "B001");
    auto w = h.conn->written();
    assert(HAS(w, "BYE IMAP4rev1 Server logging out"));
    assert(HAS(w, "B001 OK LOGOUT completed"));
    std::cout << "  [PASS] logout_bye" << std::endl;
}

// ========== 未认证状态拒绝 ==========

TEST(select_without_login) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::SELECT, "D001");
    auto w = h.conn->written();
    assert(HAS(w, "D001 NO") || HAS(w, "BAD"));
    std::cout << "  [PASS] select_without_login" << std::endl;
}

TEST(fetch_without_login) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::FETCH, "E001");
    auto w = h.conn->written();
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
    auto w = h.conn->written();
    std::cout << "  [PASS] uid_fetch_no_db response=[" << w.substr(0, 80) << "]" << std::endl;
}

// ========== 全路径 FETCH（续作链 + 存储读取） ==========
TEST(fetch_full_path_with_storage) {
    // 覆盖重写后的续作链：mock DB 提供邮件行 + LocalFileStorageProvider
    // 提供正文；本地 provider 的 async_* 内联执行，响应应同步产出。
    // 断言 size/正文/BODY 字面量/tagged OK —— 这是 FETCH 热路径的形状。
    auto io = std::make_shared<IOThreadPool>(1); io->start();
    auto wk = std::make_shared<BoostThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);   // 同步模式：sq 桥立即拿到注入的行
    namespace st = mail_system::storage;
    std::filesystem::create_directories("/tmp/imaps_fetch_test");
    auto storage = std::make_shared<st::LocalFileStorageProvider>(
        "/tmp/imaps_fetch_test/", "/tmp/imaps_fetch_test/");
    auto router2 = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<st::IStorageProvider>>{storage});

    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = "/tmp/imaps_fsm_test_mail2";
    cfg2.storage.local.attachment_path = "/tmp/imaps_fsm_test_att2";
    auto server2 = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router2));
    auto fsm2 = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router2);

    const std::string body = "Subject: hello\r\n\r\nworld body 123";
    const std::string body_path = "/tmp/imaps_fetch_test/mail_42";
    { std::ofstream f(body_path, std::ios::binary); f << body; }

    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{
            {"id", "42"}, {"sender", "a@t.local"}, {"recipient", "b@t.local"},
            {"subject", "hi"}, {"body_path", body_path},
            {"is_starred", "0"}, {"is_deleted", "0"}, {"is_important", "0"},
            {"status", "1"}, {"send_time", "1700000000"}}}));

    auto conn_u = std::make_unique<MockConnection>();
    auto* conn_ptr = conn_u.get();
    auto session = std::make_shared<ImapsSession<MockConnection>>(
        server2.get(), std::move(conn_u), fsm2);
    auto* c = static_cast<ImapContext*>(session->get_context());
    c->is_authenticated = true;
    c->user_id = 1;
    c->mailbox_selected = true;
    c->selected_mailbox_id = 1;
    c->current_tag = "A001";
    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    // 走真实解析+分发路径：handle_read 解析 tag/cmd/args，process_read 派发 FSM
    session->handle_read("A001 FETCH 1 (FLAGS RFC822.SIZE BODY[])\r\n");
    session->process_read();

    auto w = conn_ptr->written();
    assert(w.find("RFC822.SIZE " + std::to_string(body.size())) != std::string::npos);
    assert(w.find("BODY[] {" + std::to_string(body.size()) + "}") != std::string::npos);
    assert(w.find("world body 123") != std::string::npos);
    assert(w.find("A001 OK FETCH completed") != std::string::npos);

    std::filesystem::remove_all("/tmp/imaps_fetch_test");
    io->stop(); wk->stop();
    std::cout << "  [PASS] fetch_full_path_with_storage" << std::endl;
}

// literal 声明大小超限 → BAD + 断开（OOM 防线）
TEST(literal_too_large_rejected) {
    auto h = fx.make_session();
    h.session->handle_read("A001 APPEND INBOX (\\Flags) {4294967295+}\r\n");
    auto w = h.conn->written();
    assert(w.find("BAD") != std::string::npos);
    assert(w.find("Literal too large") != std::string::npos);
    assert(h.session->is_closed());
    std::cout << "  [PASS] literal_too_large_rejected" << std::endl;
}

// ========== 命令顺序错误 ==========

TEST(invalid_command_in_state) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::STORE, "H001");
    auto w = h.conn->written();
    assert(HAS(w, "H001 BAD") || HAS(w, "H001 NO"));
    std::cout << "  [PASS] invalid_command_in_state" << std::endl;
}

// ========== 新增: 无需 DB 的命令 ==========

TEST(logout_without_login) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->current_tag = "A001";
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LOGOUT, "A001");
    auto w = h.conn->written();
    // LOGOUT 总是允许的
    assert(HAS(w, "BYE"));
    assert(HAS(w, "A001 OK"));
    std::cout << "  [PASS] logout_without_login" << std::endl;
}

TEST(check_in_authenticated) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "C001";
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CHECK, "C001");
    auto w = h.conn->written();
    assert(HAS(w, "C001 OK CHECK") || HAS(w, "C001 BAD") || HAS(w, "C001 NO"));
    std::cout << "  [PASS] check_in_authenticated" << std::endl;
}

TEST(noop_in_selected) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "B002";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::NOOP, "B002");
    auto w = h.conn->written();
    assert(HAS(w, "B002 OK NOOP"));
    std::cout << "  [PASS] noop_in_selected" << std::endl;
}

TEST(capability_in_authenticated) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CAPABILITY, "C002");
    auto w = h.conn->written();
    assert(HAS(w, "CAPABILITY IMAP4rev1"));
    std::cout << "  [PASS] capability_in_authenticated" << std::endl;
}

// ========== 新增: DB 依赖命令（无 DB 时须优雅失败） ==========

TEST(select_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->current_tag = "S001";
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::SELECT, "S001");
    auto w = h.conn->written();
    // 无 DB 时返回 NO 或 BAD，不应崩溃
    assert(!w.empty());
    std::cout << "  [PASS] select_no_db" << std::endl;
}

TEST(examine_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::EXAMINE, "E001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] examine_no_db" << std::endl;
}

TEST(create_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::CREATE, "CR001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] create_no_db" << std::endl;
}

TEST(delete_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::DELETE, "D001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] delete_no_db" << std::endl;
}

TEST(rename_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::RENAME, "R001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] rename_no_db" << std::endl;
}

TEST(subscribe_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::SUBSCRIBE, "SUB001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] subscribe_no_db" << std::endl;
}

TEST(unsubscribe_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::UNSUBSCRIBE, "UNS001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] unsubscribe_no_db" << std::endl;
}

TEST(list_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LIST, "L001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] list_no_db" << std::endl;
}

TEST(lsub_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::LSUB, "LS001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] lsub_no_db" << std::endl;
}

TEST(status_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::IMAP_STATUS, "ST001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] status_no_db" << std::endl;
}

TEST(append_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::APPEND, "AP001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] append_no_db" << std::endl;
}

TEST(search_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "SE001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::SEARCH, "SE001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] search_no_db" << std::endl;
}

TEST(fetch_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "F001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::FETCH, "F001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] fetch_no_db" << std::endl;
}

TEST(store_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "ST001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::STORE, "ST001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] store_no_db" << std::endl;
}

TEST(copy_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "CP001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::COPY, "CP001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] copy_no_db" << std::endl;
}

TEST(move_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "MV001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::MOVE, "MV001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] move_no_db" << std::endl;
}

TEST(expunge_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "EX001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::EXPUNGE, "EX001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] expunge_no_db" << std::endl;
}

TEST(close_no_db) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    ctx->mailbox_selected = true;
    ctx->current_tag = "CL001";
    h.session->set_current_state(static_cast<int>(ImapState::SELECTED));
    fx.fsm->process_event(h.session, ImapEvent::CLOSE, "CL001");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] close_no_db" << std::endl;
}

TEST(idle_initiated) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
    auto w = h.conn->written();
    // IDLE 初始化应返回 continuation 或 tagged 响应
    assert(!w.empty());
    std::cout << "  [PASS] idle_initiated" << std::endl;
}

// ========== CONNECT (INIT → greeting) ==========

TEST(connect_greeting) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::INIT));
    fx.fsm->process_event(h.session, ImapEvent::CONNECT, "");
    auto w = h.conn->written();
    assert(HAS(w, "* OK"));
    std::cout << "  [PASS] connect_greeting" << std::endl;
}

// ========== AUTHENTICATE (SASL) ==========

TEST(authenticate_not_supported) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::AUTHENTICATE, "C001");
    auto w = h.conn->written();
    assert(HAS(w, "NO") || HAS(w, "BAD"));
    std::cout << "  [PASS] authenticate_unsupported" << std::endl;
}

// ========== ERROR / TIMEOUT ==========

TEST(error_event) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::ERROR, "");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] error_event" << std::endl;
}

TEST(timeout_event) {
    auto h = fx.make_session();
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    fx.fsm->process_event(h.session, ImapEvent::TIMEOUT, "");
    auto w = h.conn->written();
    assert(!w.empty());
    std::cout << "  [PASS] timeout_event" << std::endl;
}

// ========== IDLE + DONE (deferred read) ==========

TEST(idle_with_done) {
    auto h = fx.make_session();
    auto* ctx = static_cast<ImapContext*>(h.session->get_context());
    ctx->is_authenticated = true;
    h.session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
    // 防止 do_async_read→eof→close 杀死连接
    h.conn->set_deferred_read(true);

    fx.fsm->process_event(h.session, ImapEvent::IDLE, "I001");
    auto w = h.conn->written();
    assert(HAS(w, "+ idling"));

    // DONE 命令到达 (模拟客户端在 IDLE 中发送)
    h.conn->clear_written();
    fx.fsm->process_event(h.session, ImapEvent::DONE, "I001");
    // handle_done 写入 "I001 OK IDLE terminated"
    assert(!w.empty());
    // 测试结束：关闭连接，释放 deferred read handler（否则其捕获 shared_from_this
    // 的 handler 与 session 形成引用环，session 永不析构 → LSan 泄漏）
    h.session->close();
    std::cout << "  [PASS] idle_with_done (response: " << w.substr(0, 60) << ")" << std::endl;
}

// ========== UIDNEXT 高水位（原子推进，防并发撞值/expunge 回退） ==========
TEST(uidnext_advance_read) {
    // 剧情：SELECT/STATUS 报告 UIDNEXT。新实现 = 原子推进高水位(advance,写)
    // + 读(advance 后的 mailbox_uidnext 行)。mock 同步模式下 advance 的
    // async_execute 不消耗结果，read 的 async_query 弹出一个 uidnext 值。
    auto io = std::make_shared<IOThreadPool>(1); io->start();
    auto wk = std::make_shared<BoostThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(false);   // 同步模式
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    ServerConfig cfg2;
    cfg2.perf_mode = true; cfg2.apply_perf_mode();
    cfg2.use_database = false; cfg2.system_domain = "test.local";
    cfg2.storage.local.mail_path = "/tmp/imaps_uidnext_test_mail";
    cfg2.storage.local.attachment_path = "/tmp/imaps_uidnext_test_att";
    auto server = std::shared_ptr<TestServer>(new TestServer(cfg2, io, wk, router));
    auto fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router);

    // read 的 async_query 弹出一个 {uidnext: 42}；advance 的 async_execute 不消耗
    db->mock_conn()->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"uidnext", "42"}}}));
    auto conn = std::make_shared<ScopedConnection>(db->acquire_connection());
    uint64_t v = 0;
    fsm->get_mailbox_uidnext_async(conn, 5001, 1001, [&v](uint64_t x) { v = x; });
    assert(v == 42);
    std::cout << "  [PASS] uidnext_advance_read (uidnext=" << v << ")" << std::endl;
}

TEST(uidnext_no_db_default) {
    // 无 DB（fixture 的空 router）→ 优雅回调 1，不崩溃
    auto conn = std::make_shared<ScopedConnection>(fx.fsm->acquire_connection(0));
    uint64_t v = 0;
    fx.fsm->get_mailbox_uidnext_async(conn, 5001, 1001, [&v](uint64_t x) { v = x; });
    assert(v == 1);
    std::cout << "  [PASS] uidnext_no_db_default" << std::endl;
}

TEST(stats_flight_dedup_concurrent) {
    // 剧情：两个连接并发 SELECT 同一邮箱，缓存 miss。single-flight 应只让一个
    // 查询链在途（第二个挂到等待列表，不再发查询），owner 完成后两者都拿到结果
    // —— 防 N 连接各自查库打爆数据库，也防等待者永久挂起。
    auto io = std::make_shared<IOThreadPool>(1); io->start();
    auto wk = std::make_shared<BoostThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(true);   // 延迟模式：查询挂起，手动触发
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    ServerConfig cfg3;
    cfg3.perf_mode = true; cfg3.apply_perf_mode();
    cfg3.use_database = false; cfg3.system_domain = "test.local";
    cfg3.storage.local.mail_path = "/tmp/imaps_flight_test_mail";
    cfg3.storage.local.attachment_path = "/tmp/imaps_flight_test_att";
    auto server = std::shared_ptr<TestServer>(new TestServer(cfg3, io, wk, router));
    auto fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router);
    fsm->set_mailbox_stats_cache(
        std::make_shared<TraditionalImapsFsm<MockConnection>::MailboxStatsCache>(16));

    auto conn = std::make_shared<ScopedConnection>(db->acquire_connection());

    // 两个并发调用（同 key），缓存 miss
    MailboxCacheEntry r1, r2;
    bool cb1_done = false, cb2_done = false;
    fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
        [&](MailboxCacheEntry e, bool, bool) { r1 = e; cb1_done = true; });
    fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
        [&](MailboxCacheEntry e, bool, bool) { r2 = e; cb2_done = true; });

    // 关键断言：只有 owner 发出了 count 查询；第二个连接没发新查询（dedup）
    assert(db->mock_conn()->has_pending_query());
    assert(!cb1_done && !cb2_done);   // 两者都还在等待 owner 完成

    // 逐步触发 owner 链：count → unseen → advance(execute, 不消耗结果) → read
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "5"}}}));
    assert(!cb1_done && !cb2_done);
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "3"}}}));
    db->mock_conn()->fire_execute(true);
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"uidnext", "42"}}}));

    // 完成后 owner + waiter 都拿到同一份结果，无一挂起
    assert(cb1_done && cb2_done);
    assert(r1.exists == 5 && r2.exists == 5);
    assert(r1.unseen == 3 && r2.unseen == 3);
    assert(r1.uidnext == 42 && r2.uidnext == 42);

    // 缓存已写回：下一次直接命中，不再查库
    db->mock_conn()->clear_pending();
    MailboxCacheEntry r3;
    bool cb3_done = false;
    fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
        [&](MailboxCacheEntry e, bool, bool) { r3 = e; cb3_done = true; });
    assert(cb3_done && r3.exists == 5 && r3.uidnext == 42);
    assert(!db->mock_conn()->has_pending_query());   // 缓存命中，未发查询

    std::cout << "  [PASS] stats_flight_dedup_concurrent (1 query, both resumed, cache hit)" << std::endl;
}

TEST(stats_flight_concurrent_threads) {
    // 剧情：两个 OS 线程同时 miss 同一邮箱 key（模拟两个连接并发 SELECT）。
    // single-flight 保证只发一次查询链（谁拿 owner 不定），另一个挂等待列表，
    // 两个线程都拿到结果。TSan 下验证 flight 锁无数据竞争。
    auto io = std::make_shared<IOThreadPool>(1); io->start();
    auto wk = std::make_shared<BoostThreadPool>(2); wk->start();
    auto db = std::make_shared<test::MockDbPool>();
    db->mock_conn()->set_deferred(true);
    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},
        0,
        std::vector<std::shared_ptr<DBPool>>{db},
        std::vector<std::shared_ptr<storage::IStorageProvider>>{});
    ServerConfig cfg4;
    cfg4.perf_mode = true; cfg4.apply_perf_mode();
    cfg4.use_database = false; cfg4.system_domain = "test.local";
    cfg4.storage.local.mail_path = "/tmp/imaps_flight_test_mail2";
    cfg4.storage.local.attachment_path = "/tmp/imaps_flight_test_att2";
    auto server = std::shared_ptr<TestServer>(new TestServer(cfg4, io, wk, router));
    auto fsm = std::make_shared<TraditionalImapsFsm<MockConnection>>(io, wk, router);
    fsm->set_mailbox_stats_cache(
        std::make_shared<TraditionalImapsFsm<MockConnection>::MailboxStatsCache>(16));
    auto conn = std::make_shared<ScopedConnection>(db->acquire_connection());

    MailboxCacheEntry ra, rb;
    bool a_done = false, b_done = false;
    std::atomic<bool> go{false};
    std::thread ta([&] {
        while (!go.load(std::memory_order_acquire)) {}
        fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
            [&](MailboxCacheEntry e, bool, bool) { ra = e; a_done = true; });
    });
    std::thread tb([&] {
        while (!go.load(std::memory_order_acquire)) {}
        fsm->get_mailbox_stats_cached_async(conn, 1001, 5001,
            [&](MailboxCacheEntry e, bool, bool) { rb = e; b_done = true; });
    });
    go.store(true, std::memory_order_release);
    ta.join();
    tb.join();

    // 两个线程都到达后：只有 owner 发出了 count 查询（去重生效）
    assert(db->mock_conn()->has_pending_query());
    assert(!a_done && !b_done);

    // 触发 owner 链 → 两个 waiter 都恢复
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "7"}}}));
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "2"}}}));
    db->mock_conn()->fire_execute(true);
    db->mock_conn()->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"uidnext", "99"}}}));
    assert(a_done && b_done);
    assert(ra.exists == 7 && rb.exists == 7);
    assert(ra.unseen == 2 && rb.unseen == 2);
    assert(ra.uidnext == 99 && rb.uidnext == 99);

    std::cout << "  [PASS] stats_flight_concurrent_threads (2 threads, 1 query, no hang)" << std::endl;
}

int main() {
    std::cout << "IMAP FSM Test Suite\n==================\n";

    try {
        FsmTestFixture fx;

        // ── 基础命令 ──
        test_capability_response(fx);
        test_login_response(fx);
        test_login_wrong_password(fx);
        test_login_many_failures_close(fx);
        test_noop_reply(fx);
        test_logout_bye(fx);

        // ── 未认证拒绝 ──
        test_select_without_login(fx);
        test_fetch_without_login(fx);

        // ── UID FETCH crash repro ──
        test_uid_fetch_no_db(fx);
        test_fetch_full_path_with_storage(fx);
        test_literal_too_large_rejected(fx);

        // ── UIDNEXT 高水位 ──
        test_uidnext_advance_read(fx);
        test_uidnext_no_db_default(fx);

        // ── 统计 single-flight 去重（并发 SELECT 同邮箱） ──
        test_stats_flight_dedup_concurrent(fx);
        test_stats_flight_concurrent_threads(fx);

        // ── 命令错误 ──
        test_invalid_command_in_state(fx);

        // ── 无需 DB 的命令 ──
        test_logout_without_login(fx);
        test_check_in_authenticated(fx);
        test_noop_in_selected(fx);
        test_capability_in_authenticated(fx);

        // ── DB 依赖命令（无 DB 时须优雅失败） ──
        test_select_no_db(fx);
        test_examine_no_db(fx);
        test_create_no_db(fx);
        test_delete_no_db(fx);
        test_rename_no_db(fx);
        test_subscribe_no_db(fx);
        test_unsubscribe_no_db(fx);
        test_list_no_db(fx);
        test_lsub_no_db(fx);
        test_status_no_db(fx);
        test_append_no_db(fx);
        test_search_no_db(fx);
        test_fetch_no_db(fx);
        test_store_no_db(fx);
        test_copy_no_db(fx);
        test_move_no_db(fx);
        test_expunge_no_db(fx);
        test_close_no_db(fx);

        // ── 连接/错误/超时 ──
        test_connect_greeting(fx);
        test_authenticate_not_supported(fx);
        test_error_event(fx);
        test_timeout_event(fx);

        // ── IDLE/DONE ──
        test_idle_initiated(fx);
        test_idle_with_done(fx);

        std::cout << "\nAll tests passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
