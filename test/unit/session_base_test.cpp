// SessionBase 生命周期单元测试（协议无关基座的核心）。
//
// 用 TestSession + MockConnection（同步模式内联投递）驱动：
//   - 异步读：完整行/流水线/残缺行/缓冲上限/读错误
//   - 异步写：直写/pipeline 累积/关闭后 no-op
//   - pause/drain：暂停消费与恢复
//   - trace：前缀记录/64KB 上限/干净关闭丢弃
//   - 错误码、认证计数、握手、close/release
// 无需 COMMON_SRCS（只链 framework 头 + mock）。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "test_session.h"

namespace {

using mail_system::MockConnection;
using mail_system::SessionBase;
using mail_system::SessionError;
using mail_system::TestSession;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

std::shared_ptr<TestSession> make_session() {
    return std::make_shared<TestSession>(std::make_unique<MockConnection>());
}

// 覆写缓冲上限的小会话（测无换行无界累积防线）
struct SmallBufSession : TestSession {
    using TestSession::TestSession;
    size_t max_command_buffer_bytes() const override { return 4; }
};

} // namespace

int main() {
    std::printf("session_base_test\n");

    // ── 构造 / 访问器 ─────────────────────────────────────────
    {
        auto s = make_session();
        expect_true(!s->is_closed(), "session open initially");
        expect_true(s->get_server() == nullptr, "server null when not provided");
        expect_true(&s->get_connection() != nullptr, "connection accessible");
        expect_true(s->get_client_ip() == "127.0.0.1", "client ip from mock connection");
        expect_true(!s->is_authenticated(), "not authenticated by default");
        s->set_authenticated(true);
        expect_true(s->is_authenticated(), "authenticated after set");
    }

    // ── do_async_read：单行完整 ───────────────────────────────
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        conn.set_read_data("HELO x\r\n");
        s->do_async_read();
        expect_true(s->read_lines.size() == 1 && s->read_lines[0] == "HELO x\r\n",
                    "single line read + processed");
        expect_true(s->process_count == 1, "process_read invoked");
        expect_true(s->buffered_size() == 0, "buffer drained after consume");
    }

    // ── do_async_read：两行流水线一次读完 ─────────────────────
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        conn.set_read_data("HELO x\r\nMAIL FROM:<a@b>\r\n");
        s->do_async_read();
        expect_true(s->read_lines.size() == 2, "pipelined lines consumed in one read");
        expect_true(s->read_lines[0] == "HELO x\r\n" &&
                    s->read_lines[1] == "MAIL FROM:<a@b>\r\n", "lines in order");
    }

    // ── do_async_read：残缺行留在缓冲，不触发 handle_read ─────
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        conn.set_read_data("HELO x");          // 无换行
        s->do_async_read();
        expect_true(s->read_lines.empty(), "incomplete line not processed");
        expect_true(s->buffered_size() == 6, "incomplete line stays buffered");
    }

    // ── do_async_read：读错误（EOF）→ handle_error → close ───
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        conn.set_read_data("");                 // 无数据 → eof
        s->do_async_read();
        expect_true(s->is_closed(), "read error closes session");
    }

    // ── do_async_read：close 后 no-op ────────────────────────
    {
        auto s = make_session();
        s->close();
        s->get_connection().set_read_data("HELO x\r\n");
        s->do_async_read();
        expect_true(s->read_lines.empty(), "no read after close");
    }

    // ── do_async_write：直写 + 回调 ──────────────────────────
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        int cb = 0;
        s->do_async_write("220 ok\r\n", [&cb](auto, auto) { cb++; });
        expect_true(conn.written() == "220 ok\r\n", "write flushed to connection");
        expect_true(cb == 1, "write callback invoked");
    }

    // ── do_async_write：close 后 no-op ───────────────────────
    {
        auto s = make_session();
        s->close();
        s->do_async_write("data", nullptr);
        expect_true(s->get_connection().written().empty(), "no write after close");
    }

    // ── pipeline：缓冲有行时累积响应，下一写一并 flush ───────
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        s->append_raw("NOOP\r\n");              // 缓冲中的命令
        int cb1 = 0;
        s->do_async_write("250 ok\r\n", [&cb1](auto, auto) { cb1++; });
        expect_true(cb1 == 1, "pipelined write callback synchronous");
        expect_true(conn.written().empty(), "pipelined write held in pending buffer");
        s->drain_buffered_commands();
        expect_true(s->read_lines.size() == 1, "buffered command consumed by drain");
        int cb2 = 0;
        s->do_async_write("221 bye\r\n", [&cb2](auto, auto) { cb2++; });
        expect_true(conn.written() == "250 ok\r\n221 bye\r\n",
                    "pending response flushed together with next write");
    }

    // ── pause / drain：暂停时缓冲命令等待，drain 恢复消费 ────
    {
        auto s = make_session();
        s->append_raw("AUTH LOGIN\r\n");
        s->set_paused(true);
        s->do_async_read();                     // 缓冲有行但 paused → 不消费
        expect_true(s->read_lines.empty(), "paused blocks buffered consumption");
        s->drain_buffered_commands();
        expect_true(s->read_lines.size() == 1 && s->read_lines[0] == "AUTH LOGIN\r\n",
                    "drain resumes after pause");
        expect_true(!s->is_paused(), "drain clears paused flag");
    }

    // ── 缓冲上限：无换行累积超限 → close ─────────────────────
    {
        auto s = std::make_shared<SmallBufSession>(std::make_unique<MockConnection>());
        s->get_connection().set_read_data("ABCDEFGH");   // 8 > 4 且无换行
        s->do_async_read();
        expect_true(s->is_closed(), "over command buffer limit closes session");
    }

    // ── close / release_connection ───────────────────────────
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        s->close();
        expect_true(s->is_closed(), "close sets closed");
        expect_true(!conn.is_open(), "close closes underlying connection");
        s->close();                              // 幂等
        expect_true(s->is_closed(), "double close safe");
    }
    {
        auto s = make_session();
        auto conn = s->release_connection();
        expect_true(conn != nullptr, "release_connection yields connection");
    }

    // ── close 时 flush 未写完的 pending 响应 ──────────────────
    {
        auto s = make_session();
        auto& conn = s->get_connection();
        s->append_raw("NOOP\r\n");
        s->do_async_write("250 ok\r\n", nullptr);   // 进 pending，不落连接
        s->close();                                 // close 刷 pending
        expect_true(conn.written() == "250 ok\r\n", "close flushes pending response");
    }

    // ── trace：前缀记录 + 64KB 上限 + 干净关闭丢弃 ────────────
    {
        auto s = make_session();
        s->trace_append_inbound("HELO x\r\n");
        s->trace_append_outbound("250 ok\r\n");
        auto buf = s->take_trace_buffer();
        expect_true(buf.find("C: HELO") != std::string::npos, "inbound trace prefixed C:");
        expect_true(buf.find("S: 250 ok") != std::string::npos, "outbound trace prefixed S:");
    }
    {
        auto s = make_session();
        s->trace_append_inbound(std::string(100000, 'x'));
        expect_true(s->take_trace_buffer().size() <= 64 * 1024, "trace capped at 64KB");
    }
    {
        auto s = make_session();
        s->trace_append_inbound("DATA\r\n");
        s->set_trace_clean_close();
        s->close();
        expect_true(s->take_trace_buffer().empty(), "clean close discards trace");
    }

    // ── 错误码 / 错误消息映射 ────────────────────────────────
    {
        auto s = make_session();
        s->set_error(SessionError::AuthFailed, "bad credentials");
        expect_true(s->get_error() == SessionError::AuthFailed, "get_error");
        expect_true(s->get_error_detail() == "bad credentials", "get_error_detail");
        expect_true(s->error_message(SessionError::AuthFailed) == "Authentication failed",
                    "error_message AuthFailed");
        expect_true(s->error_message(SessionError::InvalidCommand) == "Invalid command",
                    "error_message InvalidCommand");
        expect_true(s->error_message(SessionError::Timeout) == "Connection timeout",
                    "error_message Timeout");
        expect_true(s->error_message(SessionError::Internal) == "Internal server error",
                    "error_message Internal");
        expect_true(s->error_message(SessionError::None).empty(), "error_message None empty");
    }

    // ── 认证失败计数（无 server → 上限 3）────────────────────
    {
        auto s = make_session();
        expect_true(!s->record_auth_failure_and_check(), "1st failure not closing");
        expect_true(!s->record_auth_failure_and_check(), "2nd failure not closing");
        expect_true(s->record_auth_failure_and_check(), "3rd failure triggers close");
    }

    // ── do_handshake 成功回调 ────────────────────────────────
    {
        auto s = make_session();
        int hs_calls = 0;
        bool hs_ok = false;
        SessionBase<MockConnection>::do_handshake(
            s, boost::asio::ssl::stream_base::client,
            [&](std::shared_ptr<SessionBase<MockConnection>>,
                const boost::system::error_code& ec) { hs_calls++; hs_ok = !ec; });
        expect_true(hs_calls == 1 && hs_ok, "handshake completes without error on mock");
    }

    // ── handle_error → close ─────────────────────────────────
    {
        auto s = make_session();
        s->handle_error(boost::asio::error::eof);
        expect_true(s->is_closed(), "handle_error closes session");
    }

    // ── pop_buffered_line / take_buffered_input ──────────────
    {
        auto s = make_session();
        s->append_raw("A\r\nB\r\n");
        expect_true(s->pop_buffered_line() == "A\r\n", "pop first line");
        expect_true(s->pop_buffered_line() == "B\r\n", "pop second line");
        expect_true(s->pop_buffered_line().empty(), "pop on empty buffer returns empty");
    }
    {
        auto s = make_session();
        s->append_raw("tail");
        expect_true(s->take_buffered_input() == "tail", "take_buffered_input returns all");
        expect_true(s->buffered_size() == 0, "buffer cleared after take");
    }

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
