#ifndef MOCK_DNS_RESOLVER_H
#define MOCK_DNS_RESOLVER_H

#include "mail_system/back/outbound/dns_resolver.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace test {

// 可编程 Mock DNS 解析器 — 返回预置的记录，不访问真实网络
class MockDnsResolver : public outbound::IDnsResolver {
public:
    void set_mx(const std::string& domain, const std::vector<outbound::MxRecord>& records) {
        mx_map_[domain] = records;
    }
    void set_txt(const std::string& domain, const std::vector<std::string>& records) {
        txt_map_[domain] = records;
    }
    void set_host_addresses(const std::string& host, const std::vector<std::string>& addrs) {
        addr_map_[host] = addrs;
    }
    void set_ptr(const std::string& ip, const std::vector<std::string>& ptrs) {
        ptr_map_[ip] = ptrs;
    }

    std::vector<outbound::MxRecord> resolve_mx(const std::string& domain) override {
        auto it = mx_map_.find(domain);
        return it != mx_map_.end() ? it->second : std::vector<outbound::MxRecord>{};
    }

    std::vector<std::string> resolve_host_addresses(const std::string& host) override {
        auto it = addr_map_.find(host);
        return it != addr_map_.end() ? it->second : std::vector<std::string>{};
    }

    std::vector<std::string> resolve_txt(const std::string& domain) override {
        auto it = txt_map_.find(domain);
        return it != txt_map_.end() ? it->second : std::vector<std::string>{};
    }

    std::vector<std::string> resolve_ptr(const std::string& ip) override {
        auto it = ptr_map_.find(ip);
        return it != ptr_map_.end() ? it->second : std::vector<std::string>{};
    }

private:
    std::unordered_map<std::string, std::vector<outbound::MxRecord>> mx_map_;
    std::unordered_map<std::string, std::vector<std::string>> txt_map_;
    std::unordered_map<std::string, std::vector<std::string>> addr_map_;
    std::unordered_map<std::string, std::vector<std::string>> ptr_map_;
};

} // namespace test
} // namespace mail_system

#endif // MOCK_DNS_RESOLVER_H
