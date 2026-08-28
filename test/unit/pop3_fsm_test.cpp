// POP3 FSM 单元测试 — MockConnection 零 I/O 验证状态转换和响应
// 覆盖 11 命令 + 状态错 + 大小写 + 未知命令 + 锁 + dot-stuffing。
// 仿 smtps_fsm_test.cpp / imaps_fsm_test.cpp 的夹具模式。
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.tpp"
#include "mail_system/back/mailServer/session/pop3_session.tpp"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mock_connection.h"
#include "mock_db_pool.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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
    static constexpr const char* MAIL_DIR = "/tmp/pop3_fsm_test_mail";
    static constexpr const char* ATT_DIR  = "/tmp/pop3_fsm_test_att";

    std::shared_ptr<IOThreadPool> io_pool;
    std::shared_ptr<BoostThreadPool> worker_pool;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<test::MockDbPool> db_pool;
    std::shared_ptr<test::MockDbConnection> db_conn;
    std::shared_ptr<storage::LocalFileStorageProvider> storage;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalPop3Fsm<MockConnection>> fsm;
    ServerConfig cfg;

    FsmTestFixture() {
        Logger::get_instance().init("pop3_fsm_test.log", 0, 1, spdlog::level::off, false, false);
        io_pool     = std::make_shared<IOThreadPool>(1);
        worker_pool = std::make_shared<BoostThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        db_pool = std::make_shared<test::MockDbPool>();
        db_conn = db_pool->mock_conn();
        db_conn->set_deferred(false);   // 同步模式：async_query 回调在返回前触发（sq sync-bridge）

        storage = std::make_shared<storage::LocalFileStorageProvider>(MAIL_DIR, ATT_DIR);
        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{db_pool},
            std::vector<std::shared_ptr<storage::IStorageProvider>>{storage});

        cfg.perf_mode    = true;
        cfg.apply_perf_mode();
        cfg.use_database = false;
        cfg.system_domain = "test.local";
        cfg.storage.local.mail_path       = MAIL_DIR;
        cfg.storage.local.attachment_path = ATT_DIR;

        std::filesystem::create_directories(MAIL_DIR);
        std::filesystem::create_directories(ATT_DIR);
        // 两封邮件的 body 文件；1002 首行以 '.' 开头，验 dot-stuffing
        {
            std::ofstream f1(std::string(MAIL_DIR) + "/1001", std::ios::binary);
            f1 << "hello from mail 1001\n";
        }
        {
            std::ofstream f2(std::string(MAIL_DIR) + "/1002", std::ios::binary);
            f2 << ".dot line\nnormal line\n";
        }

        server = std::shared_ptr<TestServer>(new TestServer(cfg, io_pool, worker_pool, router));
        fsm = std::make_shared<TraditionalPop3Fsm<MockConnection>>(
            io_pool, worker_pool, router);

        // 注入 auth 缓存（明文 test123，不走 DB）
        {
            AuthCacheEntry e;
            e.password_hash = "test123";
            e.status = 1;
            e.user_id = 1001;
            e.shard = 0;
            fsm->m_authCache->inject("alice@test.local", e);
        }
        {
            AuthCacheEntry e;
            e.password_hash = "test123";
            e.status = 1;
            e.user_id = 1002;
            e.shard = 0;
            fsm->m_authCache->inject("bob@test.local", e);
        }
    }

    ~FsmTestFixture() {
        worker_pool->stop();
        io_pool->stop();
        std::filesystem::remove_all(MAIL_DIR);
        std::filesystem::remove_all(ATT_DIR);
    }

    struct Handle {
        MockConnection* conn;
        std::shared_ptr<Pop3Session<MockConnection>> session;
        // deferred read 会让 greeting 之后的 do_async_read 挂起一个 pending handler
        // （捕获 shared_from_this），若不关闭会形成引用环 → LSan 泄漏。析构幂等关闭。
        ~Handle() {
            if (session && !session->is_closed()) {
                session->set_trace_clean_close();   // 单测无需落 trace
                session->close();
            }
        }
    };

    Handle make_session() {
        auto conn_u = std::make_unique<MockConnection>();
        auto* conn_ptr = conn_u.get();
        // 关键：deferred read 让 async_read 无数据时挂起而非返回 EOF，
        // 否则 greeting 写完成后的 do_async_read 读到空缓冲 → EOF → 会话被提前关闭。
        conn_ptr->set_deferred_read(true);
        auto session = std::make_shared<Pop3Session<MockConnection>>(
            server.get(), std::move(conn_u), fsm);
        // 发 +OK banner → AUTHORIZATION
        fsm->process_event(session, Pop3Event::CONNECT);
        return {conn_ptr, session};
    }

    // 喂一条命令（模拟 read loop 的 extract_one_line → handle_read → process_read）
    void cmd(Handle& h, const std::string& line) {
        h.session->handle_read(line + "\r\n");
        h.session->process_read();
    }

    // 播种 DB：成功 PASS 依次消费 3 个查询结果（inbox_id → lock verify → mails）
    void seed_inbox(uint64_t inbox_id = 5001) {
        db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"id", std::to_string(inbox_id)}}}));
        db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{{{"cnt", "1"}}}));
        db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
            std::vector<std::map<std::string, std::string>>{
                {{"id", "1001"}, {"body_path", std::string(MAIL_DIR) + "/1001"}},
                {{"id", "1002"}, {"body_path", std::string(MAIL_DIR) + "/1002"}},
            }));
    }

    // 登录到 TRANSACTION
    Handle login() {
        auto h = make_session();
        cmd(h, "USER alice@test.local");
        seed_inbox();
        cmd(h, "PASS test123");
        wait_for(h, "+OK Mailbox locked and loaded");
        return h;
    }

    template <typename H>
    static bool wait_for(H& h, const std::string& needle, int timeout_ms = 2000) {
        for (int waited = 0; waited < timeout_ms; waited += 5) {
            if (h.conn->written().find(needle) != std::string::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return h.conn->written().find(needle) != std::string::npos;
    }

    template <typename H>
    static bool wait_closed(H& h, int timeout_ms = 3000) {
        for (int waited = 0; waited < timeout_ms; waited += 5) {
            if (h.session->is_closed()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return h.session->is_closed();
    }
};

#define TEST(name) void test_##name(FsmTestFixture& fx)
#define HAS(str, sub) ((str).find(sub) != std::string::npos)

// ========== AUTHORIZATION 阶段 ==========

TEST(greeting) {
    auto h = fx.make_session();
    auto w = h.conn->written();
    assert(HAS(w, "+OK"));
    assert(HAS(w, "POP3 server ready"));
    std::cout << "  [PASS] greeting: " << w.substr(0, w.find('\r')) << std::endl;
}

TEST(capa) {
    auto h = fx.make_session();
    fx.cmd(h, "CAPA");
    auto w = h.conn->written();
    assert(HAS(w, "+OK Capability list follows"));
    assert(HAS(w, "USER"));
    assert(HAS(w, "UIDL"));
    assert(HAS(w, "."));
    std::cout << "  [PASS] capa" << std::endl;
}

TEST(user_ok) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local");
    assert(HAS(h.conn->written(), "+OK Send PASS"));
    std::cout << "  [PASS] user_ok" << std::endl;
}

TEST(user_no_arg) {
    auto h = fx.make_session();
    fx.cmd(h, "USER");
    assert(HAS(h.conn->written(), "-ERR"));
    std::cout << "  [PASS] user_no_arg" << std::endl;
}

TEST(pass_without_user) {
    auto h = fx.make_session();
    fx.cmd(h, "PASS x");
    assert(HAS(h.conn->written(), "-ERR USER first"));
    std::cout << "  [PASS] pass_without_user" << std::endl;
}

TEST(pass_wrong_password) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local");
    fx.cmd(h, "PASS wrongpass");
    assert(FsmTestFixture::wait_for(h, "-ERR Authentication failed"));
    // 仍在 AUTHORIZATION，可重试
    assert(!HAS(h.conn->written(), "Mailbox locked"));
    std::cout << "  [PASS] pass_wrong_password" << std::endl;
}

TEST(pass_three_failures_close) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local");
    fx.cmd(h, "PASS bad1");
    FsmTestFixture::wait_for(h, "-ERR Authentication failed");
    fx.cmd(h, "PASS bad2");
    FsmTestFixture::wait_for(h, "-ERR Authentication failed");
    fx.cmd(h, "PASS bad3");
    assert(FsmTestFixture::wait_for(h, "Too many auth failures"));
    assert(h.session->is_closed());
    std::cout << "  [PASS] pass_three_failures_close" << std::endl;
}

