#include "mail_system/back/mailServer/pop3_server.h"
#include "mail_system/back/cli/help_text.h"
#include "mail_system/back/common/logger.h"
#include <iostream>
#include <memory>
#include <signal.h>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace mail_system;

namespace {
constexpr const char* kDefaultConfigPath = "config/pop3Config.json";

struct CliOptions {
    std::string config_path = kDefaultConfigPath;
    bool show_help = false;
    bool show_version = false;
};

bool parse_cli_options(int argc, char* argv[], CliOptions& options, std::string& error) {
    bool positional_config_set = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        if (arg.empty()) continue;
        if (arg == "-h" || arg == "--help") { options.show_help = true; return true; }
        if (arg == "-V" || arg == "--version") { options.show_version = true; return true; }
        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                error = "Missing config path after " + arg;
                return false;
            }
            options.config_path = argv[++i];
            positional_config_set = true;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') { error = "Unknown option: " + arg; return false; }
        if (positional_config_set) { error = "Only one config path is allowed"; return false; }
        options.config_path = arg;
        positional_config_set = true;
    }
    return true;
}
} // anonymous namespace

std::unique_ptr<Pop3Server> g_server = nullptr;
sigset_t g_sigset;

int main(int argc, char* argv[]) {
    CliOptions options;
    std::string parse_error;
    if (!parse_cli_options(argc, argv, options, parse_error)) {
        std::cerr << parse_error << "\n\n";
        std::cerr << cli::render_help_text(argv[0]);
        return 2;
    }
    if (options.show_help) { std::cout << cli::render_help_text(argv[0]); return 0; }
    if (options.show_version) { std::cout << cli::render_version_text(); return 0; }

    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    sigaddset(&g_sigset, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &g_sigset, nullptr);

#ifdef GNU_LINUX
    std::cout << "Remember to execute 'sudo setcap 'cap_net_bind_service=+ep' " << argv[0]
              << "' to allow binding to privileged ports without running as root." << std::endl;
#endif

    std::cout << "ProtoRelay POP3 Server starting..." << std::endl;

    try {
        std::string config_path = options.config_path;

        ServerConfig config;
        if (!config.loadFromFile(config_path)) {
            std::cerr << "Failed to load config file: " << config_path << std::endl;
            return 1;
        }

        Logger::get_instance().init(
            config.log_file,
            1024 * 1024 * 5,
            3,
            Logger::string_to_level(config.log_level),
            config.log_to_console,
            config.log_to_file
        );
        LOG_SERVER_INFO("Loaded config file: {}", config_path);
        LOG_SERVER_INFO("POP3 Server configuration:");
        LOG_SERVER_INFO("  System domain: {}", config.system_domain);
        for (auto& l : config.mail_listeners)
            LOG_SERVER_INFO("  Listener: {}:{} auth={}", listener_type_to_string(l.type), l.port,
                           inbound_auth_policy_to_string(l.auth_policy));

        g_server = std::make_unique<Pop3Server>(config);
        g_server->m_configFilePath = config_path;
        g_server->start();

        LOG_SERVER_INFO("POP3 Server running with {} listener(s)", config.mail_listeners.size());
        LOG_SERVER_INFO("Press Ctrl+C to stop, SIGHUP to reload config");

        int sig = 0;
        while (g_server) {
            sigwait(&g_sigset, &sig);
            if (sig == SIGHUP) {
                LOG_SERVER_INFO("SIGHUP reload");
                g_server->reload_config(g_server->m_configFilePath);
                continue;
            }
            break;
        }

        LOG_SERVER_INFO("Shutting down...");
        g_server->request_stop();
        LOG_SERVER_INFO("POP3 Server stopped");

        g_server.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
