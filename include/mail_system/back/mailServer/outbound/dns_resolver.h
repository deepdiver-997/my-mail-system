#ifndef MAIL_SYSTEM_DNS_RESOLVER_H
#define MAIL_SYSTEM_DNS_RESOLVER_H

#include "framework/net/dns_resolver.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>

namespace mail_system {
namespace outbound {

// 向后兼容
using pr::MxRecord;
using pr::MxCallback;
using pr::AddrCallback;
using pr::TxtCallback;
using pr::PtrCallback;
using pr::IDnsResolver;

// ---- .local 短路（仅测试用途） ----
// RFC 6762 把 .local 保留给 mDNS，但 e2e 测试时并不想依赖 mDNS responder。
// 当 env var PR_E2E_LOCAL_SHORTCUT=1 时，.local 后缀的 host 直接被替换为
// 127.0.0.1，跳过系统 DNS / mDNS。这样 A.OutboundSmtpSession.connect_to_mx
// 和 mx_routing_utils 走 c-ares 路径都能复用同一份逻辑。
// 生产环境默认不启用（env var 未设 → 走原 DNS 路径），且 .local 在公网上
// 不会出现在合法域名中，因此不构成安全或路由风险。
inline std::string local_shortcut_if_enabled(const std::string& host) {
    if (host.size() >= 6 &&
        host.compare(host.size() - 6, 6, ".local") == 0) {
        const char* env = std::getenv("PR_E2E_LOCAL_SHORTCUT");
        if (env && std::string(env) == "1") return "127.0.0.1";
    }
    return host;
}

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
