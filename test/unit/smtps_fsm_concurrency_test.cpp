// SMTP 入站异步并发测试 — Mock DNS/DB 延迟回调 + 真实 IO 线程
//
// 目标：为入站校验/FSM 的异步路径（MAIL FROM SPF / RCPT user_exists / DATA_END verify_all）
// 建立真实多线程时序测试：
//   - session 的常规命令处理跑在 IOThreadPool 的 io_context 线程上；
//   - DNS/DB 异步回调由测试线程手动触发（模拟 c-ares / DB worker 线程）；
//   - 验证跨线程的 paused/resume、shared_ptr 保活（回调晚于 session 逻辑结束不悬垂）。
// 配合 TSan（-fsanitize=thread）可检测 session 状态竞争。
#undef NDEBUG
#include <cassert>  // 在 #undef NDEBUG 后重新包含，确保 assert 生效
#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp"
#include "mail_system/back/mailServer/session/smtps_session.tpp"
#include "framework/session_base.tpp"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/inbound/inbound_verifier.h"
#include "mock_connection.h"
#include "mock_dns_resolver.h"
#include "mock_db_pool.h"

#include <boost/asio/post.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <spdlog/common.h>

using namespace mail_system;
namespace pq = persist_storage;

// ========== 测试夹具 ==========

// 与 smtps_fsm_test 一致：必须派生自 SmtpsServer（SmtpsSession 构造用 static_cast 读 m_persistentQueue）。
struct TestServer : SmtpsServer {
    TestServer(const ServerConfig& c,
               std::shared_ptr<ThreadPoolBase> io,
               std::shared_ptr<ThreadPoolBase> w,
               std::shared_ptr<router::IShardRouter> r)
        : SmtpsServer(c, io, w, nullptr) {
        m_shardRouter = std::move(r);
    }
    void start() override {}
};

// 会话句柄：captured 为外部写缓冲镜像（session 析构后仍可断言）
struct SessionHandle {
    MockConnection* conn = nullptr;
    std::shared_ptr<SmtpsSession<MockConnection>> session;
    std::string captured;
};

struct ConcurrencyTestFixture {
    std::shared_ptr<IOThreadPool> io_pool;
    std::shared_ptr<BoostThreadPool> worker_pool;
    std::shared_ptr<test::MockDnsResolver> dns;
    std::shared_ptr<test::MockDbPool> db_pool;
    std::shared_ptr<router::StaticShardRouter> router;
    std::shared_ptr<pq::PersistentQueue> persist_q;
    std::shared_ptr<TestServer> server;
    std::shared_ptr<TraditionalSmtpsFsm<MockConnection>> fsm;
    ServerConfig cfg;

