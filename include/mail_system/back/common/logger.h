#ifndef MAIL_SYSTEM_LOGGER_H
#define MAIL_SYSTEM_LOGGER_H

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace mail_system {

// 日志模块定义
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

// 日志级别转换
inline spdlog::level::level_enum to_spdlog_level(spdlog::level::level_enum level) {
    return level;
}

// 日志系统管理类
class Logger {
public:
    static Logger& get_instance() {
        static Logger instance;
        return instance;
    }

    // 初始化日志系统
    void init(const std::string& log_file = "logs/mail_system.log",
              size_t max_file_size = 1024 * 1024 * 5,
              size_t max_files = 3,
              spdlog::level::level_enum level = spdlog::level::info,
              bool enable_console_sink = true,
              bool enable_file_sink = true) {
        if (m_initialized) {
            return;
        }

        try {
            if (!enable_console_sink && !enable_file_sink) {
                enable_console_sink = true;
            }

            std::vector<spdlog::sink_ptr> sinks;

            if (enable_console_sink) {
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                console_sink->set_level(level);
                // %@ = spdlog::source_loc (file:line), 仅当宏传入 source_loc 时显示
                console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] [%n] %@  %v");
                sinks.push_back(console_sink);
            }

            if (enable_file_sink) {
                std::filesystem::path log_dir(log_file);
                if (!log_dir.parent_path().empty() && !std::filesystem::exists(log_dir.parent_path())) {
                    std::filesystem::create_directories(log_dir.parent_path());
                }

                auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_file, max_file_size, max_files);
                file_sink->set_level(level);
                file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%n] %@  %v");
                sinks.push_back(file_sink);
            }

            // 创建各模块 logger
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

    static spdlog::level::level_enum string_to_level(const std::string& level_str) {
        if (level_str == "trace")    return spdlog::level::trace;
        if (level_str == "debug")    return spdlog::level::debug;
        if (level_str == "info")     return spdlog::level::info;
        if (level_str == "warn")     return spdlog::level::warn;
        if (level_str == "error")    return spdlog::level::err;
        if (level_str == "critical") return spdlog::level::critical;
        if (level_str == "off")      return spdlog::level::off;
        return spdlog::level::info;
    }

    std::shared_ptr<spdlog::logger> get_logger(LogModule module) {
        size_t index = static_cast<size_t>(module);
        if (index < m_loggers.size() && m_loggers[index]) {
            return m_loggers[index];
        }
        return spdlog::default_logger();
    }

    void set_level(spdlog::level::level_enum level) {
        spdlog::set_level(level);
        for (auto& logger : m_loggers) {
            if (logger) logger->set_level(level);
        }
    }

    void set_module_level(LogModule module, spdlog::level::level_enum level) {
        size_t index = static_cast<size_t>(module);
        if (index < m_loggers.size() && m_loggers[index]) {
            m_loggers[index]->set_level(level);
        }
    }

    void flush() {
        for (auto& logger : m_loggers) {
            if (logger) logger->flush();
        }
        spdlog::default_logger()->flush();
    }

    void shutdown() {
        flush();
        spdlog::shutdown();
    }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool m_initialized = false;
    static constexpr size_t kModuleCount = 15;
    std::array<std::shared_ptr<spdlog::logger>, kModuleCount> m_loggers;
};

inline std::shared_ptr<spdlog::logger> log(LogModule module) {
    return Logger::get_instance().get_logger(module);
}

inline void set_log_level(spdlog::level::level_enum level) {
    Logger::get_instance().set_level(level);
}

inline void set_module_log_level(LogModule module, spdlog::level::level_enum level) {
    Logger::get_instance().set_module_level(module, level);
}

} // namespace mail_system

// ==================== 模块化日志宏控制 ====================

