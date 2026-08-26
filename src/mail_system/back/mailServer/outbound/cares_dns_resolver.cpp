#include "mail_system/back/mailServer/outbound/cares_dns_resolver.h"
#include "mail_system/back/common/logger.h"
#include <ares.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <string>

namespace mail_system {
namespace outbound {
namespace {

// 2026-08-27: 把 c-ares status 映射成 metrics label 字符串。粗粒度：ok/nxdomain/
// timeout/servfail/other，够监控告警用，不必 1:1 映射 30+ ares 错误码。
const char* ares_status_label(int status) {
    switch (status) {
        case ARES_SUCCESS:    return "ok";
        case ARES_ENOTFOUND:  return "nxdomain";
        case ARES_ETIMEOUT:   return "timeout";
        case ARES_ESERVFAIL:  return "servfail";
        case ARES_ENODATA:    return "nodata";
        default:              return "other";
    }
}

// ---- c-ares 回调上下文：持有用户 callback + metrics 字段 ----
// 必须先于 push_dns_metrics 定义（C++ 模板/类型前向引用规则）。
struct QueryContextBase {
    std::chrono::steady_clock::time_point start_time;
    std::weak_ptr<MetricsServer> metrics;
};
template <typename Cb>
struct QueryContext : QueryContextBase {
    Cb callback;
};

// 2026-08-27: 推 DNS metrics 的 helper。4 个 callback 复用，避免重复 5 行 push 代码。
// 注意 c-ares 回调在 c-ares 内部线程跑（ARES_EVSYS_DEFAULT），不在 IO 线程；
// MetricsServer 用 shared_mutex 保护 map，跨线程 push 安全。
void push_dns_metrics(QueryContextBase* base, int status, const char* qtype) {
    auto m = base->metrics.lock();
    if (!m) return;
    auto dur = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - base->start_time).count();
    const char* status_str = ares_status_label(status);
    MetricsServer::LabelMap labels{{"qtype", qtype}, {"status", status_str}};
    m->observe("protorelay_dns_query_duration_seconds", labels, dur);
    m->inc_counter("protorelay_dns_query_total", labels, 1);
}

// ---- 提取数据的 c-ares 回调 ----
void mx_callback(void* arg, int status, int, unsigned char* abuf, int alen) {
    auto* ctx = static_cast<QueryContext<MxCallback>*>(arg);
    if (!ctx || !ctx->callback) { delete ctx; return; }
    push_dns_metrics(ctx, status, "MX");
    std::vector<MxRecord> records;
    if (status == ARES_SUCCESS && abuf && alen > 0) {
        struct ares_mx_reply* reply = nullptr;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (ares_parse_mx_reply(abuf, alen, &reply) == ARES_SUCCESS && reply) {
            for (auto* it = reply; it; it = it->next) {
                if (it->host && std::strlen(it->host) > 0)
                    records.push_back({it->host, static_cast<std::uint16_t>(it->priority)});
            }
            ares_free_data(reply);
        }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    }
    std::sort(records.begin(), records.end(),
              [](auto& a, auto& b) { return a.priority != b.priority ? a.priority < b.priority : a.host < b.host; });
    ctx->callback(std::move(records));
    delete ctx;
}

void addr_callback(void* arg, int status, int, struct ares_addrinfo* result) {
    auto* ctx = static_cast<QueryContext<AddrCallback>*>(arg);
    if (!ctx || !ctx->callback) { delete ctx; return; }
    push_dns_metrics(ctx, status, "A");
    std::vector<std::string> addrs;
    if (status == ARES_SUCCESS && result) {
        for (auto* node = result->nodes; node; node = node->ai_next) {
            if (!node->ai_addr) continue;
            char buf[INET6_ADDRSTRLEN] = {0};
            if (node->ai_family == AF_INET)
                inet_ntop(AF_INET, &((sockaddr_in*)node->ai_addr)->sin_addr, buf, sizeof(buf));
            else if (node->ai_family == AF_INET6)
                inet_ntop(AF_INET6, &((sockaddr_in6*)node->ai_addr)->sin6_addr, buf, sizeof(buf));
            else continue;
            if (buf[0]) addrs.emplace_back(buf);
        }
        ares_freeaddrinfo(result);
    }
    std::sort(addrs.begin(), addrs.end());
    addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
    ctx->callback(std::move(addrs));
    delete ctx;
}

void txt_callback(void* arg, int status, int, unsigned char* abuf, int alen) {
    auto* ctx = static_cast<QueryContext<TxtCallback>*>(arg);
    if (!ctx || !ctx->callback) { delete ctx; return; }
    push_dns_metrics(ctx, status, "TXT");
    std::vector<std::string> records;
    if (status == ARES_SUCCESS && abuf && alen > 0) {
        struct ares_txt_reply* reply = nullptr;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (ares_parse_txt_reply(abuf, alen, &reply) == ARES_SUCCESS && reply) {
            for (auto* it = reply; it; it = it->next)
                if (it->txt && it->length > 0)
                    records.emplace_back(reinterpret_cast<const char*>(it->txt), it->length);
            ares_free_data(reply);
        }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    }
    ctx->callback(std::move(records));
    delete ctx;
}

void ptr_callback(void* arg, int status, int, unsigned char* abuf, int alen) {
    auto* ctx = static_cast<QueryContext<PtrCallback>*>(arg);
    if (!ctx || !ctx->callback) { delete ctx; return; }
    push_dns_metrics(ctx, status, "PTR");
    std::vector<std::string> hostnames;
    if (status == ARES_SUCCESS && abuf && alen > 0) {
        struct hostent* host = nullptr;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (ares_parse_ptr_reply(abuf, alen, nullptr, 0, AF_UNSPEC, &host) == ARES_SUCCESS && host) {
            if (host->h_name && std::strlen(host->h_name) > 0) hostnames.emplace_back(host->h_name);
            for (char** a = host->h_aliases; a && *a; ++a)
                if (*a && std::strlen(*a) > 0) hostnames.emplace_back(*a);
            ares_free_hostent(host);
        }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    }
    ctx->callback(std::move(hostnames));
    delete ctx;
}

void ipv4_to_ptr(const std::string& ip, char* buf, size_t bufsz) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) { buf[0] = 0; return; }
    uint32_t n = ntohl(addr.s_addr);
    snprintf(buf, bufsz, "%u.%u.%u.%u.in-addr.arpa",
             n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, n >> 24);
}

} // namespace