    ConcurrencyTestFixture() {
        Logger::get_instance().init("smtps_fsm_concurrency_test.log", 0, 1, spdlog::level::off, false, false);
        // 连接级异步 I/O 由 MockConnection 内部 MockIoContext 线程驱动（还原 asio 投递语义）。
        // io_pool 保留仅为满足 SmtpsServer/FSM 构造签名，不参与连接读写。
        io_pool     = std::make_shared<IOThreadPool>(1);
        worker_pool = std::make_shared<BoostThreadPool>(2);
        io_pool->start();
        worker_pool->start();

        dns     = std::make_shared<test::MockDnsResolver>();
        dns->set_mode(test::MockDnsResolver::Mode::Manual);   // 并发测试手动触发 DNS 回调
        db_pool = std::make_shared<test::MockDbPool>();

        router = std::make_shared<router::StaticShardRouter>(
            std::vector<std::pair<std::string, int>>{},
            0,
            std::vector<std::shared_ptr<DBPool>>{db_pool},  // shard 0 → mock DB（user_exists 异步路径）
            std::vector<std::shared_ptr<storage::IStorageProvider>>{});

        persist_q = std::make_shared<pq::PersistentQueue>(router, worker_pool);
        pq::PersistentQueuePressureConfig pc;
        pc.max_inflight_mails           = 1000000;
        pc.min_available_memory_mb      = 0;
        pc.min_db_available_connections = 0;
        persist_q->set_pressure_config(pc);
        persist_q->set_local_domain("test.local");

        cfg.perf_mode          = false;   // 允许 SPF/DKIM/DMARC 校验
        cfg.use_database       = false;
        cfg.inbound_ack_mode   = InboundAckMode::AFTER_ENQUEUE;
        cfg.system_domain      = "test.local";
        cfg.inbound_spf_mode   = "off";   // 测试按需开启（hard/pass）
        cfg.inbound_dkim_mode  = "off";
        cfg.inbound_dmarc_mode = "off";
        cfg.mail_storage_path  = "/tmp/smtps_fsm_conc_test_mail";
        cfg.attachment_storage_path = "/tmp/smtps_fsm_conc_test_att";
        cfg.storage.local.mail_path       = "/tmp/smtps_fsm_conc_test_mail";
        cfg.storage.local.attachment_path = "/tmp/smtps_fsm_conc_test_att";

        system("mkdir -p /tmp/smtps_fsm_conc_test_mail /tmp/smtps_fsm_conc_test_att");

        server = std::shared_ptr<TestServer>(new TestServer(cfg, io_pool, worker_pool, router));
        server->m_persistentQueue = persist_q;
        server->m_dnsResolver = dns;   // 注入 mock DNS → MAIL FROM SPF / DATA_END verify 路径

        fsm = std::make_shared<TraditionalSmtpsFsm<MockConnection>>(
            io_pool, worker_pool, persist_q, router);

        // 预注入收件人存在性缓存（RCPT 快速路径）
        auto inject_rcpt = [this](const std::string& email, int status) {
            AuthCacheEntry entry;
            entry.password_hash = "";
            entry.status = status;
            fsm->m_recipientCache->inject(email, entry);
        };
        inject_rcpt("rcpt@test.local", 1);
        inject_rcpt("rcpt1@test.local", 1);
        inject_rcpt("r1@test.local", 1);
    }

    ~ConcurrencyTestFixture() {
        // 确保 DB mock 回到同步模式（若还有 pending 持久化链，避免 worker 线程卡在未触发的回调上）
        db_pool->mock_conn()->set_deferred(false);
        persist_q->shutdown();
        worker_pool->stop();
        io_pool->stop();
        dns->join_all();
        system("rm -rf /tmp/smtps_fsm_conc_test_mail /tmp/smtps_fsm_conc_test_att");
    }

    // 重新应用配置（FSM 通过 atomic_load 读取 server->m_config）
    void apply_config(const ServerConfig& c) {
        cfg = c;
        std::atomic_store(&server->m_config, std::make_shared<ServerConfig>(c));
    }
    ServerConfig with_spf(const std::string& mode) {
        ServerConfig c = cfg;
        c.inbound_spf_mode = mode;
        return c;
    }

    std::shared_ptr<SessionHandle> make_session(const std::string& preload = "",
                                                InboundAuthPolicy auth_policy = InboundAuthPolicy::OFF) {
        auto h = std::make_shared<SessionHandle>();
        auto conn_u = std::make_unique<MockConnection>();
        h->conn = conn_u.get();
        h->conn->capture_to(&h->captured);   // 写缓冲镜像到外部 string（session 析构后仍可断言）
        if (!preload.empty())
            h->conn->set_read_data(preload);

        h->session = std::make_shared<SmtpsSession<MockConnection>>(
            server.get(), std::move(conn_u), fsm);

        ListenerConfig lc;
        lc.type        = ListenerType::TCP;
        lc.port        = 25;
        lc.auth_policy = auth_policy;
        h->session->set_listener_config(lc);
        h->session->set_current_state(static_cast<int>(SmtpsState::INIT));
        h->session->set_next_event(static_cast<int>(SmtpsEvent::CONNECT));
        return h;
    }

