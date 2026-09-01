#ifndef POP3_SESSION_TPP
#define POP3_SESSION_TPP

#include "mail_system/back/mailServer/session/pop3_session.h"

namespace mail_system {

template <typename ConnectionType>
Pop3Session<ConnectionType>::Pop3Session(
    ServerBase* server,
    std::unique_ptr<ConnectionType> connection,
    std::shared_ptr<TraditionalPop3Fsm<ConnectionType>> fsm)
    : SessionBase<ConnectionType>(std::move(connection), server)
    , fsm_(std::move(fsm))
    , next_event_(Pop3Event::CONNECT)
    , context_()
{
}

template <typename ConnectionType>
void Pop3Session<ConnectionType>::start(std::shared_ptr<Pop3Session> self) {
    if (!self) return;
    self->rearm(self->config_read_timeout());   // 挂接看门狗：闲置/对端断电 over read_timeout 回收
    // POP3 启动即发 +OK banner（INIT → AUTHORIZATION 自动转移）
    auto fsm = std::static_pointer_cast<TraditionalPop3Fsm<ConnectionType>>(self->fsm_);
    fsm->process_event(self, Pop3Event::CONNECT);
}

template <typename ConnectionType>
void Pop3Session<ConnectionType>::start_after_starttls(std::shared_ptr<Pop3Session> self) {
    // POP3 v1 不支持 STARTTLS（v2 再加）。TcpServerBase 会调这个钩子；
    // 这里走和 start() 一样的路径，发 +OK banner。
    start(self);
}

template <typename ConnectionType>
void Pop3Session<ConnectionType>::close() {
    // 幂等：只在首次关闭时推一次 sessions_total。
    // result=ok 表示干净 QUIT（handle_quit 已 set_trace_clean_close），
    // 否则视为异常结束（客户端断连 / 超时 / 错误）。
    if (!this->is_closed()) {
        // 取消锁心跳定时器：会话结束立即停续约（否则 pending handler 会
        // 继续在 worker 上 renew，且与 session 形成引用环延迟析构）。
        if (auto* ctx = static_cast<Pop3Context*>(this->get_context())) {
            if (ctx->heartbeat_timer) {
                ctx->heartbeat_timer->cancel();
                ctx->heartbeat_timer.reset();
            }
            ctx->heartbeat_handler.reset();
        }
        const std::string label = this->m_trace_clean_close ? "ok" : "err";
        if (auto* srv = this->get_server()) {
            srv->push_metric_counter("protorelay_pop3_sessions_total",
                                     {{"result", label}}, 1);
        }
    }
    SessionBase<ConnectionType>::close();
}

template <typename ConnectionType>
void Pop3Session<ConnectionType>::parse_pop3_command(const std::string& data) {
    if (!data.empty() && data.back() == '\n') {
        std::string line = data;
        if (line.size() >= 2 && line[line.size() - 2] == '\r')
            line.resize(line.size() - 2);
        else
            line.resize(line.size() - 1);

        // 去掉行尾空白
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();

        if (line.empty()) {
            next_event_ = Pop3Event::ERROR;
            last_command_args_.clear();
            return;
        }

        // 切 command + args
        std::string cmd, args;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            cmd = line;
        } else {
            cmd = line.substr(0, sp);
            args = line.substr(sp + 1);
            // 去掉 args 前的单个前导空格（已隐含：line 拆 +1 后就是 args 起点）
            while (!args.empty() && (args.front() == ' ' || args.front() == '\t'))
                args.erase(args.begin());
        }
        // 大写命令
        std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        last_command_args_ = std::move(args);

        if      (cmd == "CAPA")  next_event_ = Pop3Event::CAPA;
        else if (cmd == "USER")  next_event_ = Pop3Event::USER;
        else if (cmd == "PASS")  next_event_ = Pop3Event::PASS;
        else if (cmd == "STAT")  next_event_ = Pop3Event::STAT;
        else if (cmd == "LIST")  next_event_ = Pop3Event::LIST;
        else if (cmd == "UIDL")  next_event_ = Pop3Event::UIDL;
        else if (cmd == "RETR")  next_event_ = Pop3Event::RETR;
        else if (cmd == "DELE")  next_event_ = Pop3Event::DELE;
        else if (cmd == "NOOP")  next_event_ = Pop3Event::NOOP;
        else if (cmd == "RSET")  next_event_ = Pop3Event::RSET;
        else if (cmd == "QUIT")  next_event_ = Pop3Event::QUIT;
        else                     next_event_ = Pop3Event::ERROR;
    } else {
        // 没有换行：ignore，等下次 buffer 攒够
    }
}

template <typename ConnectionType>
void Pop3Session<ConnectionType>::handle_read(const std::string& data) {
    // watchdog：每次入站命令续命（线程安全，post 到 io）。POP3 无合法长 idle 态。
    this->rearm(this->config_read_timeout());
    parse_pop3_command(data);
}

template <typename ConnectionType>
bool Pop3Session<ConnectionType>::has_buffered_input() const {
    return this->command_read_buffer_.find('\n') != std::string::npos;
}

template <typename ConnectionType>
std::string Pop3Session<ConnectionType>::extract_one_line() {
    auto pos = this->command_read_buffer_.find('\n');
    if (pos == std::string::npos) return {};
    std::string line = this->command_read_buffer_.substr(0, pos + 1);
    this->command_read_buffer_.erase(0, pos + 1);
    return line;
}

template <typename ConnectionType>
std::chrono::milliseconds Pop3Session<ConnectionType>::compute_reply_delay() const {
    return std::chrono::milliseconds(0);
}

template <typename ConnectionType>
void Pop3Session<ConnectionType>::process_read() {
    auto fsm = static_cast<TraditionalPop3Fsm<ConnectionType>*>(this->get_fsm());
    fsm->auto_process_event(this->shared_from_this());
}

template <typename ConnectionType>
void* Pop3Session<ConnectionType>::get_fsm() const { return fsm_.get(); }

template <typename ConnectionType>
void* Pop3Session<ConnectionType>::get_context() { return &context_; }

template <typename ConnectionType>
void Pop3Session<ConnectionType>::set_current_state(int state) {
    state_.store(state, std::memory_order_release);
}

template <typename ConnectionType>
void Pop3Session<ConnectionType>::set_next_event(int event) {
    next_event_ = static_cast<Pop3Event>(event);
}

template <typename ConnectionType>
int Pop3Session<ConnectionType>::get_current_state() const {
    return state_.load(std::memory_order_acquire);
}

template <typename ConnectionType>
int Pop3Session<ConnectionType>::get_next_event() const {
    return static_cast<int>(next_event_);
}

template <typename ConnectionType>
std::string Pop3Session<ConnectionType>::get_last_command_args() const {
    return last_command_args_;
}

} // namespace mail_system

#endif // POP3_SESSION_TPP
