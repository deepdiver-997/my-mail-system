// ServerBase 单元测试：生命周期状态、连接计数、入侵检测集成、配置热重载、
// DNS 注入、metrics 端到端。
//
// 用 NullStorageProvider + 0 线程池构造，免 DB/免真实存储/免线程（ServerBase ctor
// 已把 NullDBPool / NullStorageProvider 接好）。必须链 COMMON_SRCS（同 FSM 测试）。
#undef NDEBUG
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "framework/metrics_server.h"
#include "framework/net/dns_resolver.h"
#include "framework/server_base.h"
#include "framework/server_config.h"
#include "framework/thread_pool/io_thread_pool.h"

using namespace mail_system;

namespace {

// IDnsResolver 最小空实现 — A2 DNS 注入测试要注入一个具体子类
struct StubDnsResolver : pr::IDnsResolver {
    void async_resolve_mx(const std::string&, pr::MxCallback cb) override  { cb({}); }
    void async_resolve_host(const std::string&, pr::AddrCallback cb) override { cb({}); }
    void async_resolve_txt(const std::string&, pr::TxtCallback cb) override { cb({}); }
    void async_resolve_ptr(const std::string&, pr::PtrCallback cb) override { cb({}); }
};

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

std::filesystem::path tmp_dir() {
    return std::filesystem::temp_directory_path() / "protorelay_server_base_test";
}

// ServerBase 是抽象基类：实现纯虚接口的最小具体子类
struct TestServerBase : ServerBase {
    explicit TestServerBase(const ServerConfig& c) : ServerBase(c, nullptr, nullptr, nullptr) {}
    TestServerBase(const ServerConfig& c, std::shared_ptr<ThreadPoolBase> io)
        : ServerBase(c, io, nullptr, nullptr) {}
    void start() override {}
    bool should_reject_connection(std::string& reason, const std::string&) const override {
        reason.clear();
        return false;
    }
    // 暴露 protected 启动方法供测试调
    using ServerBase::start_metrics_server;
    using ServerBase::stop_metrics_server;
};

// 手写 HTTP GET：拼 HTTP/1.1 请求 + 解析 body。不用 libcurl。
// 返回 HTTP 响应体（剥离 status line + headers），连接失败返空。
std::string http_get(const std::string& host, uint16_t port, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (::send(fd, req.data(), req.size(), 0) <= 0) { ::close(fd); return {}; }

    std::string raw;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return {};
    return raw.substr(hdr_end + 4);
}

ServerConfig make_cfg() {
    ServerConfig cfg;
    cfg.use_database                = false;
    cfg.storage.provider            = "null";       // 不写盘
    cfg.io_thread_count             = 0;            // 不建线程池
    cfg.worker_thread_count         = 0;
    cfg.metrics_enabled             = false;
    cfg.system_domain               = "test.local";
    cfg.intrusion_detection_enabled = false;
    cfg.log_level                   = "off";
    cfg.log_to_file                 = false;   // 避免空 log_file 触发 spdlog 打开失败噪音
    cfg.listeners.emplace_back(pr::ListenerType::TCP, 25);
    return cfg;
}

} // namespace