TEST(pass_user_not_found) {
    auto h = fx.make_session();
    fx.cmd(h, "USER nobody@test.local");
    // auth cache miss → DB 查，推一个空结果（0 行）
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>());
    fx.cmd(h, "PASS x");
    assert(FsmTestFixture::wait_for(h, "-ERR Authentication failed"));
    std::cout << "  [PASS] pass_user_not_found" << std::endl;
}

TEST(quit_in_authorization) {
    auto h = fx.make_session();
    fx.cmd(h, "QUIT");
    assert(HAS(h.conn->written(), "+OK Bye"));
    std::cout << "  [PASS] quit_in_authorization" << std::endl;
}

// ========== TRANSACTION 阶段 ==========

TEST(pass_success) {
    auto h = fx.login();
    auto w = h.conn->written();
    assert(HAS(w, "+OK Mailbox locked and loaded, 2 messages"));
    std::cout << "  [PASS] pass_success" << std::endl;
}

TEST(stat) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "STAT");
    auto w = h.conn->written();
    assert(HAS(w, "+OK 2 "));
    assert(HAS(w, "43"));   // 21 + 22 octets
    std::cout << "  [PASS] stat: " << w.substr(0, w.find('\r')) << std::endl;
}

TEST(list_all) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "LIST");
    auto w = h.conn->written();
    assert(HAS(w, "+OK 2 messages (43 octets)"));
    assert(HAS(w, "1 21"));
    assert(HAS(w, "2 22"));
    assert(HAS(w, "\r\n.\r\n"));   // 终止符
    std::cout << "  [PASS] list_all" << std::endl;
}

