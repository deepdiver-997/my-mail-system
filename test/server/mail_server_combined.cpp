/**
 * ProtoRelay Combined Mail Server
 *
 * Runs SMTP(S) + IMAP(S) simultaneously, sharing thread pools and database connection pool.
 *
 * Usage:
 *   mail_server_combined -s config/smtpsConfig.json
 *
 * Flags:
 *   -s, --smtp-config   SMTP 配置文件路径（默认 config/smtpsConfig.json）
 *   -i, --imap-config   IMAP 配置文件路径（默认同 SMTP 配置）
 *   -h, --help          显示帮助
 *   -V, --version       显示版本
 */

#include "mail_system/back/mailServer/smtps_server.h"
#include "mail_system/back/mailServer/imaps_server.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/distributed_mysql_pool.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/cli/help_text.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>
#include <signal.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdlib>

using namespace mail_system;

// ====================================================================
// 默认配置文件路径
// ====================================================================
namespace {
constexpr const char* kDefaultSmtpConfigPath = "config/smtpsConfig.json";
constexpr const char* kDefaultImapConfigPath = "config/smtpsConfig.json";

struct CliOptions {
    std::string smtp_config_path = kDefaultSmtpConfigPath;
    std::string imap_config_path = kDefaultImapConfigPath;
    bool show_help = false;
    bool show_version = false;
};

bool parse_cli_options(int argc, char* argv[], CliOptions& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg.empty()) continue;

        if (arg == "-h" || arg == "--help") {
            options.show_help = true;
            return true;
        }
        if (arg == "-V" || arg == "--version") {
            options.show_version = true;
            return true;
        }
        if ((arg == "-s" || arg == "--smtp-config") && i + 1 < argc) {
            options.smtp_config_path = argv[++i];
            continue;
        }
        if ((arg == "-i" || arg == "--imap-config") && i + 1 < argc) {
            options.imap_config_path = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            error = "Unknown option: " + arg;
            return false;
        }
    }
    return true;
}

// 创建数据库连接池（根据配置类型）
std::shared_ptr<DBPool> create_db_pool(const ServerConfig& config) {
    if (!config.use_database) {
        return nullptr;
    }

    try {
        std::shared_ptr<DBPool> pool;
        if (config.db_pool_config.achieve == "mysql_distributed") {
            pool = DistributedMySQLPoolFactory::get_instance().create_pool(
                config.db_pool_config,
                std::make_shared<MySQLService>());
        } else {
            pool = MySQLPoolFactory::get_instance().create_pool(
                config.db_pool_config,
                std::make_shared<MySQLService>());
        }
        if (pool) {
            LOG_SERVER_INFO("Database pool created successfully ({}: {})",
                           config.db_pool_config.achieve,
                           config.system_name);
        } else {
            LOG_SERVER_ERROR("Failed to create database pool");
        }
        return pool;
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Exception creating DB pool: {}", e.what());
        return nullptr;
    }
}

} // anonymous namespace

// ====================================================================
// 全局共享资源
// ====================================================================
struct SharedServerResources {
    std::shared_ptr<ThreadPoolBase> io_pool;
    std::shared_ptr<ThreadPoolBase> work_pool;
    std::shared_ptr<DBPool> db_pool;
};

// 全局服务器实例（信号处理用）
std::shared_ptr<SmtpsServer> g_smtp_server = nullptr;
std::shared_ptr<ImapsServer> g_imap_server = nullptr;
SharedServerResources g_resources;

// 信号安全：只写 volatile sig_atomic_t，让主线程轮询
volatile sig_atomic_t g_signal_flag = 0;
std::mutex g_signal_mutex;
std::condition_variable g_signal_cv;

void signal_handler(int signal) {
    g_signal_flag = (signal == SIGHUP) ? 2 : 1;
    g_signal_cv.notify_one();
}

