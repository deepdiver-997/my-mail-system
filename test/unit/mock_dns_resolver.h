#ifndef MOCK_DNS_RESOLVER_H
#define MOCK_DNS_RESOLVER_H

#include "mail_system/back/outbound/dns_resolver.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace test {

// 可编程 Mock DNS 解析器 — 返回预置记录，不访问真实网络
class MockDnsResolver : public outbound::IDnsResolver {
public:
    void set_mx(const std::string& domain, const std::vector<outbound::MxRecord>& records) { mx_[domain] = records; }
    void set_txt(const std::string& domain, const std::vector<std::string>& records)   { txt_[domain] = records; }
    void set_host(const std::string& host, const std::vector<std::string>& addrs)       { addr_[host] = addrs; }
    void set_ptr(const std::string& ip, const std::vector<std::string>& ptrs)           { ptr_[ip] = ptrs; }

    void async_resolve_mx(const std::string& d, outbound::MxCallback cb) override {
        auto it = mx_.find(d); cb(it != mx_.end() ? it->second : std::vector<outbound::MxRecord>{});
    }
    void async_resolve_host(const std::string& h, outbound::AddrCallback cb) override {
        auto it = addr_.find(h); cb(it != addr_.end() ? it->second : std::vector<std::string>{});
    }
    void async_resolve_txt(const std::string& d, outbound::TxtCallback cb) override {
        auto it = txt_.find(d); cb(it != txt_.end() ? it->second : std::vector<std::string>{});
    }
    void async_resolve_ptr(const std::string& ip, outbound::PtrCallback cb) override {
        auto it = ptr_.find(ip); cb(it != ptr_.end() ? it->second : std::vector<std::string>{});
    }

private:
    std::unordered_map<std::string, std::vector<outbound::MxRecord>> mx_;
    std::unordered_map<std::string, std::vector<std::string>> txt_, addr_, ptr_;
};

} // namespace test
} // namespace mail_system
#endif
