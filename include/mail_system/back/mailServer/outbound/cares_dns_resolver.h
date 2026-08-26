#ifndef MAIL_SYSTEM_CARES_DNS_RESOLVER_H
#define MAIL_SYSTEM_CARES_DNS_RESOLVER_H

#include "framework/metrics_server.h"
#include "mail_system/back/mailServer/outbound/dns_resolver.h"
#include <ares.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mail_system {
namespace outbound {

// ---- CaresDnsResolver: 基于 c-ares 的异步 DNS 解析器 ----
//
// 实现 pr::IDnsResolver 异步接口。c-ares 回调在其内部线程执行，
// 直接调用用户回调。用户回调中可通过 session->drain_buffered_commands()
// 恢复 FSM 处理，后续 do_async_read/write 自然回到 IO 线程。
class CaresDnsResolver : public IDnsResolver {
public:
    CaresDnsResolver();
    ~CaresDnsResolver() override;

    // ---- IDnsResolver async 接口 ----
    void async_resolve_mx(const std::string& domain, MxCallback cb) override;
    void async_resolve_host(const std::string& host, AddrCallback cb) override;
    void async_resolve_txt(const std::string& domain, TxtCallback cb) override;
    void async_resolve_ptr(const std::string& ip, PtrCallback cb) override;

    // 2026-08-27: 注入 metrics。weak_ptr 避免循环引用，析构后 push 自动失效。
    // 注入时机：OutboundServer::set_config 之后（与 set_metrics 同步注入）。
    void set_metrics(std::weak_ptr<MetricsServer> m) { m_metrics_ = std::move(m); }

private:
    bool init_channel_locked();
    void destroy_channel_locked();

    ares_channel channel_{nullptr};
    bool library_inited_{false};
    std::mutex mutex_;
    std::weak_ptr<MetricsServer> m_metrics_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_CARES_DNS_RESOLVER_H