    // 启动连接的 MockIoContext 工作线程，并把 session 初始 process_read
    // 作为任务投递到该 context——由独立线程执行命令链（还原 asio 语义）。
    // 等待命令链完全暂停（context 空闲）后才返回，保证主线程触发异步回调时
    // context 线程已退出 session（与生产时序一致：io 线程已暂停，回调独占）。
    void run_on_io(const std::shared_ptr<SessionHandle>& h) {
        h->conn->start();                    // 线程模式：异步完成由独立线程投递
        h->conn->context().post([h]() { h->session->process_read(); });
        assert(h->conn->wait_idle(3000));    // 命令链暂停后 context 空闲
    }
};

// ========== 测试宏 ==========
#define TEST(name) \
    void test_##name(ConcurrencyTestFixture& fx)
#define HAS(str, sub) ((str).find(sub) != std::string::npos)

// 带超时的轮询等待（异步回调/析构发生在其它线程）
template <typename Pred>
static bool wait_until(Pred pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// ========== 1. MAIL FROM SPF 异步路径（手动触发 DNS 回调） ==========

TEST(spf_hard_fail_async) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("hard"));
    fx.dns->set_txt("attacker.com", {"v=spf1 -all"});   // SPF hard-fail

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<spam@attacker.com>\r\n");
    fx.run_on_io(h);

    // 等待 context 线程处理命令链至 SPF 暂停（DNS 回调 pending）
    assert(wait_until([&] { return fx.dns->has_pending_txt("attacker.com"); }));
    // 主线程手动触发 DNS 回调（模拟 c-ares 线程）→ 应在本线程恢复 session
    fx.dns->fire_txt("attacker.com");

    assert(HAS(h->captured, "550 5.7.1 SPF verification failed"));
    std::cout << "  [PASS] spf_hard_fail_async (cross-thread DNS callback)" << std::endl;
}

TEST(spf_pass_async) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("pass"));
    fx.dns->set_txt("sender.com", {"v=spf1 ip4:127.0.0.1"});   // 客户端 IP=127.0.0.1 → pass

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@sender.com>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n");
    fx.run_on_io(h);

    assert(wait_until([&] { return fx.dns->has_pending_txt("sender.com"); }));
    fx.dns->fire_txt("sender.com");

    // SPF pass → 250 Ok → drain 缓冲的 RCPT TO（缓存命中）→ 250
    assert(HAS(h->captured, "250 Ok"));
    std::cout << "  [PASS] spf_pass_async (SPF 250 + 流水线续传)" << std::endl;
}

// ========== 2. RCPT user_exists DB 异步路径（延迟触发 async_query 回调） ==========

TEST(user_exists_db_async_exists) {
    fx.apply_config(fx.with_spf("off"));   // MAIL FROM 不走 SPF，保持流程直接到 RCPT
    fx.db_pool->mock_conn()->set_deferred(true);
    fx.db_pool->mock_conn()->clear_pending();   // 排除持久化 worker 等其它查询来源

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@test.local>\r\n"
        "RCPT TO:<dbuser@test.local>\r\n");   // 缓存未命中 → DB 异步
    fx.run_on_io(h);

    auto* db = fx.db_pool->mock_conn().get();
    assert(wait_until([&] {
        return db->has_pending_query() &&
               !db->last_query_params().empty() && db->last_query_params()[0] == "dbuser@test.local";
    }));
    assert(!db->last_query_params().empty() && db->last_query_params()[0] == "dbuser@test.local");

    // 主线程触发 DB 回调：查询命中 status=1 → 存在 → 250
    db->fire_query(std::make_shared<test::MockDbResult>(
        std::vector<std::map<std::string, std::string>>{{{"status", "1"}}}));

    assert(HAS(h->captured, "250 Ok"));
    std::cout << "  [PASS] user_exists_db_async_exists (DB 回调触发 → 250)" << std::endl;
}