int main(int argc, char* argv[]) {
    CliOptions options;
    std::string parse_error;
    if (!parse_cli_options(argc, argv, options, parse_error)) {
        std::cerr << parse_error << "\n\n";
        std::cerr << cli::render_help_text(argv[0]);
        return 2;
    }

    if (options.show_help) {
        std::cout << cli::render_help_text(argv[0]);
        std::cout << "\nCombined server options:\n"
                  << "  -s, --smtp-config FILE   SMTP config path (default: "
                  << kDefaultSmtpConfigPath << ")\n"
                  << "  -i, --imap-config FILE   IMAP config path (default: "
                  << kDefaultImapConfigPath << ")\n";
        return 0;
    }

    if (options.show_version) {
        std::cout << cli::render_version_text();
        return 0;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

#ifdef GNU_LINUX
    std::cout << "Remember to execute 'sudo setcap 'cap_net_bind_service=+ep' " << argv[0]
              << "' to allow binding to privileged ports without running as root." << std::endl;
#endif

    std::cout << "ProtoRelay Combined Mail Server starting..." << std::endl;

    try {
        // ================================================================
        // 1. 加载 SMTP 配置（作为主配置，用于初始化日志和线程池）
        // ================================================================
        ServerConfig smtp_config;
        if (!smtp_config.loadFromFile(options.smtp_config_path)) {
            std::cerr << "Failed to load SMTP config: " << options.smtp_config_path << std::endl;
            return 1;
        }

        // ================================================================
        // 2. 加载 IMAP 配置
        // ================================================================
        ServerConfig imap_config;
        if (!imap_config.loadFromFile(options.imap_config_path)) {
            std::cerr << "Failed to load IMAP config: " << options.imap_config_path << std::endl;
            return 1;
        }

        // ================================================================
        // 3. 初始化日志系统（使用 SMTP 配置中的日志设置）
        // ================================================================
        Logger::get_instance().init(
            smtp_config.log_file,
            1024 * 1024 * 5,
            3,
            Logger::string_to_level(smtp_config.log_level),
            smtp_config.log_to_console,
            smtp_config.log_to_file
        );

        LOG_SERVER_INFO("=== ProtoRelay Combined Server ===");
        LOG_SERVER_INFO("SMTP config: {}", options.smtp_config_path);
        LOG_SERVER_INFO("IMAP config: {}", options.imap_config_path);

        // ================================================================
        // 4. 创建共享基础设施
        //    线程池和数据库连接池在两个服务器间共享
        // ================================================================
        auto io_pool = std::make_shared<IOThreadPool>(smtp_config.io_thread_count);
        io_pool->start();
        LOG_SERVER_INFO("Shared IO thread pool started ({} threads)", smtp_config.io_thread_count);

        auto work_pool = std::make_shared<BoostThreadPool>(smtp_config.worker_thread_count);
        work_pool->start();
        LOG_SERVER_INFO("Shared worker thread pool started ({} threads)", smtp_config.worker_thread_count);

        auto db_pool = create_db_pool(smtp_config);
        if (smtp_config.use_database && !db_pool) {
            LOG_SERVER_WARN("Database pool is null — some features may be unavailable");
        }

        g_resources = {io_pool, work_pool, db_pool};

        // ================================================================
        // 5. 创建 SMTP 服务器
        //    内部会创建 PersistentQueue + OutboundClient
        // ================================================================
        LOG_SERVER_INFO("Creating SMTP server...");
        g_smtp_server = std::make_shared<SmtpsServer>(
            smtp_config, io_pool, work_pool, db_pool);
        g_smtp_server->m_configFilePath = options.smtp_config_path;
        LOG_SERVER_INFO("SMTP server created with {} listener(s)", smtp_config.mail_listeners.size());

        LOG_SERVER_INFO("Creating IMAP server...");
        g_imap_server = std::make_shared<ImapsServer>(
            imap_config, io_pool, work_pool, db_pool);
        g_imap_server->m_configFilePath = options.imap_config_path;
        LOG_SERVER_INFO("IMAP server created with {} listener(s)", imap_config.mail_listeners.size());

        LOG_SERVER_INFO("Starting SMTP server...");
        g_smtp_server->start();

        LOG_SERVER_INFO("Starting IMAP server...");
        g_imap_server->start();

        LOG_SERVER_INFO("");
        LOG_SERVER_INFO("=== ProtoRelay Combined Server is RUNNING ===");
        for (auto& l : smtp_config.mail_listeners)
            LOG_SERVER_INFO("  SMTP: {}:{} auth={}", listener_type_to_string(l.type), l.port,
                           inbound_auth_policy_to_string(l.auth_policy));
        for (auto& l : imap_config.mail_listeners)
            LOG_SERVER_INFO("  IMAP: {}:{} auth={}", listener_type_to_string(l.type), l.port,
                           inbound_auth_policy_to_string(l.auth_policy));
        if (smtp_config.metrics_enabled) {
            LOG_SERVER_INFO("  Metrics:  http://{}:{}", smtp_config.metrics_bind_address, smtp_config.metrics_port);
        }
        LOG_SERVER_INFO("Press Ctrl+C to stop all servers.");

        // ================================================================
        // 8. 主循环：轮询信号标志，安全关闭
        // ================================================================
        {
            std::unique_lock<std::mutex> lk(g_signal_mutex);
            g_signal_cv.wait(lk, []{ return g_signal_flag != 0; });
        }

        if (g_signal_flag == 2) {
            LOG_SERVER_INFO("SIGHUP reload (SMTP only)");
            g_signal_flag = 0;
            if (g_smtp_server) {
                bool ok = g_smtp_server->reload_config(g_smtp_server->m_configFilePath);
                LOG_SERVER_INFO("SMTP config reload: {}", ok ? "success" : "failed");
            }
        }

        if (g_signal_flag == 1 || !g_smtp_server || !g_imap_server) {
            LOG_SERVER_INFO("Shutting down...");
            // request_stop 只设原子状态，然后 run() 检测到后退出
            // reset() 触发析构链，在正常上下文中释放所有资源
            if (g_smtp_server) g_smtp_server->request_stop();
            if (g_imap_server) g_imap_server->request_stop();
            g_smtp_server.reset();
            g_imap_server.reset();
        }

        LOG_SERVER_INFO("");
        LOG_SERVER_INFO("All servers stopped. Cleaning up...");

        // ================================================================
        // 9. 清理
        // ================================================================
        g_smtp_server.reset();
        g_imap_server.reset();

        // 停止共享线程池
        io_pool->stop();
        work_pool->stop();
        LOG_SERVER_INFO("Shared thread pools stopped");

        // 等待后台线程完全退出
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 关闭日志
        Logger::get_instance().shutdown();
        LOG_SERVER_INFO("ProtoRelay Combined Server shut down cleanly");

    } catch (const std::exception& e) {
        std::cerr << "Fatal exception: " << e.what() << std::endl;
        // 尽力清理
        g_smtp_server.reset();
        g_imap_server.reset();
        if (g_resources.io_pool) g_resources.io_pool->stop();
        if (g_resources.work_pool) g_resources.work_pool->stop();
        try { Logger::get_instance().shutdown(); } catch (...) {}
        return 1;
    } catch (...) {
        std::cerr << "Fatal unknown exception" << std::endl;
        g_smtp_server.reset();
        g_imap_server.reset();
        return 1;
    }

    return 0;
}
