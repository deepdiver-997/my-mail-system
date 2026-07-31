#include "mail_system/back/mailServer/imaps_server.h"
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

#if ENABLE_DATABASE_QUERY_DEBUG_LOG
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0
#endif
#if ENABLE_IMAP_DETAIL_DEBUG_LOG
#undef ENABLE_IMAP_DETAIL_DEBUG_LOG
#define ENABLE_IMAP_DETAIL_DEBUG_LOG 1
#endif

using namespace mail_system;

namespace {
constexpr const char* kDefaultConfigPath = "config/imapsConfig.json";

struct CliOptions {
    std::string config_path = kDefaultConfigPath;
    bool show_help = false;
    bool show_version = false;
};

bool parse_cli_options(int argc, char* argv[], CliOptions& options, std::string& error) {
    bool positional_config_set = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg.empty()) {
            continue;
        }

        if (arg == "-h" || arg == "--help") {
            options.show_help = true;
            return true;
        }

        if (arg == "-V" || arg == "--version") {
            options.show_version = true;
            return true;
        }

        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                error = "Missing config path after " + arg;
                return false;
            }
            options.config_path = argv[++i];
            positional_config_set = true;
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            error = "Unknown option: " + arg;
            return false;
        }

        if (positional_config_set) {
            error = "Only one config path is allowed";
            return false;
        }

        options.config_path = arg;
        positional_config_set = true;
    }

    return true;
}
} // anonymous namespace

// 全局服务器指针，用于信号处理
std::unique_ptr<ImapsServer> g_server = nullptr;
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
        return 0;
    }

    if (options.show_version) {
        std::cout << cli::render_version_text();
        return 0;
    }

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

#ifdef GNU_LINUX
    std::cout << "Remember to execute 'sudo setcap 'cap_net_bind_service=+ep' " << argv[0]
              << "' to allow binding to privileged ports without running as root." << std::endl;
#endif

    std::cout << "ProtoRelay IMAP Server starting..." << std::endl;

    try {
        std::string config_path = options.config_path;

        // 加载配置
        ServerConfig config;
        if (!config.loadFromFile(config_path)) {
            std::cerr << "Failed to load config file: " << config_path << std::endl;
            return 1;
        }

        // 初始化日志系统
        Logger::get_instance().init(
            config.log_file,
            1024 * 1024 * 5,
            3,
            Logger::string_to_level(config.log_level),
            config.log_to_console,
            config.log_to_file
        );
        LOG_SERVER_INFO("Loaded config file: {}", config_path);
        LOG_SERVER_INFO("IMAP Server configuration:");
        LOG_SERVER_INFO("  System domain: {}", config.system_domain);
        LOG_SERVER_INFO("  Storage: {} @ {}", config.storage_provider, config.mail_storage_path);
        for (auto& l : config.listeners)
            LOG_SERVER_INFO("  Listener: {}:{} auth={}", listener_type_to_string(l.type), l.port,
                           inbound_auth_policy_to_string(l.auth_policy));

        g_server = std::make_unique<ImapsServer>(config);
        g_server->m_configFilePath = config_path;
        g_server->start();

        LOG_SERVER_INFO("IMAPS Server running with {} listener(s)", config.listeners.size());
        LOG_SERVER_INFO("Press Ctrl+C to stop, SIGHUP to reload config");

        // 主循环：condition_variable 等信号，替代忙等
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(g_signal_mutex);
                g_signal_cv.wait_for(lk, std::chrono::seconds(1), []{ return g_signal_flag != 0; });
                if (!g_signal_flag) continue;
            }

            if (g_signal_flag == 2) {
                // SIGHUP → reload config, keep running
                LOG_SERVER_INFO("SIGHUP reload");
                g_signal_flag = 0;
                g_server->reload_config(g_server->m_configFilePath);
                continue;  // 回到 polling 循环
            }

            // SIGINT or SIGTERM → stop
            LOG_SERVER_INFO("Shutting down...");
            g_server->request_stop();  // 原子设状态 Pausing
            break;
        }

        LOG_SERVER_INFO("IMAPS Server stopped");

        // request_stop + reset 触发 ServerBase 析构 → RAII 释放所有资源
        g_server.reset();

        // 等待后台线程退出
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 关闭日志系统
        Logger::get_instance().shutdown();

    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error: {}", e.what());
        std::cerr << "Exception caught in main: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        LOG_SERVER_ERROR("Unknown exception caught in main");
        std::cerr << "Unknown exception in main()" << std::endl;
        return 1;
    }

    return 0;
}
