// FsmBase 分发语义测试。
//
// FsmBase 是 std::map 版本的协议无关状态机基类（fast_fsm_base 是数组版）。
// SMTP/IMAP 的真实 FSM 都继承它，但 dispatch 的分支（terminal 关闭、
// 非法转换、缺 handler、pre_dispatch 顺序）此前只被协议级测试间接覆盖。
// 这里用最小枚举 + TestSession 直接打透 dispatch 的每一条路径。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include "framework/fsm_base.h"
#include "mock_connection.h"
#include "test_session.h"

namespace {

using mail_system::MockConnection;
using mail_system::SessionBase;
using mail_system::TestSession;

enum class St : int { INIT = 0, AUTH = 1, DATA = 2, DONE = 3, COUNT = 4 };
enum class Ev : int { HELO = 0, AUTH = 1, QUIT = 2, COUNT = 3 };

using Fsm = mail_system::FsmBase<MockConnection, St, Ev>;
using Handler = Fsm::Handler;
using SessionPtr = std::shared_ptr<SessionBase<MockConnection>>;

struct TestFsm : Fsm {
    int invalid_calls = 0;
    int notfound_calls = 0;
    int pre_calls = 0;
    int handler_calls = 0;
    int seq = 0;            // 调用顺序：pre_dispatch 先于 handler
    int pre_seq = -1, handler_seq = -1;

    bool is_terminal_state(St s) const override { return s == St::DONE; }
    void on_invalid_transition(St, Ev, SessionPtr) override { invalid_calls++; }
    void on_handler_not_found(St, Ev, SessionPtr) override { notfound_calls++; }
    void pre_dispatch(St, Ev, SessionPtr) override { pre_seq = ++seq; pre_calls++; }

    // dispatch 是 protected，包一层公开入口
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
    std::printf("fsm_base_test\n");

    // 1. 合法转换：pre_dispatch → handler 依次执行，会话不关闭
    {
        TestFsm fsm;
        fsm.add_transition(St::INIT, Ev::HELO, St::AUTH);
        fsm.add_handler(St::INIT, Ev::HELO, [&fsm](SessionPtr) { fsm.handler_seq = ++fsm.seq; fsm.handler_calls++; });
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::HELO);
        expect_true(fsm.handler_calls == 1 && fsm.pre_calls == 1, "valid transition runs handler once");
        expect_true(fsm.pre_seq == 1 && fsm.handler_seq == 2, "pre_dispatch runs before handler");
        expect_true(!s->is_closed(), "non-terminal dispatch keeps session open");
    }

    // 2. terminal 状态：无论事件，直接 close()，不查表不调 handler
    {
        TestFsm fsm;
        fsm.add_transition(St::INIT, Ev::QUIT, St::DONE);
        fsm.add_handler(St::DONE, Ev::HELO, [&fsm](SessionPtr) { fsm.handler_calls++; });
        auto s = make_session();
        fsm.run(s, St::DONE, Ev::HELO);
        expect_true(s->is_closed(), "terminal state closes session");
        expect_true(fsm.handler_calls == 0 && fsm.pre_calls == 0, "terminal closes before any dispatch");
    }

    // 3. 未注册转换 → on_invalid_transition
    {
        TestFsm fsm;
        fsm.add_transition(St::INIT, Ev::HELO, St::AUTH);   // 只注册 (INIT,HELO)
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::QUIT);                     // (INIT,QUIT) 未注册
        expect_true(fsm.invalid_calls == 1, "missing transition -> on_invalid_transition");
    }

    // 4. 转换存在但缺 handler → on_handler_not_found
    {
        TestFsm fsm;
        fsm.add_transition(St::INIT, Ev::HELO, St::AUTH);   // 无 add_handler
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::HELO);
        expect_true(fsm.notfound_calls == 1, "missing handler -> on_handler_not_found");
    }

    // 5. 状态有 handler 但该事件没注册 → on_handler_not_found
    {
        TestFsm fsm;
        fsm.add_transition(St::INIT, Ev::HELO, St::AUTH);
        fsm.add_transition(St::INIT, Ev::AUTH, St::DATA);
        fsm.add_handler(St::INIT, Ev::HELO, [](SessionPtr) {});  // 只注册 HELO
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::AUTH);
        expect_true(fsm.notfound_calls == 1, "state present but event unhandled -> on_handler_not_found");
    }

    // 6. transition 表可被 add_transition 覆盖（后写生效）
    {
        TestFsm fsm;
        fsm.add_transition(St::INIT, Ev::HELO, St::AUTH);
        fsm.add_transition(St::INIT, Ev::HELO, St::DATA);   // 覆盖
        fsm.add_handler(St::INIT, Ev::HELO, [&fsm](SessionPtr) { fsm.handler_calls++; });
        auto s = make_session();
        fsm.run(s, St::INIT, Ev::HELO);
        expect_true(fsm.handler_calls == 1, "overwritten transition still dispatches");
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
