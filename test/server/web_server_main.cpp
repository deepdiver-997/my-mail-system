// ──────────────────────────────────────────────────────────────────
// web_server_main — HTTP/1.1 静态文件服务器入口
//
// 用法:  web_server_main [doc_root] [port]
//       默认  doc_root=./config/web  port=8080
// 对照 smtps_test 的 main：少配置加载，直接程序化填 ServerConfig。
// ──────────────────────────────────────────────────────────────────
#include "web_server/http_server.h"
#include "mail_system/back/common/logger.h"
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

using namespace mail_system;

static sigset_t g_sigset;
static std::unique_ptr<web_server::HttpServer> g_server;

int main(int argc, char* argv[]) {
    std::string doc_root = argc > 1 ? argv[1] : "./config/web";
    int port = argc > 2 ? std::atoi(argv[2]) : 8080;

    // 信号处理：阻塞 → sigwait 接管（同 smtps_test）
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &g_sigset, nullptr);

    pr::Logger::get_instance().init("", 1024 * 1024 * 5, 3,
                                    pr::Logger::string_to_level("info"),
                                    /*console=*/true, /*file=*/false);

    try {
        ServerConfig config;
        config.address = "0.0.0.0";
        config.maxConnections = 10000;
        config.listeners.emplace_back(pr::ListenerType::TCP, static_cast<uint16_t>(port));
        // ServerBase 初始化总会装配本地存储，给两个非空路径避免 create_directories("")
        config.storage.provider = "local";
        config.storage.local.mail_path = "config/storage/mail";
        config.storage.local.attachment_path = "config/storage/attachments";

        std::cout << "Starting HTTP/1.1 static server on :" << port
                  << "  doc_root=" << doc_root << "\n";

        g_server = std::make_unique<web_server::HttpServer>(config, doc_root);
        g_server->start();

        LOG_SERVER_INFO("HTTP server running. Ctrl+C to stop.");
        std::cout << "  try: curl -v http://localhost:" << port << "/index.html\n";

        int sig = 0;
        while (g_server) {
            sigwait(&g_sigset, &sig);
            break;   // SIGINT / SIGTERM
        }

        LOG_SERVER_INFO("Shutting down...");
        g_server->request_stop();
        g_server.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        pr::Logger::get_instance().shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}