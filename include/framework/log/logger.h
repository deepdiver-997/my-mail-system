#ifndef PR_FRAMEWORK_LOG_LOGGER_H
#define PR_FRAMEWORK_LOG_LOGGER_H

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace pr {

// ================================================================
// LogModule — 日志模块枚举
// 应用层通过注册模块来扩展此枚举，框架只定义基本常量
// ================================================================
enum class LogModule {
    SERVER,           // 服务器基础日志
    NETWORK,          // 网络连接日志
    DATABASE,         // 数据库连接池日志
    DATABASE_QUERY,   // 数据库查询详细日志
    SMTP,             // SMTP 协议日志
    SMTP_DETAIL,      // SMTP 详细状态机日志
    SESSION,          // 会话管理日志
    PERSISTENT_QUEUE, // 持久化队列日志
    OUTBOUND,         // 出站投递日志
    INBOUND,          // 入站验证日志 (SPF/DKIM/DMARC)
    THREAD_POOL,      // 线程池日志
    FILE_IO,          // 文件 I/O 日志
    AUTH,             // 认证日志
    IMAP,             // IMAP 协议日志
    IMAP_DETAIL       // IMAP 详细状态机日志
};

// ================================================================
// Logger — 框架级日志系统管理类 (Meyer's Singleton)
//
// 管理 15 个模块级 spdlog logger，共享同一组 sink。
// 初始化后所有模块可通过 pr::log(module) 获取对应 logger。
// ================================================================
class Logger {
public:
    static Logger& get_instance() {
        static Logger instance;
        return instance;
    }

    void init(const std::string& log_file = "logs/mail_system.log",
              size_t max_file_size = 1024 * 1024 * 5,
              size_t max_files = 3,
              spdlog::level::level_enum level = spdlog::level::info,
              bool enable_console_sink = true,
              bool enable_file_sink = true) {
        if (m_initialized) return;

        try {
            if (!enable_console_sink && !enable_file_sink)
                enable_console_sink = true;

            std::vector<spdlog::sink_ptr> sinks;

            if (enable_console_sink) {
                auto cs = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                cs->set_level(level);
                cs->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] [%n] %@  %v");
                sinks.push_back(cs);
            }

            if (enable_file_sink) {
                std::filesystem::path lp(log_file);
                if (!lp.parent_path().empty() && !std::filesystem::exists(lp.parent_path()))
                    std::filesystem::create_directories(lp.parent_path());
                auto fs = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_file, max_file_size, max_files);
                fs->set_level(level);
                fs->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%n] %@  %v");
                sinks.push_back(fs);
            }

            struct { LogModule m; const char* name; } mods[] = {
                {LogModule::SERVER,           "SERVER"},
                {LogModule::NETWORK,          "NETWORK"},
                {LogModule::DATABASE,         "DATABASE"},
                {LogModule::DATABASE_QUERY,   "DB_QUERY"},
                {LogModule::SMTP,             "SMTP"},
                {LogModule::SMTP_DETAIL,      "SMTP_DETAIL"},
                {LogModule::SESSION,          "SESSION"},
                {LogModule::PERSISTENT_QUEUE, "PERSISTENT_QUEUE"},
                {LogModule::OUTBOUND,         "OUTBOUND"},
                {LogModule::INBOUND,          "INBOUND"},
                {LogModule::THREAD_POOL,      "THREAD_POOL"},
                {LogModule::FILE_IO,          "FILE_IO"},
                {LogModule::AUTH,             "AUTH"},
                {LogModule::IMAP,             "IMAP"},
                {LogModule::IMAP_DETAIL,      "IMAP_DETAIL"},
            };
            for (auto& mod : mods) {
                m_loggers[static_cast<size_t>(mod.m)] =
                    std::make_shared<spdlog::logger>(mod.name, sinks.begin(), sinks.end());
            }

            spdlog::set_default_logger(m_loggers[static_cast<size_t>(LogModule::SERVER)]);
            spdlog::set_level(level);
            spdlog::flush_on(spdlog::level::warn);
            m_initialized = true;
            spdlog::info("Logger initialized");
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Log init failed: " << ex.what() << std::endl;
        }
    }

    static spdlog::level::level_enum string_to_level(const std::string& s) {
        if (s == "trace")    return spdlog::level::trace;
        if (s == "debug")    return spdlog::level::debug;
        if (s == "info")     return spdlog::level::info;
        if (s == "warn")     return spdlog::level::warn;
        if (s == "error")    return spdlog::level::err;
        if (s == "critical") return spdlog::level::critical;
        if (s == "off")      return spdlog::level::off;
        return spdlog::level::info;
    }

    std::shared_ptr<spdlog::logger> get_logger(LogModule m) {
        size_t i = static_cast<size_t>(m);
        if (i < m_loggers.size() && m_loggers[i]) return m_loggers[i];
        return spdlog::default_logger();
    }

    void set_level(spdlog::level::level_enum lv) {
        spdlog::set_level(lv);
        for (auto& lg : m_loggers) if (lg) lg->set_level(lv);
    }

    void set_module_level(LogModule m, spdlog::level::level_enum lv) {
        size_t i = static_cast<size_t>(m);
        if (i < m_loggers.size() && m_loggers[i]) m_loggers[i]->set_level(lv);
    }

    void flush() {
        for (auto& lg : m_loggers) if (lg) lg->flush();
        spdlog::default_logger()->flush();
    }

    void shutdown() { flush(); spdlog::shutdown(); }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool m_initialized = false;
    static constexpr size_t kModuleCount = 15;
    std::array<std::shared_ptr<spdlog::logger>, kModuleCount> m_loggers;
};

// ---- 便捷函数 ----
inline std::shared_ptr<spdlog::logger> log(LogModule m) {
    return Logger::get_instance().get_logger(m);
}
inline void set_log_level(spdlog::level::level_enum lv) {
    Logger::get_instance().set_level(lv);
}
inline void set_module_log_level(LogModule m, spdlog::level::level_enum lv) {
    Logger::get_instance().set_module_level(m, lv);
}

} // namespace pr

#endif // PR_FRAMEWORK_LOG_LOGGER_H
