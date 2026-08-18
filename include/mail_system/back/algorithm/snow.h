#pragma once
#include <cstdint>
#include <chrono>
#include <mutex>
#include <atomic>

namespace mail_system {
namespace algorithm {

/**
 * @brief 雪花算法ID生成器
 *
 * 结构：时间戳(41位) + 机器ID(10位) + 序列号(12位)
 * - 时间戳：从自定义epoch开始的毫秒数
 * - 机器ID：10位可支持1024台机器
 * - 序列号：12位每毫秒可生成4096个ID
 */
class SnowflakeGenerator {
public:
    /**
     * @brief 构造函数
     * @param worker_id 机器ID/工作ID (0-1023)
     * @param datacenter_id 数据中心ID (0-31)，兼容原标准
     */
    explicit SnowflakeGenerator(uint16_t worker_id = 0, uint16_t datacenter_id = 0);

    /**
     * @brief 生成下一个雪花ID
     * @return 64位雪花ID
     */
    int64_t next_id();

    /**
     * @brief 获取当前生成的ID数量
     */
    uint64_t get_generated_count() const { return m_generated_count; }

private:
    // 常量定义
    static constexpr int64_t TWEPOCH = 1288834974657L; // 起始时间戳
    static constexpr int64_t WORKER_ID_BITS = 5L;
    static constexpr int64_t DATACENTER_ID_BITS = 5L;
    // 位掩码用 (1L<<n)-1（C++17 常量表达式）；避免 -1L<<n（C++20 才定义）
    static constexpr int64_t MAX_WORKER_ID = (1L << WORKER_ID_BITS) - 1;
    static constexpr int64_t MAX_DATACENTER_ID = (1L << DATACENTER_ID_BITS) - 1;
    static constexpr int64_t SEQUENCE_BITS = 12L;

    static constexpr int64_t WORKER_ID_SHIFT = SEQUENCE_BITS;
    static constexpr int64_t DATACENTER_ID_SHIFT = SEQUENCE_BITS + WORKER_ID_BITS;
    static constexpr int64_t TIMESTAMP_LEFT_SHIFT = SEQUENCE_BITS + WORKER_ID_BITS + DATACENTER_ID_BITS;
    static constexpr int64_t SEQUENCE_MASK = (1L << SEQUENCE_BITS) - 1;

    // 成员变量
    std::mutex m_mutex;
    int64_t m_last_timestamp = -1;
    int64_t m_sequence = 0;
    uint16_t m_worker_id;
    uint16_t m_datacenter_id;
    std::atomic<uint64_t> m_generated_count{0};

    /**
     * @brief 获取当前毫秒时间戳
     */
    int64_t current_millis() const;

    /**
     * @brief 等待下一毫秒
     */
    int64_t til_next_millis(int64_t last_timestamp) const;
};

// 全局默认生成器（单例模式）
SnowflakeGenerator& get_snowflake_generator(uint16_t worker_id = 0, uint16_t datacenter_id = 0);

} // namespace algorithm
} // namespace mail_system