#ifndef ENABLE_SERVER_DEBUG_LOG
#define ENABLE_SERVER_DEBUG_LOG 0
#endif
#ifndef ENABLE_NETWORK_DEBUG_LOG
#define ENABLE_NETWORK_DEBUG_LOG 0
#endif
#ifndef ENABLE_DATABASE_DEBUG_LOG
#define ENABLE_DATABASE_DEBUG_LOG 0
#endif
#ifndef ENABLE_DATABASE_QUERY_DEBUG_LOG
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0
#endif
#ifndef ENABLE_SMTP_DEBUG_LOG
#define ENABLE_SMTP_DEBUG_LOG 0
#endif
#ifndef ENABLE_SMTP_DETAIL_DEBUG_LOG
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0
#endif
#ifndef ENABLE_SESSION_DEBUG_LOG
#define ENABLE_SESSION_DEBUG_LOG 0
#endif
#ifndef ENABLE_THREAD_POOL_DEBUG_LOG
#define ENABLE_THREAD_POOL_DEBUG_LOG 0
#endif
#ifndef ENABLE_FILE_IO_DEBUG_LOG
#define ENABLE_FILE_IO_DEBUG_LOG 0
#endif
#ifndef ENABLE_AUTH_DEBUG_LOG
#define ENABLE_AUTH_DEBUG_LOG 0
#endif
#ifndef ENABLE_PERSISTENT_QUEUE_DEBUG_LOG
#define ENABLE_PERSISTENT_QUEUE_DEBUG_LOG 0
#endif
#ifndef ENABLE_OUTBOUND_DEBUG_LOG
#define ENABLE_OUTBOUND_DEBUG_LOG 0
#endif
#ifndef ENABLE_INBOUND_DEBUG_LOG
#define ENABLE_INBOUND_DEBUG_LOG 0
#endif
#ifndef ENABLE_IMAP_DEBUG_LOG
#define ENABLE_IMAP_DEBUG_LOG 0
#endif
#ifndef ENABLE_IMAP_DETAIL_DEBUG_LOG
#define ENABLE_IMAP_DETAIL_DEBUG_LOG 0
#endif

// ==================== source_loc helper ====================
// spdlog::source_loc 提供 __FILE__/__LINE__/__FUNCTION__.
// 仅当 format pattern 包含 %@ (或 %g:%#) 时显示，否则零开销忽略.
#define _SRC_LOC spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}

// ==================== 模块化日志宏定义 ====================
// 每个模块提供: TRACE DEBUG INFO WARN ERROR CRITICAL 六个级别.
// DEBUG/TRACE 在 Release 下编译期消除 (ENABLE_*_DEBUG_LOG=0).

