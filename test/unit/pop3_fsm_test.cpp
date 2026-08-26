// pop3_fsm_test — Commit 1 scaffold stub
// Commit 3 会用 TestServer + FsmTestFixture 模式（仿 test/unit/smtps_fsm_test.cpp）
// 填充真实测试，覆盖 USER/PASS/STAT/LIST/UIDL/RETR/DELE/NOOP/RSET/QUIT/CAPA。
// 本文件作为 placeholder，保证 ctest target 存在并 PASS。

#undef NDEBUG
#include <cassert>
#include <cstdio>

int main() {
    std::printf("pop3_fsm_test scaffold OK\n");
    assert(1 + 1 == 2);
    return 0;
}
