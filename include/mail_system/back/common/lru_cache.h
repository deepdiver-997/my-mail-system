#ifndef MAIL_SYSTEM_LRU_CACHE_H
#define MAIL_SYSTEM_LRU_CACHE_H

#include <chrono>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <functional>

namespace mail_system {

struct MailboxCacheEntry {
    uint64_t exists = 0;
    uint64_t unseen = 0;
    uint64_t uidnext = 0;
    uint64_t uidvalidity = 0;
};

inline std::string mbox_cache_key(uint64_t user_id, uint64_t mailbox_id) {
    return std::to_string(user_id) + ":" + std::to_string(mailbox_id);
}

struct MailSummary {
    uint64_t uid = 0;
    int flags = 0;
    uint64_t size = 0;
    std::string subject;
    std::string from;
    std::string date;
};

class IMailboxCache {
public:
    virtual ~IMailboxCache() = default;
    virtual void notify_change(uint64_t user_id, uint64_t mailbox_id) = 0;
};

// ====================================================================
// 通用线程安全 LRU 缓存
//
// 双锁设计:
//   m_map_mutex (shared_mutex) — 保护 hashmap 查找/插入/删除
//   m_list_mutex (mutex)       — 保护 LRU 链表顺序
//
// get() 命中时: shared_lock(map) + unique_lock(list) → splice 移前端
// put() 写入时: unique_lock(map) + unique_lock(list) → 一致性修改
//
// std::list::splice 不失效迭代器，不需要 weak_ptr。
// ====================================================================
template <typename Key, typename Value>
class LruCache {
public:
    using Clock = std::chrono::steady_clock;

    explicit LruCache(size_t capacity, std::chrono::seconds ttl = std::chrono::seconds(5))
        : m_capacity(capacity > 0 ? capacity : 1), m_ttl(ttl) {}

    bool get(const Key& key, Value& out_value, bool& out_stale) const {
        std::shared_lock map_lock(m_map_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end()) { out_stale = false; return false; }

        // 持 map 共享锁 + list 独占锁 → splice 安全
        std::unique_lock list_lock(m_list_mutex);
        m_list.splice(m_list.begin(), m_list, it->second);
        auto& entry = it->second->second;
        out_value = entry.value;
        out_stale = (Clock::now() - entry.created) >= m_ttl;
        return true;
    }

    void put(const Key& key, const Value& value) {
        std::unique_lock map_lock(m_map_mutex);
        std::unique_lock list_lock(m_list_mutex);

        auto it = m_map.find(key);
        if (it != m_map.end()) {
            it->second->second = {value, Clock::now()};
            m_list.splice(m_list.begin(), m_list, it->second);
            return;
        }

        if (m_map.size() >= m_capacity) {
            m_map.erase(m_list.back().first);
            m_list.pop_back();
        }

        m_list.emplace_front(key, InternalEntry{value, Clock::now()});
        m_map[key] = m_list.begin();
    }

    void invalidate(const Key& key) {
        std::unique_lock map_lock(m_map_mutex);
        std::unique_lock list_lock(m_list_mutex);
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            m_list.erase(it->second);
            m_map.erase(it);
        }
    }

    template <typename Pred>
    void invalidate_if(Pred pred) {
        std::unique_lock map_lock(m_map_mutex);
        std::unique_lock list_lock(m_list_mutex);
        for (auto it = m_list.begin(); it != m_list.end(); ) {
            if (pred(it->first)) {
                m_map.erase(it->first);
                it = m_list.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        std::unique_lock map_lock(m_map_mutex);
        std::unique_lock list_lock(m_list_mutex);
        m_list.clear();
        m_map.clear();
    }

    size_t size() const {
        std::shared_lock lock(m_map_mutex);
        return m_map.size();
    }

    Value get_or_refresh(const Key& key, std::function<Value()> loader, bool& out_stale) {
        Value v;
        if (get(key, v, out_stale)) return v;
        v = loader();
        put(key, v);
        out_stale = false;
        return v;
    }

private:
    struct InternalEntry { Value value; Clock::time_point created; };

    size_t m_capacity;
    std::chrono::seconds m_ttl;

    mutable std::list<std::pair<Key, InternalEntry>> m_list;
    mutable std::unordered_map<Key, decltype(m_list.begin())> m_map;

    mutable std::shared_mutex m_map_mutex;
    mutable std::mutex m_list_mutex;
};

} // namespace mail_system
#endif // MAIL_SYSTEM_LRU_CACHE_H
