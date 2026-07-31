#ifndef PR_FRAMEWORK_LOG_LOG_PURE_H
#define PR_FRAMEWORK_LOG_LOG_PURE_H

#include <spdlog/fmt/fmt.h>
#include <cstdio>
#include <chrono>
#include <string>
#include <filesystem>
#include <utility>

namespace pr {

// ================================================================
// LOG_PURE — 日志压缩运行时
//
// 配合 tools/log_transform.py 使用。
// 构建时脚本将 LOG_* 宏替换为 LOG_PURE(hash, args..., ts)。
// 运行时输出 "0xhash|arg1|...|ts_ms\n" 到专用纯日志文件。
//
// hash = shake128([LEVEL][MODULE]fmt) — 稳定，不含文件/行号
// 映射表增量维护，后处理用 tools/log_restore.py 还原可读日志。
// ================================================================

/** 获取当前毫秒时间戳，LOG_PURE 自动注入 */
inline uint64_t log_pure_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

namespace {
    std::string _log_pure_path = "logs/pure.log";
}

/** 设置纯日志输出文件路径 (须在首次写日志前调用) */
inline void log_pure_init(const std::string& path = "logs/pure.log") {
    _log_pure_path = path;
}

/** 写纯日志行: "0xhash|arg1|...|argN\n" (行缓冲, 崩溃不丢) */
template <typename... Args>
inline void log_pure_write(uint64_t hash, Args&&... args) {
    static thread_local FILE* s_fp = nullptr;
    static thread_local std::string s_last_path;
    if (!s_fp || s_last_path != _log_pure_path) {
        if (s_fp) std::fclose(s_fp);
        std::filesystem::path p(_log_pure_path);
        if (!p.parent_path().empty())
            std::filesystem::create_directories(p.parent_path());
        s_fp = std::fopen(_log_pure_path.c_str(), "a");
        s_last_path = _log_pure_path;
        if (s_fp) std::setvbuf(s_fp, nullptr, _IOLBF, 0);
    }
    if (!s_fp) return;
    fmt::print(s_fp, "0x{:016x}", hash);
    ((fmt::print(s_fp, "|{}", std::forward<Args>(args))), ...);
    fmt::print(s_fp, "\n");
}

inline void log_pure_flush() {
    static thread_local FILE* s_fp = nullptr;
    if (!s_fp) {
        s_fp = std::fopen(_log_pure_path.c_str(), "a");
        if (s_fp) std::setvbuf(s_fp, nullptr, _IOLBF, 0);
    }
    if (s_fp) std::fflush(s_fp);
}

} // namespace pr

// ---- 宏定义 ----

// LOG_PURE(hash, args...) → "0xhash|arg1|...|ts\n"
#define LOG_PURE(hash, ...) \
    pr::log_pure_write(hash, ##__VA_ARGS__)

// ================================================================
// LOG_LOOK_UP 模式 —— 预处理器分析阶段
//
// 当通过 -DLOG_LOOK_UP 编译时，所有 LOG_* 宏展开为带有 \001 分隔符
// 的标记字符串。tools/log_transform.py 运行 gcc -E 解析这些标记。
// ================================================================
#ifdef LOG_LOOK_UP
#define __LOG_MARK(module, level, fmt, ...) \
    (void)("LUK" "\001" module "\001" level "\001" fmt)
#else
#define __LOG_MARK(module, level, fmt, ...)  /* no-op in normal mode */
#endif

#endif // PR_FRAMEWORK_LOG_LOG_PURE_H
