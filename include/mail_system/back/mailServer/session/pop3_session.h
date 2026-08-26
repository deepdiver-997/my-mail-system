#ifndef POP3_SESSION_H
#define POP3_SESSION_H

#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "framework/session_base.h"
#include "mail_system/back/mailServer/fsm/pop3/pop3_types.hpp"
#include "mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm.h"
#include <cctype>
#include <memory>
#include <string>

namespace mail_system {

template <typename ConnectionType>
class Pop3Session : public SessionBase<ConnectionType> {
public:
    Pop3Session(
        ServerBase* server,
        std::unique_ptr<ConnectionType> connection,
        std::shared_ptr<TraditionalPop3Fsm<ConnectionType>> fsm);

    ~Pop3Session() override = default;

    // 关闭时推送 protorelay_pop3_sessions_total{result=ok/err}，
    // 再走 SessionBase::close()（其内会推通用 session_duration / record_session_end）。
    void close() override;

    static void start(std::shared_ptr<Pop3Session> self);
    static void start_after_starttls(std::shared_ptr<Pop3Session> self);

    void handle_read(const std::string& data) override;
    void process_read() override;
    bool has_buffered_input() const override;
    std::string extract_one_line() override;
    std::chrono::milliseconds compute_reply_delay() const override;
    void* get_fsm() const override;
    void* get_context() override;

    void set_current_state(int state) override;
    void set_next_event(int event) override;
    int get_current_state() const override;
    int get_next_event() const override;
    std::string get_last_command_args() const override;

    // POP3 命令解析：取一行（已含 \r\n），切出 command + args，
    // 把大写 command 映射到 Pop3Event，写入 last_command_args_。
    void parse_pop3_command(const std::string& data);

private:
    std::shared_ptr<TraditionalPop3Fsm<ConnectionType>> fsm_;
    std::atomic<int> state_{static_cast<int>(Pop3State::INIT)};
    Pop3Event next_event_{Pop3Event::CONNECT};
    std::string last_command_args_;
    Pop3Context context_;
};

using TcpPop3Session = Pop3Session<TcpConnection>;
using SslPop3Session = Pop3Session<SslConnection>;

} // namespace mail_system

#endif // POP3_SESSION_H
