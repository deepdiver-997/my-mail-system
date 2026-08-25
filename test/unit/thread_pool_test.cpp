// 线程池单元测试：BoostThreadPool（submit/post 真实执行）与
// IOThreadPool（get_io_context 生产路径）。
//
// ⚠️ IOThreadPool::submit()/post() 是死 API：post_impl 是空实现（注释掉了），
//    提交的任务永远不会执行、future 永不完成 —— 若调用 submit 并阻塞 get 会挂死。
//    生产代码从不走这条路（accept/session 直接用 asio 在 get_io_context 上 post），
//    因此这里只测 get_io_context + asio::post 的真实用法，不调死 API。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <boost/asio/post.hpp>

#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"

namespace {

using mail_system::BoostThreadPool;
using mail_system::IOThreadPool;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

template <typename F>
bool wait_until(F f, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return f();
}

} // namespace

int main() {
    std::printf("thread_pool_test\n");

    // ── BoostThreadPool ──────────────────────────────────────────
    {
        BoostThreadPool pool(2);
        expect_true(!pool.is_running(), "boost pool not running before start");
        pool.start();
        expect_true(pool.is_running(), "boost pool running after start");
        expect_true(pool.thread_count() == 2, "boost pool thread count");

        // submit：future 拿到返回值
        auto fut = pool.submit([](int a, int b) { return a + b; }, 2, 3);
        expect_true(fut.get() == 5, "boost submit returns value");

        // post：异步执行 + 原子计数
        std::atomic<int> counter{0};
        for (int i = 0; i < 100; i++) pool.post([&counter] { counter++; });
        expect_true(wait_until([&] { return counter.load() == 100; }),
                    "boost post executes all tasks");

        // 重复 start 幂等
        pool.start();
        expect_true(pool.is_running(), "boost double start idempotent");

        // stop(wait=true)：排空任务后再返回
        std::atomic<bool> drained{false};
        pool.post([&drained] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            drained = true;
        });
        pool.stop(true);
        expect_true(drained.load(), "boost stop(true) drains pending tasks");
        expect_true(!pool.is_running(), "boost pool stopped");
    }

    // stop 后 submit 抛异常
    {
        BoostThreadPool pool(1);
        pool.start();
        pool.stop();
        bool threw = false;
        try {
            auto f = pool.submit([] { return 1; });
            (void)f;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        expect_true(threw, "boost submit after stop throws runtime_error");
    }

    // 未 start 直接 stop 安全
    {
        BoostThreadPool pool(2);
        pool.stop();
        expect_true(true, "boost stop before start is safe");
    }

    // ── IOThreadPool ────────────────────────────────────────────
    {
        IOThreadPool pool(2);
        expect_true(!pool.is_running(), "io pool not running before start");
        pool.start();
        expect_true(pool.is_running(), "io pool running after start");
        expect_true(pool.thread_count() == 2, "io pool thread count");

        // 生产路径：get_io_context + asio::post → 工作线程执行
        std::atomic<bool> ran{false};
        auto& ctx = pool.get_io_context();
        boost::asio::post(ctx, [&ran] { ran = true; });
        expect_true(wait_until([&] { return ran.load(); }),
                    "io pool executes asio::post on its context");

        // 重复 start 幂等
        pool.start();
        expect_true(pool.is_running(), "io pool double start idempotent");

        pool.stop();
        expect_true(!pool.is_running(), "io pool stopped");
    }

    // get_io_context 在 stop 后抛异常
    {
        IOThreadPool pool(1);
        pool.start();
        pool.stop();
        bool threw = false;
        try {
            (void)pool.get_io_context();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        expect_true(threw, "get_io_context after stop throws runtime_error");
    }

    // 未 start 直接 stop 安全
    {
        IOThreadPool pool(2);
        pool.stop();
        expect_true(true, "io pool stop before start is safe");
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
