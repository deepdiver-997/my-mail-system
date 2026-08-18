#ifndef MOCK_DNS_RESOLVER_H
#define MOCK_DNS_RESOLVER_H

#include "mail_system/back/outbound/dns_resolver.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace test {

// 可编程 Mock DNS 解析器 — 返回预置记录，不访问真实网络。
//
// 三种回调触发模式（默认 Sync，兼容 SyncDnsWrapper 同步路径）：
//   - Sync     : async_resolve_* 立即用当前记录调用回调（原行为）。
//                供 test_inbound_verifier 等走 SyncDnsWrapper 的同步测试。
//   - Manual   : async_resolve_* 保存回调（按 domain 入队），由测试线程
//                fire_txt()/fire_host()/... 手动触发（可跨线程）。
//                模拟 c-ares / DB worker 线程延迟回调，同时保持确定性。
//   - AutoDelay: 每次解析在独立线程 sleep(delay_ms) 后调用回调，制造真实
//                跨线程时序，供 TSan 检测潜在数据竞争。注意：若 delay 过小，
//                回调可能在 io 线程尚未退出命令链时触发（生产 DNS 延迟远大于
//                io 线程退出耗时，故测试应用足够的 delay）。
//                测试结束前应调用 join_all()（或依赖析构）等待线程完成。
class MockDnsResolver : public outbound::IDnsResolver {
public:
    enum class Mode { Sync, Manual, AutoDelay };

    // ---- 记录注入 ----
    void set_mx(const std::string& domain, const std::vector<outbound::MxRecord>& records) {
        std::lock_guard<std::mutex> lk(mu_);
        mx_[domain] = records;
    }
    void set_txt(const std::string& domain, const std::vector<std::string>& records) {
        std::lock_guard<std::mutex> lk(mu_);
        txt_[domain] = records;
    }
    void set_host(const std::string& host, const std::vector<std::string>& addrs) {
        std::lock_guard<std::mutex> lk(mu_);
        addr_[host] = addrs;
    }
    void set_ptr(const std::string& ip, const std::vector<std::string>& ptrs) {
        std::lock_guard<std::mutex> lk(mu_);
        ptr_[ip] = ptrs;
    }

    // ---- 触发模式 ----
    void set_mode(Mode m) {
        std::lock_guard<std::mutex> lk(mu_);
        mode_ = m;
    }
    void set_auto_delay(unsigned ms) { auto_delay_ms_ = ms; }

    // 等待所有 AutoDelay 线程结束（析构也会自动 join）。
    void join_all() {
        std::vector<std::thread> done;
        {
            std::lock_guard<std::mutex> lk(mu_);
            done.swap(threads_);
        }
        for (auto& t : done)
            if (t.joinable()) t.join();
    }

