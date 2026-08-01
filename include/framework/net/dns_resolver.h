#ifndef PR_FRAMEWORK_NET_DNS_RESOLVER_H
#define PR_FRAMEWORK_NET_DNS_RESOLVER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pr {

struct MxRecord {
    std::string host;
    std::uint16_t priority{0};
};

// ---- 异步回调类型 ----
using MxCallback  = std::function<void(std::vector<MxRecord>)>;
using AddrCallback = std::function<void(std::vector<std::string>)>;
using TxtCallback  = std::function<void(std::vector<std::string>)>;
using PtrCallback  = std::function<void(std::vector<std::string>)>;

// ---- IDnsResolver: 异步 DNS 解析接口 ----
//
// 回调在解析完成时直接调用（通常在 c-ares 内部线程上）。
// 调用者负责在回调中通过业务自身的异步机制恢复处理，
// 例如 SMTP session 的 drain_buffered_commands() 模式：
//
//   dns->async_resolve_txt(domain, [self](auto records) {
//       self->dns_result_ = records;
//       self->drain_buffered_commands();  // 恢复 FSM
//   });
//
class IDnsResolver {
public:
    virtual ~IDnsResolver() = default;

    virtual void async_resolve_mx(const std::string& domain, MxCallback cb) = 0;
    virtual void async_resolve_host(const std::string& host, AddrCallback cb) = 0;
    virtual void async_resolve_txt(const std::string& domain, TxtCallback cb) = 0;
    virtual void async_resolve_ptr(const std::string& ip, PtrCallback cb) = 0;
};

} // namespace pr

#endif // PR_FRAMEWORK_NET_DNS_RESOLVER_H
