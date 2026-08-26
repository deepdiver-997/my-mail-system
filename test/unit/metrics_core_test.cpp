// MetricsServer 核心 API 语义测试 — counter/gauge/histogram 三类的不变量
//
// 故事背景：2026-08-27 发现 ServerBase::increment_* 三个 helper 把 fetch_add
// 返回的累计值 v (1, 2, 3, ...) 当 inc_counter 的 delta 传入 → counter map
// 累加 1+2+...+N = N*(N+1)/2 三角形数，/metrics 渲染出错的数字。
//
// 本测试不为了覆盖率写小单测（HTTP 端点 / build_status JSON 留给 e2e），
// 只验三个核心 API 的不变量是「业务直觉应当成立」的：
//   - counter N 次 push delta=1 ⇒ counter 终值 = N（不是三角形数）
//   - gauge 后写覆盖前写（瞬时值语义）
//   - histogram sum/count 正确累加
//
// 这三个不变量是上层业务指标（mails_accepted / outbound_attempts / dns_query
// 等）正确性的基础。生产里任何一个被破，所有依赖它们的监控/告警都会失真。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

#include <boost/asio.hpp>

#include "framework/metrics_server.h"

namespace {

using mail_system::MetricsServer;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

// 从 Prometheus text 输出里找 "name value" 数字行（跳过 # HELP / # TYPE）
// 返回 -1 表示未找到。用于不依赖格式细节的稳健断言。
long find_value(const std::string& scrape, const std::string& name) {
    auto pos = scrape.find("\n" + name + " ");
    if (pos == std::string::npos) {
        // 可能在文件最末行（无 trailing newline）
        if (scrape.rfind(name + " ", 0) == 0) pos = 0;
        else return -1;
    } else {
        pos += 1;  // 跳过前导 \n
    }
    auto sp = scrape.find(' ', pos);
    if (sp == std::string::npos) return -1;
    auto nl = scrape.find('\n', sp);
    std::string num = scrape.substr(sp + 1,
        nl == std::string::npos ? std::string::npos : nl - sp - 1);
    try { return std::stol(num); } catch (...) { return -2; }
}

} // namespace

int main() {
    std::printf("metrics_core_test\n");

    // 1. counter 增量：N 次 inc delta=1 ⇒ 终值 = N（不是三角形数）
    //    这是 2026-08-27 修复的 bug 的不变量回归测试。
    {
        boost::asio::io_context io;
        MetricsServer m(io, 0, "127.0.0.1");
        // 不调 start()：本测试只验内部 map 渲染（scrape_text），不启 HTTP。
        for (int i = 0; i < 100; ++i) {
            m.inc_counter("mails_accepted_total", {}, 1);
        }
        long v = find_value(m.scrape_text(), "mails_accepted_total");
        expect_true(v == 100, "100 increments of delta=1 -> counter == 100 (NOT 5050 triangle)");
    }

    // 1b. counter 多 label：按 label 维度独立累加
    {
        boost::asio::io_context io;
        MetricsServer m(io, 0, "127.0.0.1");
        m.inc_counter("outbound_attempts_total", {{"domain", "a.local"}}, 1);
        m.inc_counter("outbound_attempts_total", {{"domain", "a.local"}}, 1);
        m.inc_counter("outbound_attempts_total", {{"domain", "b.local"}}, 1);
        std::string body = m.scrape_text();
        long va = find_value(body, "outbound_attempts_total{domain=\"a.local\"}");
        long vb = find_value(body, "outbound_attempts_total{domain=\"b.local\"}");
        expect_true(va == 2, "label-grouped counter: a.local == 2");
        expect_true(vb == 1, "label-grouped counter: b.local == 1");
    }

    // 2. gauge：后写覆盖前写（瞬时值语义）
    {
        boost::asio::io_context io;
        MetricsServer m(io, 0, "127.0.0.1");
        m.set_gauge("active_connections", {}, 1);
        m.set_gauge("active_connections", {}, 2);
        m.set_gauge("active_connections", {}, 5);
        long v = find_value(m.scrape_text(), "active_connections");
        expect_true(v == 5, "gauge last-write-wins: 1->2->5 -> 5");
    }

    // 3. histogram：sum 累加，count 累加 1（输出 _sum / _count 两行）
    {
        boost::asio::io_context io;
        MetricsServer m(io, 0, "127.0.0.1");
        m.observe("request_latency_ms", {}, 2.0);
        m.observe("request_latency_ms", {}, 3.0);
        m.observe("request_latency_ms", {}, 5.0);
        std::string body = m.scrape_text();
        long sum = find_value(body, "request_latency_ms_sum");
        long cnt = find_value(body, "request_latency_ms_count");
        expect_true(sum == 10, "histogram sum: 2+3+5 = 10");
        expect_true(cnt == 3,  "histogram count: 3 observations");
    }

    std::printf("metrics_core_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
