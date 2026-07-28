// FSM pipeline benchmark — MockConnection 剥离网络 I/O，测量纯业务逻辑吞吐
//
// 构建: cd build && make fsm_bench
// 用法: ./fsm_bench [--threads N] [--iterations N]

#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/server_config.h"
#include "mail_system/back/thread_pool/io_thread_pool.h"
#include "mail_system/back/thread_pool/boost_thread_pool.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/common/logger.h"
#include "mock_connection.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <spdlog/common.h>

using namespace mail_system;
namespace pq = persist_storage;

// 预构建流水线命令
static std::string build_pipeline_msg(int idx) {
    return std::string("EHLO test\r\n")
        + "MAIL FROM:<user" + std::to_string(idx % 10) + "@test.local>\r\n"
        + "RCPT TO:<dest"  + std::to_string(idx % 10) + "@test.local>\r\n"
        + "DATA\r\n"
        + "From: user" + std::to_string(idx % 10) + "@test.local\r\n"
        + "To: dest"  + std::to_string(idx % 10) + "@test.local\r\n"
        + "Subject: bench\r\n\r\nshort body\r\n.\r\n"
        + "QUIT\r\n";
}

// 最小 ServerBase — 仅提供 config + persistent_queue，不启动监听
struct BenchServer : ServerBase {
    BenchServer(const ServerConfig& c,
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

// 单封投递: 创建 mock session → 喂命令 → FSM 处理 → QUIT 关闭
static bool deliver_one(TraditionalSmtpsFsm<MockConnection>& fsm,
                        BenchServer& server, int idx) {
    auto conn = std::make_unique<MockConnection>();
    conn->set_read_data(build_pipeline_msg(idx));  // 预加载流水线命令

    auto session = std::make_unique<SmtpsSession<MockConnection>>(
        &server, std::move(conn),
        std::shared_ptr<TraditionalSmtpsFsm<MockConnection>>(&fsm, [](void*){}));

    ListenerConfig lc;
    lc.type       = ListenerType::TCP;
    lc.port       = 25;
    lc.auth_policy = InboundAuthPolicy::OFF;
    session->set_listener_config(lc);

    session->set_current_state(static_cast<int>(SmtpsState::INIT));
    session->set_next_event(static_cast<int>(SmtpsEvent::CONNECT));

    // process_read → do_async_read → 从 MockConnection 读 → pipeline 循环
    auto base = std::unique_ptr<SessionBase<MockConnection>>(session.release());
    static_cast<SmtpsSession<MockConnection>*>(base.get())
        ->process_read();
    return true;
}

int main(int argc, char* argv[]) {
    size_t thread_count = 4;
    size_t iterations   = 50000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--threads" || arg == "-t") && i + 1 < argc)
            thread_count = static_cast<size_t>(std::stoull(argv[++i]));
        else if ((arg == "--iterations" || arg == "-n") && i + 1 < argc)
            iterations = static_cast<size_t>(std::stoull(argv[++i]));
    }

    Logger::get_instance().init("fsm_bench.log", 0, 1, spdlog::level::off, false, false);
    auto io_pool     = std::make_shared<IOThreadPool>(thread_count);
    auto worker_pool = std::make_shared<BoostThreadPool>(2);
    io_pool->start();
    worker_pool->start();

    auto router = std::make_shared<router::StaticShardRouter>(
        std::vector<std::pair<std::string, int>>{},  // empty domain mappings
        0,                                            // default shard
        std::vector<std::shared_ptr<DBPool>>{},       // no DB pools
        std::vector<std::shared_ptr<storage::IStorageProvider>>{}  // no storages
    );

    auto persist_q = std::make_shared<pq::PersistentQueue>(router, worker_pool);
    pq::PersistentQueuePressureConfig pc;
    pc.max_inflight_mails           = 1000000;
    pc.min_available_memory_mb      = 0;
    pc.min_db_available_connections = 0;
    persist_q->set_pressure_config(pc);
    persist_q->set_local_domain("test.local");

    ServerConfig cfg;
    cfg.perf_mode     = true;
    cfg.apply_perf_mode();
    cfg.use_database       = false;
    cfg.inbound_ack_mode   = InboundAckMode::AFTER_ENQUEUE;
    cfg.system_domain      = "test.local";
    cfg.mail_storage_path  = "/tmp/fsm_bench_mail";
    cfg.attachment_storage_path = "/tmp/fsm_bench_att";
    cfg.storage.local.mail_path       = "/tmp/fsm_bench_mail";
    cfg.storage.local.attachment_path = "/tmp/fsm_bench_att";

    BenchServer server(cfg, io_pool, worker_pool, router, persist_q);

    auto fsm = std::make_shared<TraditionalSmtpsFsm<MockConnection>>(
        io_pool, worker_pool, persist_q, router);

    // 预注入 auth 缓存数据 (模拟已认证用户，避免 DB 查询)
    for (int i = 0; i < 10; ++i) {
        std::string email = "test" + std::to_string(i) + "@test.local";
        AuthCacheEntry entry;
        entry.password_hash = "test123";
        entry.status = 1;
        fsm->m_authCache->inject(email, entry);
    }

    std::cout << "=== FSM Mock Benchmark ===\n"
              << "  threads=" << thread_count
              << "  iterations_per_thread=" << iterations
              << "  total=" << (thread_count * iterations) << "\n"
              << "  auth_cache_preload=10\n\n";

    // 预热
    deliver_one(*fsm, server, 0);

    // 单线程
    size_t warm_n = std::min<size_t>(5000, iterations);
    {
        auto t0 = std::chrono::steady_clock::now();
        size_t ok = 0;
        for (size_t i = 0; i < warm_n; ++i) {
            if (deliver_one(*fsm, server, static_cast<int>(i))) ++ok;
        }
        auto t1 = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[1 thread]  ok=" << ok << "/" << warm_n
                  << "  elapsed=" << s << "s"
                  << "  rate=" << static_cast<size_t>(ok / s) << " msg/s\n";
    }

    // 多线程
    {
        std::atomic<size_t> total_ok{0};
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&, t]() {
                size_t local = 0;
                for (size_t i = 0; i < iterations; ++i) {
                    if (deliver_one(*fsm, server,
                                    static_cast<int>(i * thread_count + t)))
                        ++local;
                }
                total_ok.fetch_add(local, std::memory_order_relaxed);
            });
        }
        for (auto& th : threads) th.join();
        auto t1 = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        size_t ok = total_ok.load();
        std::cout << "[" << thread_count << " threads]  ok=" << ok
                  << "/" << (thread_count * iterations)
                  << "  elapsed=" << s << "s"
                  << "  rate=" << static_cast<size_t>(ok / s) << " msg/s\n\n";
    }

    std::cout << "=== Interpretation ===\n"
              << "Mock rate = pure FSM (no TCP/TLS/asio overhead).\n"
              << "Real pipe+reuse plain: ~12502 msg/s → overhead = mock/12502 ×\n"
              << "Real TLS+AUTH:          ~349 msg/s → TLS cost visible\n"
              << "Full matrix in test/bench-report.md\n";

    persist_q->shutdown();
    worker_pool->stop();
    io_pool->stop();
}
