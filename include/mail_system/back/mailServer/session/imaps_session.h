#ifndef IMAPS_SESSION_H
#define IMAPS_SESSION_H

#include "mail_system/back/common/logger.h"
#include "framework/connection/tcp_connection.h"
#include "framework/connection/ssl_connection.h"
#include "framework/session_base.h"
#include "mail_system/back/mailServer/fsm/imaps/imaps_fsm.hpp"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace mail_system {

template <typename ConnectionType>
class ImapsSession : public SessionBase<ConnectionType> {
public:
    ImapsSession(
        ServerBase* server,
        std::unique_ptr<ConnectionType> connection,
        std::shared_ptr<TraditionalImapsFsm<ConnectionType>> fsm);

    ~ImapsSession() {
        this->session_authenticated_ = context_.is_authenticated;
    }

    static void start(std::shared_ptr<ImapsSession> self);
    static void start_after_starttls(std::shared_ptr<ImapsSession> self);

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

    // IMAP 命令解析
    void parse_imap_command(const std::string& data);

private:
    // 处理 IMAP 文字量 (literal) 模式
    bool try_parse_literal(const std::string& data, size_t& consumed);
    void append_literal_data(const std::string& data, size_t& consumed);

    // 检查是否正在等待 literal 数据
    bool is_waiting_for_literal() const { return awaiting_literal_; }
    
    // 各模块指针
    std::shared_ptr<TraditionalImapsFsm<ConnectionType>> fsm_;
    
    // 状态
    ImapState state_;
    ImapEvent next_event_;
    std::string current_tag_;
    std::string current_command_;
    std::string last_command_args_;
    ImapContext context_;
    
    // Literal 处理
    bool awaiting_literal_ = false;
    size_t expected_literal_size_ = 0;
    std::string literal_data_buffer_;

    // handle_read 解析完成标志，process_read 据此分发 FSM
    bool command_parsed_ = false;
    bool idle_done_received_ = false;
};

using TcpImapsSession = ImapsSession<TcpConnection>;
using SslImapsSession = ImapsSession<SslConnection>;

} // namespace mail_system


#endif // IMAPS_SESSION_H
