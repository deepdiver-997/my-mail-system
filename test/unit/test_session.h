#ifndef TEST_SESSION_H
#define TEST_SESSION_H

// ================================================================
// TestSession — 最小协议无关测试会话
//
// 实现 SessionBase<MockConnection> 的全部纯虚接口，供 framework 层
// 单元测试复用（fsm_base_test / fast_fsm_base_test / session_base_test）。
// 零 I/O：底层是 MockConnection（同步模式内联投递），无需真实套接字。
// ================================================================

#include "framework/session_base.h"
#include "framework/session_base.tpp"   // SessionBase<...>:: 模板成员函数定义
#include "mock_connection.h"

#include <memory>
#include <string>
#include <vector>

namespace mail_system {

class TestSession : public SessionBase<MockConnection> {
public:
    TestSession(std::unique_ptr<MockConnection> conn, ServerBase* server = nullptr)
        : SessionBase<MockConnection>(std::move(conn), server) {}

    // ── 记录 handle_read / process_read 调用（供断言）────
    std::vector<std::string> read_lines;
    int process_count = 0;

    // ── 纯虚实现 ────────────────────────────────────────────
    std::chrono::milliseconds compute_reply_delay() const override {
        return std::chrono::milliseconds(0);
    }
    // 用 \n 分行（SMTP 风格：兼容仅发 LF 的客户端）
    bool has_buffered_input() const override {
        return command_read_buffer_.find('\n') != std::string::npos;
    }
    std::string extract_one_line() override {
        auto pos = command_read_buffer_.find('\n');
        if (pos == std::string::npos) return {};
        std::string line = command_read_buffer_.substr(0, pos + 1);
        command_read_buffer_.erase(0, pos + 1);
        return line;
    }
    void handle_read(const std::string& line) override {
        read_lines.push_back(line);
    }
    void process_read() override {
        process_count++;
    }
    void set_current_state(int s) override { cur_state_ = s; }
    void set_next_event(int e) override { cur_event_ = e; }
    int  get_current_state() const override { return cur_state_; }
    int  get_next_event() const override { return cur_event_; }
    void* get_fsm() const override { return nullptr; }
    void* get_context() override { return nullptr; }
    std::string get_last_command_args() const override { return last_args_; }

    // ── 便捷访问 ────────────────────────────────────────────
    // 直接向命令缓冲追加原始数据（等价于一次 TCP 读投递，不走 async_read）
    void append_raw(const std::string& data) { command_read_buffer_ += data; }
    size_t buffered_size() const { return command_read_buffer_.size(); }

private:
    int cur_state_ = 0;
    int cur_event_ = 0;
    std::string last_args_;
};

} // namespace mail_system
#endif // TEST_SESSION_H