    // ---- Manual 模式：查询 pending / 手动触发 ----
    // pending 按 domain 存回调队列（同一 domain 可有多个并发查询）。
    bool has_pending_txt(const std::string& d) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_txt_.find(d);
        return it != pending_txt_.end() && !it->second.empty();
    }
    bool has_pending_host(const std::string& h) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_host_.find(h);
        return it != pending_host_.end() && !it->second.empty();
    }
    bool has_pending_mx(const std::string& d) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_mx_.find(d);
        return it != pending_mx_.end() && !it->second.empty();
    }
    bool has_pending_ptr(const std::string& ip) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_ptr_.find(ip);
        return it != pending_ptr_.end() && !it->second.empty();
    }

    void fire_txt(const std::string& d) { fire_txt(d, std::vector<std::string>{}); }
    void fire_txt(const std::string& d, std::vector<std::string> records) {
        outbound::TxtCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_txt_.find(d);
            if (it == pending_txt_.end() || it->second.empty()) return;
            cb = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) pending_txt_.erase(it);
            // records 未显式提供时使用当前注入的记录（可能为空 → 负缓存）
            if (records.empty()) {
                auto r = txt_.find(d);
                if (r != txt_.end()) records = r->second;
            }
        }
        cb(std::move(records));
    }

    void fire_host(const std::string& h) { fire_host(h, std::vector<std::string>{}); }
    void fire_host(const std::string& h, std::vector<std::string> addrs) {
        outbound::AddrCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_host_.find(h);
            if (it == pending_host_.end() || it->second.empty()) return;
            cb = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) pending_host_.erase(it);
            if (addrs.empty()) {
                auto r = addr_.find(h);
                if (r != addr_.end()) addrs = r->second;
            }
        }
        cb(std::move(addrs));
    }

    void fire_mx(const std::string& d) { fire_mx(d, std::vector<outbound::MxRecord>{}); }
    void fire_mx(const std::string& d, std::vector<outbound::MxRecord> records) {
        outbound::MxCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_mx_.find(d);
            if (it == pending_mx_.end() || it->second.empty()) return;
            cb = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) pending_mx_.erase(it);
            if (records.empty()) {
                auto r = mx_.find(d);
                if (r != mx_.end()) records = r->second;
            }
        }
        cb(std::move(records));
    }

    void fire_ptr(const std::string& ip) { fire_ptr(ip, std::vector<std::string>{}); }
    void fire_ptr(const std::string& ip, std::vector<std::string> ptrs) {
        outbound::PtrCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_ptr_.find(ip);
            if (it == pending_ptr_.end() || it->second.empty()) return;
            cb = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) pending_ptr_.erase(it);
            if (ptrs.empty()) {
                auto r = ptr_.find(ip);
                if (r != ptr_.end()) ptrs = r->second;
            }
        }
        cb(std::move(ptrs));
    }

    // ---- IDnsResolver 异步接口 ----
    // 三种模式统一在锁外调用回调（Sync 同步；Manual 入队；AutoDelay 后台线程）。
    void async_resolve_mx(const std::string& d, outbound::MxCallback cb) override {
        std::vector<outbound::MxRecord> records;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (mode_ == Mode::Sync) {
                records = mx_[d];
            } else if (mode_ == Mode::AutoDelay) {
                unsigned delay = auto_delay_ms_;
                auto rec = mx_[d];
                threads_.emplace_back([delay, rec, cb = std::move(cb)]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    cb(std::move(rec));
                });
                return;
            } else {
                pending_mx_[d].push_back(std::move(cb));
                return;
            }
        }
        cb(std::move(records));
    }
    void async_resolve_host(const std::string& h, outbound::AddrCallback cb) override {
        std::vector<std::string> addrs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (mode_ == Mode::Sync) {
                addrs = addr_[h];
            } else if (mode_ == Mode::AutoDelay) {
                unsigned delay = auto_delay_ms_;
                auto a = addr_[h];
                threads_.emplace_back([delay, a, cb = std::move(cb)]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    cb(std::move(a));
                });
                return;
            } else {
                pending_host_[h].push_back(std::move(cb));
                return;
            }
        }
        cb(std::move(addrs));
    }
    void async_resolve_txt(const std::string& d, outbound::TxtCallback cb) override {
        std::vector<std::string> records;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (mode_ == Mode::Sync) {
                records = txt_[d];
            } else if (mode_ == Mode::AutoDelay) {
                unsigned delay = auto_delay_ms_;
                auto rec = txt_[d];
                threads_.emplace_back([delay, rec, cb = std::move(cb)]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    cb(std::move(rec));
                });
                return;
            } else {
                pending_txt_[d].push_back(std::move(cb));
                return;
            }
        }
        cb(std::move(records));
    }
    void async_resolve_ptr(const std::string& ip, outbound::PtrCallback cb) override {
        std::vector<std::string> ptrs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (mode_ == Mode::Sync) {
                ptrs = ptr_[ip];
            } else if (mode_ == Mode::AutoDelay) {
                unsigned delay = auto_delay_ms_;
                auto p = ptr_[ip];
                threads_.emplace_back([delay, p, cb = std::move(cb)]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    cb(std::move(p));
                });
                return;
            } else {
                pending_ptr_[ip].push_back(std::move(cb));
                return;
            }
        }
        cb(std::move(ptrs));
    }

    ~MockDnsResolver() override { join_all(); }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<outbound::MxRecord>> mx_;
    std::unordered_map<std::string, std::vector<std::string>> txt_, addr_, ptr_;
    std::unordered_map<std::string, std::deque<outbound::MxCallback>> pending_mx_;
    std::unordered_map<std::string, std::deque<outbound::AddrCallback>> pending_host_;
    std::unordered_map<std::string, std::deque<outbound::TxtCallback>> pending_txt_;
    std::unordered_map<std::string, std::deque<outbound::PtrCallback>> pending_ptr_;
    Mode mode_ = Mode::Sync;
    unsigned auto_delay_ms_ = 2;
    std::vector<std::thread> threads_;
};

} // namespace test
} // namespace mail_system
#endif