// ================================================================
CaresDnsResolver::CaresDnsResolver() {
    std::lock_guard lk(mutex_);
    if (ares_library_init(ARES_LIB_INIT_ALL) == ARES_SUCCESS) {
        library_inited_ = true;
        init_channel_locked();
    }
}

CaresDnsResolver::~CaresDnsResolver() {
    std::lock_guard lk(mutex_);
    destroy_channel_locked();
    if (library_inited_) { ares_library_cleanup(); library_inited_ = false; }
}

bool CaresDnsResolver::init_channel_locked() {
    if (!library_inited_ || channel_) return channel_ != nullptr;
    ares_options opts{};
    opts.evsys = ARES_EVSYS_DEFAULT;
    int mask = ARES_OPT_EVENT_THREAD;
    if (ares_init_options(&channel_, &opts, mask) != ARES_SUCCESS || !channel_) {
        channel_ = nullptr;
        return false;
    }
    return true;
}

void CaresDnsResolver::destroy_channel_locked() {
    if (channel_) { ares_destroy(channel_); channel_ = nullptr; }
}

// ---- async 接口 ----
// 2026-08-27: 4 个 async_resolve_* 入口统一设 start_time + metrics 字段。
// 注意 .local 短路（local_shortcut_if_enabled）路径不走 ares 也不走 callback
// push，metrics 也不计 — 它是测试设施，不是真实 DNS 查询。
void CaresDnsResolver::async_resolve_mx(const std::string& domain, MxCallback cb) {
    if (domain.empty() || !cb) return;
    if (auto short_host = local_shortcut_if_enabled(domain); short_host != domain) {
        cb({MxRecord{short_host, 10}});
        return;
    }
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<MxCallback>{};
    ctx->callback = std::move(cb);
    ctx->start_time = std::chrono::steady_clock::now();
    ctx->metrics = m_metrics_;
    ares_query(channel_, domain.c_str(), ns_c_in, ns_t_mx, &mx_callback, ctx);
}

void CaresDnsResolver::async_resolve_host(const std::string& host, AddrCallback cb) {
    if (host.empty() || !cb) return;
    if (auto short_host = local_shortcut_if_enabled(host); short_host != host) {
        cb({short_host});
        return;
    }
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<AddrCallback>{};
    ctx->callback = std::move(cb);
    ctx->start_time = std::chrono::steady_clock::now();
    ctx->metrics = m_metrics_;
    ares_addrinfo_hints hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ares_getaddrinfo(channel_, host.c_str(), nullptr, &hints, &addr_callback, ctx);
}

void CaresDnsResolver::async_resolve_txt(const std::string& domain, TxtCallback cb) {
    if (domain.empty() || !cb) return;
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<TxtCallback>{};
    ctx->callback = std::move(cb);
    ctx->start_time = std::chrono::steady_clock::now();
    ctx->metrics = m_metrics_;
    ares_query(channel_, domain.c_str(), ns_c_in, ns_t_txt, &txt_callback, ctx);
}

void CaresDnsResolver::async_resolve_ptr(const std::string& ip, PtrCallback cb) {
    if (ip.empty() || !cb) return;
    char ptr_name[128];
    ipv4_to_ptr(ip, ptr_name, sizeof(ptr_name));
    if (!ptr_name[0]) { cb({}); return; }
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<PtrCallback>{};
    ctx->callback = std::move(cb);
    ctx->start_time = std::chrono::steady_clock::now();
    ctx->metrics = m_metrics_;
    ares_query(channel_, ptr_name, ns_c_in, ns_t_ptr, &ptr_callback, ctx);
}

} // namespace outbound
} // namespace mail_system