TEST(list_single) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "LIST 2");
    assert(HAS(h.conn->written(), "+OK 2 22"));
    std::cout << "  [PASS] list_single" << std::endl;
}

TEST(list_out_of_range) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "LIST 9");
    assert(HAS(h.conn->written(), "-ERR no such message"));
    std::cout << "  [PASS] list_out_of_range" << std::endl;
}

TEST(uidl_all) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "UIDL");
    auto w = h.conn->written();
    assert(HAS(w, "+OK Unique-ID listing follows"));
    assert(HAS(w, "1 1001"));
    assert(HAS(w, "2 1002"));
    assert(HAS(w, "\r\n.\r\n"));
    std::cout << "  [PASS] uidl_all" << std::endl;
}

TEST(uidl_single) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "UIDL 2");
    assert(HAS(h.conn->written(), "+OK 2 1002"));
    std::cout << "  [PASS] uidl_single" << std::endl;
}

TEST(retr_dot_stuffing) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "RETR 2");
    assert(FsmTestFixture::wait_for(h, "+OK 22 octets"));
    auto w = h.conn->written();
    assert(HAS(w, "..dot line"));   // dot-stuffing：行首 '.' 前缀 '.'
    assert(HAS(w, "normal line"));
    assert(HAS(w, "\r\n.\r\n"));    // 终止符
    std::cout << "  [PASS] retr_dot_stuffing" << std::endl;
}

TEST(retr_out_of_range) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "RETR 9");
    assert(HAS(h.conn->written(), "-ERR no such message"));
    std::cout << "  [PASS] retr_out_of_range" << std::endl;
}

TEST(dele_then_stat) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 1");
    assert(HAS(h.conn->written(), "+OK message 1 deleted"));
    h.conn->clear_written();
    fx.cmd(h, "STAT");
    assert(HAS(h.conn->written(), "+OK 1 "));   // 只剩 1 封
    std::cout << "  [PASS] dele_then_stat" << std::endl;
}