// ── SERVER ──
#define LOG_SERVER_TRACE(...)  if constexpr (ENABLE_SERVER_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SERVER_DEBUG(...)  if constexpr (ENABLE_SERVER_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SERVER_INFO(...)   mail_system::log(mail_system::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_SERVER_WARN(...)   mail_system::log(mail_system::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_SERVER_ERROR(...)  mail_system::log(mail_system::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SERVER_CRITICAL(...) mail_system::log(mail_system::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── NETWORK ──
#define LOG_NETWORK_TRACE(...)  if constexpr (ENABLE_NETWORK_DEBUG_LOG) { mail_system::log(mail_system::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_NETWORK_DEBUG(...)  if constexpr (ENABLE_NETWORK_DEBUG_LOG) { mail_system::log(mail_system::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_NETWORK_INFO(...)   mail_system::log(mail_system::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_NETWORK_WARN(...)   mail_system::log(mail_system::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_NETWORK_ERROR(...)  mail_system::log(mail_system::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_NETWORK_CRITICAL(...) mail_system::log(mail_system::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── DATABASE ──
#define LOG_DATABASE_TRACE(...)  if constexpr (ENABLE_DATABASE_DEBUG_LOG) { mail_system::log(mail_system::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_DATABASE_DEBUG(...)  if constexpr (ENABLE_DATABASE_DEBUG_LOG) { mail_system::log(mail_system::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_DATABASE_INFO(...)   mail_system::log(mail_system::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_DATABASE_WARN(...)   mail_system::log(mail_system::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_DATABASE_ERROR(...)  mail_system::log(mail_system::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_DATABASE_CRITICAL(...) mail_system::log(mail_system::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── DATABASE_QUERY ──
#define LOG_DB_QUERY_TRACE(...)  if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { mail_system::log(mail_system::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_DB_QUERY_DEBUG(...)  if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { mail_system::log(mail_system::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_DB_QUERY_INFO(...)   if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { mail_system::log(mail_system::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__); }
#define LOG_DB_QUERY_WARN(...)   if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { mail_system::log(mail_system::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__); }
#define LOG_DB_QUERY_ERROR(...)  mail_system::log(mail_system::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_DB_QUERY_CRITICAL(...) mail_system::log(mail_system::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── SMTP ──
#define LOG_SMTP_TRACE(...)  if constexpr (ENABLE_SMTP_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SMTP_DEBUG(...)  if constexpr (ENABLE_SMTP_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SMTP_INFO(...)   mail_system::log(mail_system::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_SMTP_WARN(...)   mail_system::log(mail_system::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_SMTP_ERROR(...)  mail_system::log(mail_system::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SMTP_CRITICAL(...) mail_system::log(mail_system::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── SMTP_DETAIL ──
#define LOG_SMTP_DETAIL_TRACE(...)  if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_DEBUG(...)  if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_INFO(...)   if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_WARN(...)   if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_ERROR(...)  mail_system::log(mail_system::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SMTP_DETAIL_CRITICAL(...) mail_system::log(mail_system::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── SESSION ──
#define LOG_SESSION_TRACE(...)  if constexpr (ENABLE_SESSION_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SESSION_DEBUG(...)  if constexpr (ENABLE_SESSION_DEBUG_LOG) { mail_system::log(mail_system::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SESSION_INFO(...)   mail_system::log(mail_system::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_SESSION_WARN(...)   mail_system::log(mail_system::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_SESSION_ERROR(...)  mail_system::log(mail_system::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SESSION_CRITICAL(...) mail_system::log(mail_system::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── PERSISTENT_QUEUE ──
#define LOG_PERSISTENT_QUEUE_TRACE(...)  if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_PERSISTENT_QUEUE_DEBUG(...)  if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_PERSISTENT_QUEUE_INFO(...)   mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_WARN(...)   mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_ERROR(...)  mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_CRITICAL(...) mail_system::log(mail_system::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── OUTBOUND ──
#define LOG_OUTBOUND_TRACE(...)  if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { mail_system::log(mail_system::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_OUTBOUND_DEBUG(...)  if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { mail_system::log(mail_system::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_OUTBOUND_INFO(...)   mail_system::log(mail_system::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_OUTBOUND_WARN(...)   mail_system::log(mail_system::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_OUTBOUND_ERROR(...)  mail_system::log(mail_system::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_OUTBOUND_CRITICAL(...) mail_system::log(mail_system::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── INBOUND ──
#define LOG_INBOUND_TRACE(...)  if constexpr (ENABLE_INBOUND_DEBUG_LOG) { mail_system::log(mail_system::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_INBOUND_DEBUG(...)  if constexpr (ENABLE_INBOUND_DEBUG_LOG) { mail_system::log(mail_system::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_INBOUND_INFO(...)   mail_system::log(mail_system::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_INBOUND_WARN(...)   mail_system::log(mail_system::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_INBOUND_ERROR(...)  mail_system::log(mail_system::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_INBOUND_CRITICAL(...) mail_system::log(mail_system::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── THREAD_POOL ──
#define LOG_THREAD_POOL_TRACE(...)  if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_THREAD_POOL_DEBUG(...)  if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_THREAD_POOL_INFO(...)   mail_system::log(mail_system::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_THREAD_POOL_WARN(...)   mail_system::log(mail_system::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_THREAD_POOL_ERROR(...)  mail_system::log(mail_system::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_THREAD_POOL_CRITICAL(...) mail_system::log(mail_system::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── FILE_IO ──
#define LOG_FILE_IO_TRACE(...)  if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { mail_system::log(mail_system::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_FILE_IO_DEBUG(...)  if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { mail_system::log(mail_system::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_FILE_IO_INFO(...)   mail_system::log(mail_system::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_FILE_IO_WARN(...)   mail_system::log(mail_system::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_FILE_IO_ERROR(...)  mail_system::log(mail_system::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_FILE_IO_CRITICAL(...) mail_system::log(mail_system::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── AUTH ──
#define LOG_AUTH_TRACE(...)  if constexpr (ENABLE_AUTH_DEBUG_LOG) { mail_system::log(mail_system::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_AUTH_DEBUG(...)  if constexpr (ENABLE_AUTH_DEBUG_LOG) { mail_system::log(mail_system::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_AUTH_INFO(...)   mail_system::log(mail_system::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_AUTH_WARN(...)   mail_system::log(mail_system::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_AUTH_ERROR(...)  mail_system::log(mail_system::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_AUTH_CRITICAL(...) mail_system::log(mail_system::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── IMAP ──
#define LOG_IMAP_TRACE(...)  if constexpr (ENABLE_IMAP_DEBUG_LOG) { mail_system::log(mail_system::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_IMAP_DEBUG(...)  if constexpr (ENABLE_IMAP_DEBUG_LOG) { mail_system::log(mail_system::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_IMAP_INFO(...)   mail_system::log(mail_system::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_IMAP_WARN(...)   mail_system::log(mail_system::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_IMAP_ERROR(...)  mail_system::log(mail_system::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_IMAP_CRITICAL(...) mail_system::log(mail_system::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── IMAP_DETAIL ──
#define LOG_IMAP_DETAIL_TRACE(...)  if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_IMAP_DETAIL_DEBUG(...)  if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) { mail_system::log(mail_system::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_IMAP_DETAIL_INFO(...)   mail_system::log(mail_system::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_IMAP_DETAIL_WARN(...)   mail_system::log(mail_system::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_IMAP_DETAIL_ERROR(...)  mail_system::log(mail_system::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_IMAP_DETAIL_CRITICAL(...) mail_system::log(mail_system::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

#endif // MAIL_SYSTEM_LOGGER_H
