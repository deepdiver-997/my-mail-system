#include "mail_system/back/mailServer/outbound/cares_dns_resolver.h"
#include "mail_system/back/common/logger.h"
#include <ares.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <algorithm>
#include <cstring>
#include <netinet/in.h>

namespace mail_system {
namespace outbound {
namespace {

// ---- c-ares 回调上下文：持有用户 callback ----
template <typename Cb>
struct QueryContext {
    Cb callback;
};

// ---- 提取数据的 c-ares 回调 ----
void mx_callback(void* arg, int status, int, unsigned char* abuf, int alen) {
    auto* ctx = static_cast<QueryContext<MxCallback>*>(arg);
    if (!ctx || !ctx->callback) return;
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
    if (!ctx || !ctx->callback) return;
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
    if (!ctx || !ctx->callback) return;
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
    if (!ctx || !ctx->callback) return;
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
void CaresDnsResolver::async_resolve_mx(const std::string& domain, MxCallback cb) {
    if (domain.empty() || !cb) return;
    // .local 短路：env var PR_E2E_LOCAL_SHORTCUT=1 时直接返回 127.0.0.1 作为 MX
    if (auto short_host = local_shortcut_if_enabled(domain); short_host != domain) {
        cb({MxRecord{short_host, 10}});
        return;
    }
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<MxCallback>{std::move(cb)};
    ares_query(channel_, domain.c_str(), ns_c_in, ns_t_mx, &mx_callback, ctx);
}

void CaresDnsResolver::async_resolve_host(const std::string& host, AddrCallback cb) {
    if (host.empty() || !cb) return;
    // .local 短路：env var PR_E2E_LOCAL_SHORTCUT=1 时直接返回 127.0.0.1
    if (auto short_host = local_shortcut_if_enabled(host); short_host != host) {
        cb({short_host});
        return;
    }
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<AddrCallback>{std::move(cb)};
    ares_addrinfo_hints hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ares_getaddrinfo(channel_, host.c_str(), nullptr, &hints, &addr_callback, ctx);
}

void CaresDnsResolver::async_resolve_txt(const std::string& domain, TxtCallback cb) {
    if (domain.empty() || !cb) return;
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<TxtCallback>{std::move(cb)};
    ares_query(channel_, domain.c_str(), ns_c_in, ns_t_txt, &txt_callback, ctx);
}

void CaresDnsResolver::async_resolve_ptr(const std::string& ip, PtrCallback cb) {
    if (ip.empty() || !cb) return;
    char ptr_name[128];
    ipv4_to_ptr(ip, ptr_name, sizeof(ptr_name));
    if (!ptr_name[0]) { cb({}); return; }
    std::lock_guard lk(mutex_);
    if (!init_channel_locked()) { cb({}); return; }
    auto* ctx = new QueryContext<PtrCallback>{std::move(cb)};
    ares_query(channel_, ptr_name, ns_c_in, ns_t_ptr, &ptr_callback, ctx);
}

} // namespace outbound
} // namespace mail_system