TEST(user_exists_db_async_not_found) {
    fx.apply_config(fx.with_spf("off"));
    fx.db_pool->mock_conn()->set_deferred(true);
    fx.db_pool->mock_conn()->clear_pending();   // 排除持久化 worker 等其它查询来源

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@test.local>\r\n"
        "RCPT TO:<ghost@test.local>\r\n");   // 缓存未命中 → DB 异步
    fx.run_on_io(h);

    auto* db = fx.db_pool->mock_conn().get();
    assert(wait_until([&] {
        return db->has_pending_query() &&
               !db->last_query_params().empty() && db->last_query_params()[0] == "ghost@test.local";
    }));

    // 空结果 → 不存在 → 550 User unknown（负缓存）
    db->fire_query(std::make_shared<test::MockDbResult>());

    assert(HAS(h->captured, "550 5.1.1 User unknown"));
    std::cout << "  [PASS] user_exists_db_async_not_found (DB 回调触发 → 550)" << std::endl;
}

// ========== 3. DATA_END verify_all_from_file 异步路径 ==========

TEST(verify_all_from_file_async) {
    inbound::InboundVerifier::clear_dns_cache();
    ServerConfig c = fx.cfg;
    c.inbound_spf_mode   = "off";    // MAIL FROM 不查 SPF → spf_checked=false
    c.inbound_dkim_mode  = "off";
    c.inbound_dmarc_mode = "on";     // DATA_END 触发 verify_all_from_file → DMARC TXT 异步
    fx.apply_config(c);
    // DB mock 转同步：DATA_END 入队后后台持久化链立即完成，避免 worker 线程卡在未触发回调
    fx.db_pool->mock_conn()->set_deferred(false);
    fx.dns->set_txt("_dmarc.dmarcdom.com", {"v=DMARC1; p=none"});

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<s@test.local>\r\n"
        "RCPT TO:<rcpt@test.local>\r\n"
        "DATA\r\n"
        "From: sender@dmarcdom.com\r\n"
        "To: rcpt@test.local\r\n"
        "Subject: verify async\r\n"
        "\r\n"
        "hello verify\r\n"
        ".\r\n");
    fx.run_on_io(h);

    // DATA_END 已暂停，等待 DMARC TXT 回调
    assert(wait_until([&] { return fx.dns->has_pending_txt("_dmarc.dmarcdom.com"); }));
    fx.dns->fire_txt("_dmarc.dmarcdom.com");

    // DMARC none → 校验完成 → 入队 + AFTER_ENQUEUE 响应 "250 OK"
    assert(HAS(h->captured, "250 OK"));
    // 等待持久化 worker 处理完入队邮件，避免其后台查询干扰后续测试的 DB mock 状态
    assert(wait_until([&] { return fx.persist_q->inflight_count() == 0; }, 5000));
    std::cout << "  [PASS] verify_all_from_file_async (DMARC DNS 回调 → 入队 + 250 OK)" << std::endl;
}

// ========== 4. 关键边界：回调晚于 session 逻辑结束 → shared_ptr 保活不悬垂 ==========

TEST(boundary_callback_after_session_release) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("hard"));
    fx.dns->set_txt("attacker.com", {"v=spf1 -all"});

    auto h = fx.make_session(
        "EHLO test\r\n"
        "MAIL FROM:<spam@attacker.com>\r\n");
    fx.run_on_io(h);
    assert(wait_until([&] { return fx.dns->has_pending_txt("attacker.com"); }));

    // 模拟 io_context 即将关闭 / 连接已逻辑结束：测试释放唯一外部 session 引用。
    // 此时 session 唯一存活来源是 DNS 回调链内部持有的 shared_ptr。
    std::weak_ptr<SmtpsSession<MockConnection>> weak = h->session;
    h->session.reset();
    assert(weak.expired() == false);   // 回调链仍持有 shared_ptr → session 存活

    // 触发回调：应能在 session 上完成状态机操作而不悬垂
    fx.dns->fire_txt("attacker.com");

    // 回调完成后所有 shared_ptr 释放 → session 干净析构
    assert(wait_until([&] { return weak.expired(); }));
    assert(HAS(h->captured, "550 5.7.1 SPF verification failed"));   // 回调期间完成写响应
    std::cout << "  [PASS] boundary_callback_after_session_release (shared_ptr 保活，无悬垂)" << std::endl;
}