TEST(dele_rset_restore) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 1");
    fx.cmd(h, "RSET");
    assert(HAS(h.conn->written(), "+OK"));
    h.conn->clear_written();
    fx.cmd(h, "STAT");
    assert(HAS(h.conn->written(), "+OK 2 "));   // RSET 恢复
    std::cout << "  [PASS] dele_rset_restore" << std::endl;
}

TEST(dele_out_of_range) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 9");
    assert(HAS(h.conn->written(), "-ERR no such message"));
    std::cout << "  [PASS] dele_out_of_range" << std::endl;
}

TEST(noop) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "NOOP");
    assert(HAS(h.conn->written(), "+OK"));
    std::cout << "  [PASS] noop" << std::endl;
}

TEST(quit_in_transaction) {
    auto h = fx.login();
    h.conn->clear_written();
    fx.cmd(h, "DELE 1");
    fx.cmd(h, "QUIT");
    assert(FsmTestFixture::wait_for(h, "+OK Bye"));
    std::cout << "  [PASS] quit_in_transaction" << std::endl;
}

// ========== 边界 ==========

TEST(wrong_state_command) {
    auto h = fx.make_session();   // AUTHORIZATION
    fx.cmd(h, "STAT");
    assert(HAS(h.conn->written(), "-ERR Command not allowed in current state"));
    std::cout << "  [PASS] wrong_state_command" << std::endl;
}

TEST(command_case_insensitive) {
    auto h = fx.make_session();
    fx.cmd(h, "capa");
    assert(HAS(h.conn->written(), "+OK Capability list follows"));
    h.conn->clear_written();
    fx.cmd(h, "User alice@test.local");
    assert(HAS(h.conn->written(), "+OK Send PASS"));
    std::cout << "  [PASS] command_case_insensitive" << std::endl;
}

TEST(unknown_command) {
    auto h = fx.make_session();
    fx.cmd(h, "FOOBAR");
    assert(HAS(h.conn->written(), "-ERR Unknown command"));
    assert(!h.session->is_closed());   // 不关闭，可继续
    std::cout << "  [PASS] unknown_command" << std::endl;
}

TEST(extra_args_ignored) {
    auto h = fx.make_session();
    fx.cmd(h, "USER alice@test.local extra");
    assert(HAS(h.conn->written(), "+OK Send PASS"));
    std::cout << "  [PASS] extra_args_ignored" << std::endl;
}

// ========== 锁心跳（v2） ==========

TEST(heartbeat_timer_armed) {
    auto h = fx.login();
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    assert(ctx->heartbeat_timer != nullptr);
    assert(ctx->heartbeat_handler != nullptr);
    assert(!ctx->session_id.empty());
    std::cout << "  [PASS] heartbeat_timer_armed" << std::endl;
}

TEST(heartbeat_renew_keeps_lock) {
    auto h = fx.login();
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    // 续约 verify 得 cnt=1 → 锁还在（async CPS 版：mock 同步触发回调）
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"cnt", "1"}}}));
    bool ok = false;
    TraditionalPop3Fsm<MockConnection>::renew_lock_heartbeat_async(
        fx.router, ctx->user_id, ctx->session_id, ctx->shard_index,
        [&ok](bool v) { ok = v; });
    assert(ok);
    assert(!h.session->is_closed());
    std::cout << "  [PASS] heartbeat_renew_keeps_lock" << std::endl;
}

TEST(heartbeat_renew_lock_lost) {
    auto h = fx.login();
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    // 行已不存在（被 sweeper 回收）→ verify 得 0 → 续约失败
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>());
    bool ok = true;
    TraditionalPop3Fsm<MockConnection>::renew_lock_heartbeat_async(
        fx.router, ctx->user_id, ctx->session_id, ctx->shard_index,
        [&ok](bool v) { ok = v; });
    assert(!ok);
    std::cout << "  [PASS] heartbeat_renew_lock_lost" << std::endl;
}

