// ServerBase 单元测试：生命周期状态、连接计数、入侵检测集成、配置热重载。
//
// 用 NullStorageProvider + 0 线程池构造，免 DB/免真实存储/免线程（ServerBase ctor
// 已把 NullDBPool / NullStorageProvider 接好）。必须链 COMMON_SRCS（同 FSM 测试）。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "framework/server_base.h"
#include "framework/server_config.h"

using namespace mail_system;

namespace {

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
    void start() override {}
    bool should_reject_connection(std::string& reason, const std::string&) const override {
        reason.clear();
        return false;
    }
};

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

    std::filesystem::remove_all(dir);
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
