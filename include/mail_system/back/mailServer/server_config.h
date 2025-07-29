#ifndef MAIL_SYSTEM_SERVER_CONFIG_H
#define MAIL_SYSTEM_SERVER_CONFIG_H

#include <thread>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>
#include <nlohmann/json.hpp> // JSON库
#include "mail_system/back/db/db_pool.h"

namespace mail_system {

// 服务器配置类
struct ServerConfig {
    // 网络配置
    std::string address;              // 监听地址
    uint16_t port;                    // 监听端口

    bool use_ssl;                     // 是否使用SSL/TLS
    std::string certFile;             // SSL证书文件路径
    std::string keyFile;              // SSL私钥文件路径
    std::string dhFile;               // Diffie-Hellman参数文件路径（可选）

    size_t maxMessageSize;            // 最大消息大小
    size_t maxConnections;            // 最大连接数
    
    // 线程池配置
    size_t io_thread_count;           // IO线程池大小
    size_t worker_thread_count;       // 工作线程池大小
    bool ssl_in_worker;
    
    // 数据库配置
    bool use_database;                // 是否使用数据库
    DBPoolConfig db_pool_config;     // 数据库连接池配置
    
    // 超时配置
    uint32_t connection_timeout;      // 连接超时时间（秒）
    uint32_t read_timeout;           // 读取超时时间（秒）
    uint32_t write_timeout;          // 写入超时时间（秒）
    
    // 安全配置
    bool require_auth;               // 是否要求认证
    size_t max_auth_attempts;        // 最大认证尝试次数
    
    // 日志配置
    std::string log_level;           // 日志级别
    std::string log_file;            // 日志文件路径
    
    ServerConfig()
        : address("0.0.0.0")
        , port(0)
        , use_ssl(false)
        , maxMessageSize(1024 * 1024)  // 1MB
        , maxConnections(1000)
        , io_thread_count(std::thread::hardware_concurrency())
        , worker_thread_count(std::thread::hardware_concurrency())
        , use_database(false)
        , connection_timeout(300)      // 5分钟
        , read_timeout(60)            // 1分钟
        , write_timeout(60)           // 1分钟
        , require_auth(true)
        , max_auth_attempts(3)
        , log_level("info")
    {}

    // 显示配置
    void show() const {
        std::cout << "ServerConfig: "
                  << "\naddress = " << address
                  << "\nport = " << port
                  << "\nuse_ssl = " << (use_ssl ? "true" : "false")
                  << "\ncertFile = " << certFile
                  << "\nkeyFile = " << keyFile
                  << "\ndhFile = " << dhFile
                  << "\nmaxMessageSize = " << maxMessageSize
                  << "\nmaxConnections = " << maxConnections
                  << "\nio_thread_count = " << io_thread_count
                  << "\nworker_thread_count = " << worker_thread_count
                  << "\nuse_database = " << (use_database ? "true" : "false")
                  << std::endl;
        if (use_database) {
            db_pool_config.show();
        }
        std::cout << "\nconnection_timeout = " << connection_timeout
                  << "\nread_timeout = " << read_timeout
                  << "\nwrite_timeout = " << write_timeout
                  << "\nrequire_auth = " << (require_auth ? "true" : "false")
                  << "\nmax_auth_attempts = " << max_auth_attempts
                  << "\nlog_level = " << log_level
                  << "\nlog_file = " << log_file
                  << std::endl;
    }
    
    // 验证配置是否有效
    bool validate() const {
        // 基本验证
        if (port == 0) {
            return false;
        }
        
        // SSL配置验证
        if (use_ssl && (certFile.empty() || keyFile.empty())) {
            return false;
        }
        
        // // 数据库配置验证
        // if (use_database && db_connection_string.empty()) {
        //     return false;
        // }
        
        // 线程池配置验证
        if (io_thread_count == 0 || worker_thread_count == 0) {
            return false;
        }
        
        // 超时配置验证
        if (connection_timeout == 0 || read_timeout == 0 || write_timeout == 0) {
            return false;
        }
        
        return true;
    }
    
    // 从配置文件加载配置
    bool loadFromFile(const std::string& filename) {
        // TODO: 实现从配置文件加载配置的功能
        // 可以支持JSON、YAML或其他格式
        //using json type for example
        std::ifstream config_file(filename);
        if (!config_file.is_open()) {
            std::cerr << "Failed to open config file: " << filename << std::endl;
            return false;
        }

        nlohmann::json json_config;
        config_file >> json_config;

        // 解析JSON配置
        address = json_config.value("address", address);
        port = json_config.value("port", port);
        use_ssl = json_config.value("use_ssl", use_ssl);
        certFile = json_config.value("certFile", certFile);
        keyFile = json_config.value("keyFile", keyFile);
        dhFile = json_config.value("dhFile", dhFile);
        maxMessageSize = json_config.value("maxMessageSize", maxMessageSize);
        maxConnections = json_config.value("maxConnections", maxConnections);
        io_thread_count = json_config.value("io_thread_count", io_thread_count);
        worker_thread_count = json_config.value("worker_thread_count", worker_thread_count);
        use_database = json_config.value("use_database", use_database);
        if (use_database) {
            std::string db_config_file = json_config.value("db_config_file", "");
            db_pool_config.loadFromJson(db_config_file);
        }
        connection_timeout = json_config.value("connection_timeout", connection_timeout);
        read_timeout = json_config.value("read_timeout", read_timeout);
        write_timeout = json_config.value("write_timeout", write_timeout);
        require_auth = json_config.value("require_auth", require_auth);
        max_auth_attempts = json_config.value("max_auth_attempts", max_auth_attempts);
        log_level = json_config.value("log_level", log_level);
        log_file = json_config.value("log_file", log_file);

        return true;
    }
    
    // 保存配置到文件
    bool saveToFile(const std::string& filename) const {
        // TODO: 实现保存配置到文件的功能
        return true;
    }
};

} // namespace mail_system

#endif // MAIL_SYSTEM_SERVER_CONFIG_H