#ifndef MAIL_SYSTEM_DNS_RESOLVER_H
#define MAIL_SYSTEM_DNS_RESOLVER_H

#include "framework/net/dns_resolver.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace mail_system {
namespace outbound {

// 向后兼容
using pr::MxRecord;
using pr::MxCallback;
using pr::AddrCallback;
using pr::TxtCallback;
using pr::PtrCallback;
using pr::IDnsResolver;

// ---- SyncDnsWrapper: 将异步 IDnsResolver 包装为同步阻塞接口 ----
// 供不需要异步模式的旧代码使用。
class SyncDnsWrapper {
public:
    explicit SyncDnsWrapper(IDnsResolver& async_resolver) : resolver_(async_resolver) {}

    std::vector<MxRecord> resolve_mx(const std::string& domain) {
        return sync_call<MxRecord>([&](auto cb) { resolver_.async_resolve_mx(domain, cb); });
    }
    std::vector<std::string> resolve_host_addresses(const std::string& host) {
        return sync_call<std::string>([&](auto cb) { resolver_.async_resolve_host(host, cb); });
    }
    std::vector<std::string> resolve_txt(const std::string& domain) {
        return sync_call<std::string>([&](auto cb) { resolver_.async_resolve_txt(domain, cb); });
    }
    std::vector<std::string> resolve_ptr(const std::string& ip) {
        return sync_call<std::string>([&](auto cb) { resolver_.async_resolve_ptr(ip, cb); });
    }

private:
    template <typename T>
    std::vector<T> sync_call(std::function<void(std::function<void(std::vector<T>)>)> fn) {
        std::vector<T> result;
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        fn([&](std::vector<T> r) {
            { std::lock_guard lk(mtx); result = std::move(r); done = true; }
            cv.notify_one();
        });
        std::unique_lock lk(mtx);
        cv.wait(lk, [&]{ return done; });
        return result;
    }
    IDnsResolver& resolver_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_DNS_RESOLVER_H
