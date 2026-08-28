// dispatch_bench — 对比 FsmBase (std::map) vs 编译期数组 O(1) dispatch
// 不依赖项目任何头文件，纯粹的 map vs array 性能测试
// 构建: cd build && make dispatch_bench
// 用法: ./dispatch_bench [iterations]
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>

// ── 模拟 12 状态 × 11 事件的 SMTP 规模 ──
enum S { s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11, S_COUNT };
enum E { e0,e1,e2,e3,e4,e5,e6,e7,e8,e9,e10, E_COUNT };

using H = std::function<void()>;
static auto noop = []{};

// ── Map-based dispatch (当前的 FsmBase 实现) ──
struct MapFsm {
    std::map<std::pair<S, E>, S> transitions;
    std::map<S, std::map<E, H>>  handlers;

    void add(S s, E e, H h) { handlers[s][e] = std::move(h); }
    void add_transition(S f, E o, S t) { transitions[{f, o}] = t; }

    __attribute__((noinline))
    void dispatch(S s, E e) {
        auto ti = transitions.find({s, e});
        if (ti == transitions.end()) return;
        auto si = handlers.find(s);
        if (si == handlers.end()) return;
        auto ei = si->second.find(e);
        if (ei == si->second.end()) return;
        ei->second();
    }
};

// ── Array-based dispatch (FastFsmBase 实现) ──
struct ArrayFsm {
    H  hs[S_COUNT][E_COUNT]{};
    S  ts[S_COUNT][E_COUNT]{};

    void add(S s, E e, H h) { hs[s][e] = std::move(h); }
    void add_transition(S f, E o, S t) { ts[f][o] = t; }

    __attribute__((noinline))
    void dispatch(S s, E e) {
        auto& h = hs[s][e];
        if (h) h();
    }
};

// ── 填充 6 步 SMTP 投递路径 ──
template <typename T>
void fill(T& m) {
    m.add_transition(s0,e0,s1);  m.add(s0,e0,noop);  // INIT → CONNECT → GREETING
    m.add_transition(s1,e1,s3);  m.add(s1,e1,noop);  // GREETING → EHLO → WAIT_AUTH
    m.add_transition(s3,e3,s6);  m.add(s3,e3,noop);  // WAIT_AUTH → MAIL_FROM → WAIT_MAIL
    m.add_transition(s6,e4,s7);  m.add(s6,e4,noop);  // WAIT_MAIL → RCPT → WAIT_RCPT
    m.add_transition(s7,e5,s8);  m.add(s7,e5,noop);  // WAIT_RCPT → DATA → WAIT_DATA
    m.add_transition(s8,e6,s10); m.add(s8,e6,noop);  // WAIT_DATA → DATA_END → WAIT_QUIT
}

template <typename F>
void bench(const char* label, F&& fn, size_t total) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    auto t1 = std::chrono::steady_clock::now();
    double s = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "  " << label << ": " << static_cast<int>(total / s / 1e6)
              << "M op/s  (" << static_cast<int>(s / total * 1e9) << " ns/op)\n";
}

int main(int argc, char* argv[]) {
    size_t n = 20000000;
    if (argc > 1) n = static_cast<size_t>(std::stoull(argv[1]));

    // SMTP 完整投递路径 (6 步 dispatch/封)
    constexpr S states[] = {s0, s1, s3, s6, s7, s8};
    constexpr E events[] = {e0, e1, e3, e4, e5, e6};

    MapFsm   mf; fill(mf);
    ArrayFsm af; fill(af);

    std::cout << "=== FSM Dispatch: std::map vs array (n=" << n/1000000 << "M) ===\n\n";

    // 预热
    for (int i = 0; i < 1000; ++i)
        for (int j = 0; j < 6; ++j) { mf.dispatch(states[j], events[j]); af.dispatch(states[j], events[j]); }

    size_t total = n * 6; // 6 dispatch per loop

    // std::map
    bench("  std::map (RB-tree, O(log N))", [&]{
        for (size_t i = 0; i < n; ++i)
            for (int j = 0; j < 6; ++j) mf.dispatch(states[j], events[j]);
    }, total);

    // array
    bench("  array   (compile-time, O(1))", [&]{
        for (size_t i = 0; i < n; ++i)
            for (int j = 0; j < 6; ++j) af.dispatch(states[j], events[j]);
    }, total);

    std::cout << "\nSMTP 每封邮件 6 次 dispatch × 15000 msg/s = "
              << (15000 * 6) << " dispatch/s\n";

    return 0;
}
