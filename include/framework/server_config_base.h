#ifndef PR_FRAMEWORK_SERVER_CONFIG_BASE_H
#define PR_FRAMEWORK_SERVER_CONFIG_BASE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <fstream>
#include <thread>
#include <filesystem>
#include <nlohmann/json.hpp>

// ================================================================
// ServerConfig — 框架级服务器配置基类
//
// 抽象所有基于 TCP/SSL 的服务器的公共配置项：
// 地址、监听器、SSL 证书、线程数、超时、日志设置。
// 应用层通过继承扩展业务特有的配置（DB、存储、路由等）。
// ================================================================
namespace pr {

enum class ListenerType : int { TCP = 0, SSL = 1 };

inline const char* listener_type_to_string(ListenerType t) {
    return t == ListenerType::SSL ? "ssl" : "tcp";
}

inline ListenerType listener_type_from_string(const std::string& s) {
    return s == "ssl" ? ListenerType::SSL : ListenerType::TCP;
}

// InboundAuthPolicy — 监听器认证策略 (框架级, 任何多端口服务器都可能需要)
enum class InboundAuthPolicy : int { OFF = 0, AUTO = 1, ON = 2 };

struct ListenerConfig {
    ListenerType type = ListenerType::TCP;
    uint16_t port = 0;
    InboundAuthPolicy auth_policy = InboundAuthPolicy::OFF;

    ListenerConfig() = default;
    ListenerConfig(ListenerType t, uint16_t p) : type(t), port(p) {}
};

struct ServerConfig {
    std::string address;

    std::vector<ListenerConfig> listeners;

    std::string certFile;
    std::string keyFile;
    std::string dhFile;

    size_t maxMessageSize;
    size_t maxConnections;

    size_t io_thread_count;
    size_t worker_thread_count;

    uint32_t connection_timeout;
    uint32_t read_timeout;
    uint32_t write_timeout;

    size_t max_auth_attempts;

    std::string log_level;
    std::string log_file;
    bool log_to_console;
    bool log_to_file;

    ServerConfig()
        : address("0.0.0.0")
        , maxMessageSize(1024 * 1024)
        , maxConnections(1000)
        , io_thread_count(std::thread::hardware_concurrency())
        , worker_thread_count(std::thread::hardware_concurrency())
        , connection_timeout(300)
        , read_timeout(60)
        , write_timeout(60)
        , max_auth_attempts(3)
        , log_level("info")
        , log_to_console(true)
        , log_to_file(true)
    {}

    ServerConfig(const ServerConfig&) = default;
    virtual ~ServerConfig() = default;

    // ---- 便捷方法 ----
    std::unordered_map<uint16_t, ListenerConfig> listener_map() const {
        std::unordered_map<uint16_t, ListenerConfig> m;
        for (auto& l : listeners) m[l.port] = l;
        return m;
    }

    const ListenerConfig* find_listener(uint16_t port) const {
        for (auto& l : listeners) if (l.port == port) return &l;
        return nullptr;
    }

    static std::string resolve_path(const std::string& config_path,
                                    const std::string& relative_path) {
        if (relative_path.empty()) return "";
        auto dir = std::filesystem::path(config_path).parent_path();
        return std::filesystem::absolute((dir / relative_path).lexically_normal()).string();
    }

    // ---- 序列化 (子类应重写以扩展) ----
    virtual bool loadFromJson(const nlohmann::json& j, const std::string& base_dir) {
        if (j.contains("listeners") && j["listeners"].is_array()) {
            listeners.clear();
            for (auto& item : j["listeners"]) {
                ListenerConfig lc;
                lc.type = listener_type_from_string(item.value("type", "tcp"));
                lc.port = static_cast<uint16_t>(item.value("port", 0));
                if (lc.port != 0) listeners.push_back(lc);
            }
        }

        address             = j.value("address", address);
        maxMessageSize      = j.value("maxMessageSize", maxMessageSize);
        maxConnections      = j.value("maxConnections", maxConnections);
        io_thread_count     = j.value("io_thread_count", io_thread_count);
        worker_thread_count = j.value("worker_thread_count", worker_thread_count);
        connection_timeout  = j.value("connection_timeout", connection_timeout);
        read_timeout        = j.value("read_timeout", read_timeout);
        write_timeout       = j.value("write_timeout", write_timeout);
        max_auth_attempts   = j.value("max_auth_attempts", max_auth_attempts);
        log_level           = j.value("log_level", log_level);
        log_file            = resolve_path(base_dir, j.value("log_file", log_file));
        log_to_console      = j.value("log_to_console", log_to_console);
        log_to_file         = j.value("log_to_file", log_to_file);

        certFile = resolve_path(base_dir, j.value("certFile", certFile));
        keyFile  = resolve_path(base_dir, j.value("keyFile", keyFile));
        dhFile   = resolve_path(base_dir, j.value("dhFile", dhFile));

        return true;
    }

    // ---- 校验 (子类应重写以扩展) ----
    virtual bool validate() const {
        if (listeners.empty()) return false;
        bool has_ssl = false;
        for (auto& l : listeners) {
            if (l.port == 0) return false;
            if (l.type == ListenerType::SSL) has_ssl = true;
        }
#ifdef USE_SSL
        if (has_ssl && (certFile.empty() || keyFile.empty())) return false;
#endif
        (void)has_ssl;
        if (io_thread_count == 0 || worker_thread_count == 0) return false;
        if (connection_timeout == 0 || read_timeout == 0 || write_timeout == 0) return false;
        return true;
    }
};

} // namespace pr

#endif // PR_FRAMEWORK_SERVER_CONFIG_BASE_H
