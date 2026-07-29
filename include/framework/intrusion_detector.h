#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <chrono>
#include <atomic>
#include <nlohmann/json.hpp>

namespace mail_system {

struct IpRecord {
    int      failed_count = 0;
    int      total_connections = 0;
    uint64_t first_seen = 0;
    uint64_t last_seen  = 0;

    nlohmann::json to_json() const {
        return {
            {"failed_count", failed_count},
            {"total_connections", total_connections},
            {"first_seen", first_seen},
            {"last_seen", last_seen}
        };
    }

    static IpRecord from_json(const nlohmann::json& j) {
        IpRecord r;
        r.failed_count      = j.value("failed_count", 0);
        r.total_connections = j.value("total_connections", 0);
        r.first_seen        = j.value("first_seen", 0ULL);
        r.last_seen         = j.value("last_seen", 0ULL);
        return r;
    }
};

class IntrusionDetector {
public:
    explicit IntrusionDetector(std::string data_file = "logs/intrusion_data.json")
        : m_data_file(std::move(data_file)), m_enabled(false),
          m_max_records(10000), m_ban_threshold(0),
          m_persist_interval_sec(60), m_persist_dirty_threshold(256),
          m_records_since_persist(0), m_last_persist_time(0) {}

    void set_enabled(bool enabled) { m_enabled.store(enabled); }
    bool is_enabled() const { return m_enabled.load(); }
    void set_max_records(size_t n) { m_max_records = n; }
    void set_ban_threshold(int n) { m_ban_threshold = n; }

    // 每个会话结束时调用
    void record_session(const std::string& ip, bool authenticated) {
        if (!m_enabled.load() || ip.empty()) return;
        if (is_private_ip(ip)) return;

        uint64_t now = now_sec();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& rec = m_records[ip];
            rec.total_connections++;
            if (!authenticated) rec.failed_count++;
            if (rec.first_seen == 0) rec.first_seen = now;
            rec.last_seen = now;

            // LRU eviction
            while (m_records.size() > m_max_records) {
                auto oldest = m_records.begin();
                for (auto it = m_records.begin(); it != m_records.end(); ++it) {
                    if (it->second.last_seen < oldest->second.last_seen)
                        oldest = it;
                }
                m_records.erase(oldest);
            }

            // 懒刷盘：记录数超过脏阈值时才检查时间，避免频繁系统调用
            m_records_since_persist++;
            if (m_records_since_persist >= m_persist_dirty_threshold) {
                if (m_last_persist_time == 0 ||
                    now - m_last_persist_time >= static_cast<uint64_t>(m_persist_interval_sec)) {
                    persist_locked();
                    m_records_since_persist = 0;
                    m_last_persist_time = now;
                }
            }
        }
    }

    void set_persist_interval(int sec) { m_persist_interval_sec = sec; }
    void set_persist_dirty_threshold(int n) { m_persist_dirty_threshold = n; }

    // 判断 IP 是否应被拒绝服务
    bool is_banned(const std::string& ip) const {
        if (m_ban_threshold <= 0) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_records.find(ip);
        if (it == m_records.end()) return false;
        return it->second.failed_count >= m_ban_threshold;
    }

    // 查询某个 IP 的记录
    IpRecord query(const std::string& ip) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_records.find(ip);
        if (it != m_records.end()) return it->second;
        return IpRecord{};
    }

    // 获取完整快照
    std::unordered_map<std::string, IpRecord> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_records;
    }

    // 持久化到磁盘（显式调用，如 shutdown 时）
    bool persist() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return persist_locked();
    }

    // 从磁盘恢复
    bool restore() {
        if (m_data_file.empty()) return false;
        try {
            std::ifstream ifs(m_data_file);
            if (!ifs) return false;
            nlohmann::json j = nlohmann::json::parse(ifs);
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = j.begin(); it != j.end(); ++it) {
                m_records[it.key()] = IpRecord::from_json(it.value());
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_records.size();
    }

private:
    static bool is_private_ip(const std::string& ip) {
        if (ip == "127.0.0.1" || ip == "::1" || ip == "0.0.0.0") return true;
        if (ip.compare(0, 3, "10.") == 0) return true;
        if (ip.compare(0, 8, "192.168.") == 0) return true;
        if (ip.compare(0, 4, "172.") == 0) {
            auto pos = ip.find('.', 4);
            if (pos != std::string::npos) {
                int second = std::stoi(ip.substr(4, pos - 4));
                if (second >= 16 && second <= 31) return true;
            }
        }
        return false;
    }

    static uint64_t now_sec() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

private:
    // 必须在持锁状态下调用
    bool persist_locked() const {
        if (m_data_file.empty()) return false;
        nlohmann::json j;
        for (const auto& [ip, rec] : m_records)
            j[ip] = rec.to_json();
        try {
            std::ofstream ofs(m_data_file, std::ios::trunc);
            if (!ofs) return false;
            ofs << j.dump(2);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string m_data_file;
    std::atomic<bool> m_enabled;
    size_t m_max_records;
    int    m_ban_threshold;
    int    m_persist_interval_sec;
    int    m_persist_dirty_threshold;
    int    m_records_since_persist;
    uint64_t m_last_persist_time;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, IpRecord> m_records;
};

} // namespace mail_system