int main() {
    std::printf("server_base_test\n");
    auto dir = tmp_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // 1. 构造 + 生命周期状态
    {
        TestServerBase server(make_cfg());
        expect_true(server.get_state() == ServerState::Paused,
                    "ctor leaves server Paused");
        expect_true(server.m_domain == "test.local", "domain from config");
        expect_true(server.get_shard_router() != nullptr, "shard router built");
        expect_true(server.get_dns_resolver() == nullptr, "no dns resolver by default");
        expect_true(server.get_mailbox_cache() == nullptr, "no mailbox cache by default");

        server.request_stop();
        expect_true(server.get_state() == ServerState::Pausing,
                    "request_stop -> Pausing");
    }

    // 2. 连接计数
    {
        TestServerBase server(make_cfg());
        server.increment_connection_count();
        expect_true(server.active_connections_.load() == 1, "active +1");
        server.decrement_connection_count();
        expect_true(server.active_connections_.load() == 0, "active back to 0");
        server.increment_connections_total();
        server.increment_connections_rejected();
        server.increment_mails_accepted();
        expect_true(server.connections_total_.load() == 1, "total connections");
        expect_true(server.connections_rejected_total_.load() == 1, "rejected total");
        expect_true(server.mails_accepted_total_.load() == 1, "mails accepted total");
    }

    // 3. metrics 缺失时 push_* 安全 no-op
    {
        TestServerBase server(make_cfg());
        server.push_metric_gauge("g", {}, 1.0);
        server.push_metric_counter("c", {}, 1);
        server.push_metric_observe("o", {}, 1.0);
        expect_true(server.get_metrics().expired() || server.get_metrics().lock() == nullptr,
                    "no metrics server when disabled");
    }

    // 4. 入侵检测集成：会话结束记录 → 达标封禁
    {
        auto cfg = make_cfg();
        cfg.intrusion_detection_enabled = true;
        cfg.intrusion_ban_threshold      = 3;
        TestServerBase server(cfg);
        server.m_intrusionDetector.set_enabled(true);
        server.m_intrusionDetector.set_ban_threshold(3);

        for (int i = 0; i < 3; i++) server.record_session_end("8.8.8.8", false);
        expect_true(server.is_ip_banned("8.8.8.8"), "3 auth failures -> banned");

        server.record_session_end("1.1.1.1", true);   // 认证成功不算失败
        expect_true(!server.is_ip_banned("1.1.1.1"), "authenticated session not banned");
        expect_true(!server.is_ip_banned("9.9.9.9"), "unknown ip not banned");
    }

    // 5. reload_config：合法热重载
    {
        TestServerBase server(make_cfg());
        auto good = dir / "good.json";
        std::ofstream(good) << R"({"address":"0.0.0.0","listeners":[{"type":"tcp","port":25}],"log_level":"debug"})";
        expect_true(server.reload_config(good.string()), "valid reload accepted");
        auto cfg2 = std::atomic_load(&server.m_config);
        expect_true(cfg2->log_level == "debug", "reload applied log_level");
    }

    // 6. reload_config：结构变化（监听器端口）→ 拒绝
    {
        TestServerBase server(make_cfg());
        auto bad = dir / "bad-structure.json";
        std::ofstream(bad) << R"({"address":"0.0.0.0","listeners":[{"type":"ssl","port":465}]})";
        expect_true(!server.reload_config(bad.string()), "listener change rejected");
    }

    // 7. reload_config：地址变化 → 拒绝
    {
        TestServerBase server(make_cfg());
        auto bad = dir / "bad-addr.json";
        std::ofstream(bad) << R"({"address":"1.2.3.4","listeners":[{"type":"tcp","port":25}]})";
        expect_true(!server.reload_config(bad.string()), "address change rejected");
    }

    // 8. reload_config：文件不存在 → false
    {
        TestServerBase server(make_cfg());
        expect_true(!server.reload_config((dir / "nope.json").string()),
                    "missing config file -> false");
    }

    // 9. reload_config：detector 记录在 reload 期间不丢（reload 改 cfg 不动 detector）
    //    注：is_ip_banned 同时要求 cfg 启用 + detector 有记录，缺一不可。
    {
        auto cfg = make_cfg();
        cfg.intrusion_detection_enabled = true;
        cfg.intrusion_ban_threshold      = 3;
        TestServerBase server(cfg);
        server.m_intrusionDetector.set_enabled(true);
        server.m_intrusionDetector.set_ban_threshold(3);

        for (int i = 0; i < 3; i++) server.record_session_end("8.8.8.8", false);
        expect_true(server.is_ip_banned("8.8.8.8"), "3 failures -> banned");

        // reload 保留 intrusion_detection_enabled=true（loadFromFile 用 default 0/false）
        auto good = dir / "after_ban.json";
        std::ofstream(good) << R"({"address":"0.0.0.0","listeners":[{"type":"tcp","port":25}],"log_level":"info","intrusion_detection_enabled":true,"intrusion_ban_threshold":3})";
        expect_true(server.reload_config(good.string()), "reload after ban (keeps intrusion cfg)");

        expect_true(server.m_intrusionDetector.size() == 1,
                    "detector records survive reload (cfg change does not touch detector)");
        expect_true(server.m_intrusionDetector.query("8.8.8.8").failed_count == 3,
                    "detector record count preserved");
        expect_true(server.is_ip_banned("8.8.8.8"),
                    "is_ip_banned true when cfg enabled + detector has record");
    }

    // 10. reload_config：幂等 — 同一文件 reload 两次结果一致
    {
        TestServerBase server(make_cfg());
        auto cfg_path = dir / "idempotent.json";
        std::ofstream(cfg_path) << R"({"address":"0.0.0.0","listeners":[{"type":"tcp","port":25}],"log_level":"warn"})";
        expect_true(server.reload_config(cfg_path.string()),  "first reload ok");
        expect_true(server.reload_config(cfg_path.string()),  "second reload ok (idempotent)");
        expect_true(std::atomic_load(&server.m_config)->log_level == "warn",
                    "log_level stable across reloads");
    }

    // 11. reload_config：第二次 reload 失败不影响第一次的 config
    {
        TestServerBase server(make_cfg());
        auto good = dir / "good2.json";
        std::ofstream(good) << R"({"address":"0.0.0.0","listeners":[{"type":"tcp","port":25}],"log_level":"trace"})";
        expect_true(server.reload_config(good.string()), "first reload");
        expect_true(std::atomic_load(&server.m_config)->log_level == "trace", "first applied");

        auto bad = dir / "bad2.json";
        std::ofstream(bad) << R"({"address":"9.9.9.9","listeners":[{"type":"tcp","port":25}]})";
        expect_true(!server.reload_config(bad.string()), "second reload rejected (address change)");
        expect_true(std::atomic_load(&server.m_config)->log_level == "trace",
                    "first config preserved after rejected reload");
    }

    // 12. request_stop 后连接计数仍可递增（不变性：状态机转换不阻塞计数器）
    {
        TestServerBase server(make_cfg());
        server.request_stop();
        expect_true(server.get_state() == ServerState::Pausing, "stop requested");
        server.increment_connection_count();
        server.increment_mails_accepted();
        expect_true(server.active_connections_.load() == 1,
                    "connection counter still works after request_stop");
        expect_true(server.mails_accepted_total_.load() == 1,
                    "mails_accepted counter still works after request_stop");
        server.decrement_connection_count();
        expect_true(server.active_connections_.load() == 0, "decrement still works");
    }

    // 13. A2：DNS 注入 — 注入后 get_dns_resolver() 返注入的实例（剧情 invariant）
    {
        TestServerBase server(make_cfg());
        expect_true(server.get_dns_resolver() == nullptr, "no resolver by default");
        auto stub = std::make_shared<StubDnsResolver>();
        server.m_dnsResolver = stub;
        expect_true(server.get_dns_resolver() == stub,
                    "injected resolver returned by get_dns_resolver");
        // 替换后旧 stub 引用计数减 1，新 stub 持 1 → shared_ptr 一致
        expect_true(server.m_dnsResolver.use_count() == 2,  // m_dnsResolver + 局部 stub
                    "shared refcount balanced after inject");
    }

    // 14. B1：metrics 端到端 — start_metrics_server → HTTP GET /metrics → 看到 push 的数据
    {
        auto cfg = make_cfg();
        cfg.metrics_enabled     = true;
        cfg.metrics_port        = 0;   // OS 自动分配
        cfg.metrics_bind_address = "127.0.0.1";

        auto io_pool = std::make_shared<IOThreadPool>(1);
        io_pool->start();
        std::thread io_thread([&] { io_pool->get_io_context().run(); });

        TestServerBase server(cfg, io_pool);
        server.start_metrics_server();

        // wait for bound_port to be set (start() in ctor path is sync, but be defensive)
        uint16_t port = 0;
        for (int i = 0; i < 50; i++) {
            if (auto m = server.get_metrics().lock()) { port = m->bound_port(); }
            if (port != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        expect_true(port != 0, "metrics server bound a port");

        // 推一些指标
        server.push_metric_counter("mails_accepted_total", {}, 42);
        server.push_metric_gauge("active_connections", {}, 7);
        server.push_metric_observe("request_latency_ms", {}, 12.5);

        // 端到端：HTTP GET /metrics
        std::string body = http_get("127.0.0.1", port, "/metrics");
        expect_true(!body.empty(), "GET /metrics returns non-empty body");
        // Prometheus text format 检查
        expect_true(body.find("mails_accepted_total 42") != std::string::npos,
                    "body contains counter value 42");
        expect_true(body.find("active_connections 7") != std::string::npos,
                    "body contains gauge value 7");
        // histogram 输出 sum + count 两行
        expect_true(body.find("request_latency_ms_sum") != std::string::npos,
                    "body contains histogram _sum line");
        expect_true(body.find("request_latency_ms_count 1") != std::string::npos,
                    "body contains histogram _count=1 line");

        // /health/live 端点
        std::string health = http_get("127.0.0.1", port, "/health/live");
        expect_true(health == "OK", "/health/live returns OK");

        // 404 路径
        std::string notfound = http_get("127.0.0.1", port, "/nope");
        expect_true(notfound == "Not Found", "unknown path returns Not Found body");

        server.stop_metrics_server();
        io_pool->get_io_context().stop();
        if (io_thread.joinable()) io_thread.join();
        io_pool->stop();
    }

    std::filesystem::remove_all(dir);
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
