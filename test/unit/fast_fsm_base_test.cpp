// FastFsmBase 分发语义测试（O(1) 数组版状态机基类）。
//
// 与 fsm_base_test 互补：这里验证数组版特有的 fallback 优先级——
// 专用 handler > fallback handler > on_invalid_transition，以及
// terminal 关闭、pre_dispatch 顺序。枚举必须带 COUNT 哨兵。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

// 注意：fast_fsm_base.h 不自包含（用了 SessionBase 却没 include session_base.h，
// 生产里靠 include 顺序侥幸编译）。此处先引入 test_session.h（含 session_base.h）。
#include "test_session.h"
#include "mock_connection.h"
#include "framework/fast_fsm_base.h"

namespace {

using mail_system::MockConnection;
using mail_system::SessionBase;
using mail_system::TestSession;

enum class St : int { INIT = 0, AUTH = 1, DONE = 2, COUNT = 3 };
enum class Ev : int { HELO = 0, AUTH = 1, QUIT = 2, COUNT = 3 };

using Fsm = mail_system::FastFsmBase<MockConnection, St, Ev>;
using Handler = Fsm::Handler;
using SessionPtr = std::shared_ptr<SessionBase<MockConnection>>;

struct TestFsm : Fsm {
    int invalid_calls = 0;
    int pre_calls = 0;
    int spec_calls = 0;
    int fallback_calls = 0;

    bool is_terminal_state(St s) const override { return s == St::DONE; }
    void on_invalid_transition(St, Ev, SessionPtr) override { invalid_calls++; }
    void pre_dispatch(St, Ev, SessionPtr) override { pre_calls++; }

    void run(SessionPtr s, St st, Ev ev) { dispatch(std::move(s), st, ev); }
};

SessionPtr make_session() {
    return std::make_shared<TestSession>(std::make_unique<MockConnection>());
}

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

} // namespace

int main() {
    std::printf("fast_fsm_base_test\n");

    // 1. 专用 handler 优先于 fallback
    {
        TestFsm fsm;
        fsm.add_handler(St::INIT, Ev::HELO, [&fsm](SessionPtr) { fsm.spec_calls++; });
        fsm.add_fallback_handler(St::INIT, [&fsm](SessionPtr) { fsm.fallback_calls++; });
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::HELO);
        expect_true(fsm.spec_calls == 1 && fsm.fallback_calls == 0, "specialized handler beats fallback");
        expect_true(fsm.pre_calls == 1, "pre_dispatch called once");
    }

    // 2. 无专用 handler → fallback 生效
    {
        TestFsm fsm;
        fsm.add_fallback_handler(St::INIT, [&fsm](SessionPtr) { fsm.fallback_calls++; });
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::AUTH);     // AUTH 无专用 handler
        expect_true(fsm.fallback_calls == 1, "fallback handler used when no specialized handler");
        expect_true(fsm.invalid_calls == 0, "fallback suppresses invalid_transition");
    }

    // 3. 两者皆无 → on_invalid_transition
    {
        TestFsm fsm;
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::QUIT);
        expect_true(fsm.invalid_calls == 1, "no handler at all -> on_invalid_transition");
    }

    // 4. terminal 状态 → 直接 close，不经任何 handler
    {
        TestFsm fsm;
        fsm.add_handler(St::DONE, Ev::HELO, [&fsm](SessionPtr) { fsm.spec_calls++; });
        auto s = make_session();
        fsm.run(s, St::DONE, Ev::HELO);
        expect_true(s->is_closed(), "terminal state closes session");
        expect_true(fsm.spec_calls == 0 && fsm.fallback_calls == 0, "terminal skips handlers");
    }

    // 5. fallback 后写入不影响已注册的专用 handler（表独立）
    {
        TestFsm fsm;
        fsm.add_fallback_handler(St::INIT, [&fsm](SessionPtr) { fsm.fallback_calls++; });
        fsm.add_handler(St::INIT, Ev::HELO, [&fsm](SessionPtr) { fsm.spec_calls++; });
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::HELO);
        expect_true(fsm.spec_calls == 1 && fsm.fallback_calls == 0, "registration order irrelevant");
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