// ========== 5. 跨线程回调：Manual 触发 + 独立线程投递（TSan 下验证无数据竞争） ==========
//
// 与生产时序一致：io 线程已完成命令链并暂停（run_on_io 内 wait_idle），
// 然后 DNS 回调在独立线程触发，续跑 FSM（跨线程但不与 io 线程并发访问 session）。
// 避免 AutoDelay 固定小延迟在 io 线程尚未退出命令链时过早触发（生产 DNS 延迟
// 远大于 io 线程退出耗时，过早触发是测试假象）。MockDnsResolver::AutoDelay 模式
// 保留供压力测试：需用足够大的 delay 匹配生产时序。

TEST(cross_thread_callback_background_fire) {
    inbound::InboundVerifier::clear_dns_cache();
    fx.apply_config(fx.with_spf("pass"));
    fx.dns->set_txt("sender.com", {"v=spf1 ip4:127.0.0.1"});   // 客户端 IP=127.0.0.1 → pass

    const int kIterations = 8;
    std::vector<std::shared_ptr<SessionHandle>> handles;
    for (int i = 0; i < kIterations; ++i) {
        handles.push_back(fx.make_session(
            "EHLO test\r\n"
            "MAIL FROM:<s@sender.com>\r\n"
            "RCPT TO:<rcpt@test.local>\r\n"));
    }

    // 并发发起多个 session：命令链在各自 MockIoContext 线程执行并暂停于 SPF
    std::vector<std::thread> starters;
    for (auto& h : handles)
        starters.emplace_back([&fx, h]() { fx.run_on_io(h); });
    for (auto& t : starters) t.join();
    for (auto& h : handles)
        assert(wait_until([&] { return fx.dns->has_pending_txt("sender.com"); }));

    // 在独立线程触发 DNS 回调（模拟 c-ares 线程），跨线程续跑 FSM → 250 Ok
    std::vector<std::thread> firers;
    for (auto& h : handles)
        firers.emplace_back([&fx]() { fx.dns->fire_txt("sender.com"); });
    for (auto& t : firers) t.join();

    // 等待全部 session 完成 → 250 Ok（随后无输入 → eof → close）
    // 用 written()（内部加锁）轮询，避免与回调线程写 captured 并发读竞态
    bool all_done = wait_until([&] {
        for (auto& h : handles)
            if (!HAS(h->conn->written(), "250 Ok")) return false;
        return true;
    }, 5000);
    assert(all_done);

    std::cout << "  [PASS] cross_thread_callback_background_fire (" << kIterations
              << " sessions, 回调跨线程触发, TSan 下无 data race)" << std::endl;
}

// ========== main ==========
int main() {
    std::cout << "Inbound SMTP Async/Concurrency Test Suite\n===========================================\n";

    try {
        ConcurrencyTestFixture fx;

        // 1. SPF 异步
        test_spf_hard_fail_async(fx);
        test_spf_pass_async(fx);

        // 2. RCPT user_exists DB 异步
        test_user_exists_db_async_exists(fx);
        test_user_exists_db_async_not_found(fx);

        // 3. DATA_END verify_all_from_file 异步
        test_verify_all_from_file_async(fx);

        // 4. 边界：回调晚于 session 结束
        test_boundary_callback_after_session_release(fx);

        // 5. 跨线程回调（独立线程触发）
        test_cross_thread_callback_background_fire(fx);

        std::cout << "\nAll concurrency tests passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
