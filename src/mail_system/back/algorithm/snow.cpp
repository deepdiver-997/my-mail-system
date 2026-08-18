#include "mail_system/back/algorithm/snow.h"
#include <stdexcept>
#include <string>

namespace mail_system {
namespace algorithm {

SnowflakeGenerator::SnowflakeGenerator(uint16_t worker_id, uint16_t datacenter_id)
    : m_worker_id(worker_id & MAX_WORKER_ID)
    , m_datacenter_id(datacenter_id & MAX_DATACENTER_ID) {
    if (worker_id > MAX_WORKER_ID) {
        throw std::invalid_argument("worker_id can't be greater than " + std::to_string(MAX_WORKER_ID));
    }
    if (datacenter_id > MAX_DATACENTER_ID) {
        throw std::invalid_argument("datacenter_id can't be greater than " + std::to_string(MAX_DATACENTER_ID));
    }
}

int64_t SnowflakeGenerator::current_millis() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

int64_t SnowflakeGenerator::til_next_millis(int64_t last_timestamp) const {
    int64_t timestamp = current_millis();
    while (timestamp <= last_timestamp) {
        timestamp = current_millis();
    }
    return timestamp;
}

int64_t SnowflakeGenerator::next_id() {
    std::lock_guard<std::mutex> lock(m_mutex);

    int64_t timestamp = current_millis();

    // 时钟回拨检测
    if (timestamp < m_last_timestamp) {
        throw std::runtime_error("Clock moved backwards. Refusing to generate id");
    }

    if (timestamp == m_last_timestamp) {
        // 同一毫秒内，序列号自增
        m_sequence = (m_sequence + 1) & SEQUENCE_MASK;
        if (m_sequence == 0) {
            // 序列号溢出，等待下一毫秒
            timestamp = til_next_millis(m_last_timestamp);
        }
    } else {
        // 新的毫秒，序列号重置为0
        m_sequence = 0;
    }

    m_last_timestamp = timestamp;

    // 组装ID
    int64_t id = ((timestamp - TWEPOCH) << TIMESTAMP_LEFT_SHIFT)
                | (m_datacenter_id << DATACENTER_ID_SHIFT)
                | (m_worker_id << WORKER_ID_SHIFT)
                | m_sequence;

    m_generated_count.fetch_add(1, std::memory_order_relaxed);
    return id;
}

// 全局默认生成器实现
SnowflakeGenerator& get_snowflake_generator(uint16_t worker_id, uint16_t datacenter_id) {
    static SnowflakeGenerator generator(worker_id, datacenter_id);
    return generator;
}

} // namespace algorithm
} // namespace mail_system