TEST(heartbeat_lock_lost_closes) {
    // 剧情：会话拿到锁后，锁被外部回收（如 sweeper 清掉死锁后他人接管）。
    // 心跳续约 verify 得 0 → 本会话失去排他 → 必须关闭，而不是带着失效的锁继续。
    fx.fsm->heartbeat_interval_ = std::chrono::milliseconds(50);
    auto h = fx.login();
    // 显式推空结果：保证首次续约 verify 得 0（不依赖队列残留状态）
    fx.db_conn->push_sync_result(std::make_shared<test::MockDbResult>());
    assert(FsmTestFixture::wait_closed(h));
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    assert(!ctx->heartbeat_timer);   // close() 已取消并复位定时器
    fx.fsm->heartbeat_interval_ = std::chrono::seconds(60);   // 复位，防影响后续测试
    std::cout << "  [PASS] heartbeat_lock_lost_closes" << std::endl;
}

TEST(heartbeat_stops_after_close) {
    auto h = fx.login();
    fx.cmd(h, "QUIT");
    assert(FsmTestFixture::wait_for(h, "+OK Bye"));
    assert(FsmTestFixture::wait_closed(h));
    auto* ctx = static_cast<Pop3Context*>(h.session->get_context());
    assert(!ctx->heartbeat_timer);   // QUIT 关闭会话时取消心跳
    std::cout << "  [PASS] heartbeat_stops_after_close" << std::endl;
}

TEST(sweep_expired_locks_ok) {
    // sweeper：对所有 shard 删除过期锁（mock execute 返回 true）
    bool ok = TraditionalPop3Fsm<MockConnection>::sweep_expired_locks(fx.router);
    assert(ok);
    std::cout << "  [PASS] sweep_expired_locks_ok" << std::endl;
}

// ========== main ==========

int main() {
    FsmTestFixture fx;
    int passed = 0, failed = 0;
    auto run = [&](const char* name, void (*fn)(FsmTestFixture&)) {
        try {
            fn(fx);
            ++passed;
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "  [FAIL] " << name << ": " << e.what() << std::endl;
        } catch (...) {
            ++failed;
            std::cerr << "  [FAIL] " << name << ": unknown exception" << std::endl;
        }
    };

    run("greeting",                  test_greeting);
    run("capa",                      test_capa);
    run("user_ok",                   test_user_ok);
    run("user_no_arg",               test_user_no_arg);
    run("pass_without_user",         test_pass_without_user);
    run("pass_wrong_password",       test_pass_wrong_password);
    run("pass_three_failures_close", test_pass_three_failures_close);
    run("pass_user_not_found",       test_pass_user_not_found);
    run("quit_in_authorization",     test_quit_in_authorization);
    run("pass_success",              test_pass_success);
    run("stat",                      test_stat);
    run("list_all",                  test_list_all);
    run("list_single",               test_list_single);
    run("list_out_of_range",         test_list_out_of_range);
    run("uidl_all",                  test_uidl_all);
    run("uidl_single",               test_uidl_single);
    run("retr_dot_stuffing",         test_retr_dot_stuffing);
    run("retr_out_of_range",         test_retr_out_of_range);
    run("dele_then_stat",            test_dele_then_stat);
    run("dele_rset_restore",         test_dele_rset_restore);
    run("dele_out_of_range",         test_dele_out_of_range);
    run("noop",                      test_noop);
    run("quit_in_transaction",       test_quit_in_transaction);
    run("wrong_state_command",       test_wrong_state_command);
    run("command_case_insensitive",  test_command_case_insensitive);
    run("unknown_command",           test_unknown_command);
    run("extra_args_ignored",        test_extra_args_ignored);
    run("heartbeat_timer_armed",     test_heartbeat_timer_armed);
    run("heartbeat_renew_keeps_lock",test_heartbeat_renew_keeps_lock);
    run("heartbeat_renew_lock_lost", test_heartbeat_renew_lock_lost);
    run("heartbeat_lock_lost_closes",test_heartbeat_lock_lost_closes);
    run("heartbeat_stops_after_close",test_heartbeat_stops_after_close);
    run("sweep_expired_locks_ok",    test_sweep_expired_locks_ok);

    std::cout << "pop3_fsm_test: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed == 0 ? 0 : 1;
}
