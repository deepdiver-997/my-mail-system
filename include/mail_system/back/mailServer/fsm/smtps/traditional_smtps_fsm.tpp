#ifndef TRADITIONAL_SMTPS_FSM_TPP
#define TRADITIONAL_SMTPS_FSM_TPP

#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/inbound/inbound_verifier.h"
#include "mail_system/back/common/mapped_file.h"
#include "mail_system/back/common/mime_parser.h"
#include <filesystem>
#include <openssl/md5.h>
#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/mailServer/smtps_server.h"

namespace mail_system {

// sync-bridge: 通过 async API 同步获取结果（默认同步实现，回调在返回前触发）
namespace {

// 密码验证：自动检测 bcrypt / MD5 / 明文
inline bool verify_password(const std::string& input, const std::string& stored) {
    if (stored.size() >= 2 && stored[0] == '$' && stored[1] == '2')
        return bcrypt_verify(input, stored);
    if (stored.size() == 32 && std::all_of(stored.begin(), stored.end(),
            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
        unsigned char md5[MD5_DIGEST_LENGTH];
        MD5(reinterpret_cast<const unsigned char*>(input.data()), input.size(), md5);
        char hex[33];
        for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) snprintf(hex + i*2, 3, "%02x", md5[i]);
        return stored == std::string(hex, 32);
    }
    return stored == input;
}
inline std::shared_ptr<IDBResult> sq(class IDBConnection* c, const std::string& sql,
                                      const std::vector<std::string>& params) {
    std::shared_ptr<IDBResult> r;
    c->async_query(sql, params, [&r](auto res) { r = std::move(res); });
    return r;
}
inline std::shared_ptr<IDBResult> sq(class IDBConnection* c, const std::string& sql) {
    std::shared_ptr<IDBResult> r;
    c->async_query(sql, [&r](auto res) { r = std::move(res); });
    return r;
}
inline bool se(class IDBConnection* c, const std::string& sql,
                const std::vector<std::string>& params) {
    bool ok = false;
    c->async_execute(sql, params, [&ok](bool r) { ok = r; });
    return ok;
}
inline bool se(class IDBConnection* c, const std::string& sql) {
    bool ok = false;
    c->async_execute(sql, [&ok](bool r) { ok = r; });
    return ok;
}
} // namespace

// ========== 工具函数实现 ==========
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_streamed_attachments(SmtpsContext* ctx) {
    if (ctx) algorithm::cleanup_streamed_attachments(*ctx);
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_mail_files(mail* m) {
    if (!m->body_path.empty()) std::remove(m->body_path.c_str());
    for (const auto& p : m->attachments)
        if (!p.filepath.empty()) std::remove(p.filepath.c_str());
}

// ========== 持久化函数实现 ==========
template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::persist_mails_sync(
    SmtpsSession<ConnectionType>* session, std::string& error)
{
    if (!session->mail_) { LOG_SMTP_DETAIL_WARN("No mail to persist"); return true; }
    LOG_SMTP_DETAIL_DEBUG("Starting to persist 1 mail");
    auto& m = session->mail_;
    LOG_SMTP_DETAIL_DEBUG("Mail ID: {} Body size: {}", m->id, m->body.size());
    LOG_SMTP_DETAIL_DEBUG("Saving to file: {}", m->body_path);

    if (m->persist_status == mail_system::persist_storage::PersistStatus::SUCCESS) {
        LOG_SMTP_DETAIL_DEBUG("Mail already persisted, skipping");
        return true;
    } else {
        if (m->persist_status == mail_system::persist_storage::PersistStatus::PENDING) {
            LOG_SMTP_DETAIL_DEBUG("Mail persist status not started, skipping");
            m->persist_status = mail_system::persist_storage::PersistStatus::CANCELLED;
        }
        cleanup_mail_files(m.get());
        LOG_SMTP_DETAIL_DEBUG("Cleaned up previous mail files due to failed persist");
        return false;
    }
    LOG_SMTP_DETAIL_DEBUG("All mails persisted successfully");
    return true;
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::persist_and_reply(std::shared_ptr<SessionBase<ConnectionType>> session) {
    auto* smtp_session = static_cast<SmtpsSession<ConnectionType>*>(session.get());
    std::string error;
    bool ok = persist_mails_sync(smtp_session, error);
    std::string reply = ok ? "221 Bye\r\n" : ("451 " + error + "\r\n");
    session->do_async_write(reply,
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) LOG_SMTP_DETAIL_ERROR("Error sending QUIT reply: {}", ec.message());
            auto io_ctx = static_cast<SmtpsServer*>(s->get_server())->get_io_context();
            auto timer = std::make_shared<boost::asio::steady_timer>(*io_ctx);
            timer->expires_after(std::chrono::milliseconds(100));
            timer->async_wait([s, timer](const boost::system::error_code& ec) mutable {
                if (!ec) s->close();
            });
        });
    return ok;
}

// ========== 初始化 ==========
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::init_transition_table() {
    this->add_transition(SmtpsState::INIT, SmtpsEvent::CONNECT, SmtpsState::GREETING);
    this->add_transition(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH);
    this->add_transition(SmtpsState::GREETING, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH);
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>)
        this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::STARTTLS, SmtpsState::INIT);
    this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH_USERNAME);
    this->add_transition(SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH_PASSWORD);
    this->add_transition(SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH, SmtpsState::WAIT_MAIL_FROM);
    this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH);
    this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE);
    this->add_transition(SmtpsState::WAIT_DATA, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE);
    this->add_transition(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE);
    this->add_transition(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END, SmtpsState::WAIT_QUIT);
    this->add_transition(SmtpsState::WAIT_QUIT, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);

    for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
        this->add_transition(static_cast<SmtpsState>(i), SmtpsEvent::QUIT, SmtpsState::CLOSED);
    }
    for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
        this->add_transition(static_cast<SmtpsState>(i), SmtpsEvent::ERROR, static_cast<SmtpsState>(i));
        this->add_transition(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT, static_cast<SmtpsState>(i));
    }
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::init_state_handlers() {
    auto h = [this](auto handler) {
        return [this, handler](auto s) { (this->*handler)(s); };
    };

    this->add_handler(SmtpsState::INIT, SmtpsEvent::CONNECT,
        h(&TraditionalSmtpsFsm::handle_init_connect));
    this->add_handler(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO,
        h(&TraditionalSmtpsFsm::handle_greeting_ehlo));
    this->add_handler(SmtpsState::GREETING, SmtpsEvent::EHLO,
        h(&TraditionalSmtpsFsm::handle_greeting_ehlo));

    if constexpr (!std::is_same_v<ConnectionType, boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>)
        this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::STARTTLS,
            h(&TraditionalSmtpsFsm::handle_wait_auth_starttls));

    this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH,
        h(&TraditionalSmtpsFsm::handle_wait_auth_auth));
    this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::EHLO,
        h(&TraditionalSmtpsFsm::handle_greeting_ehlo));
    this->add_handler(SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH,
        h(&TraditionalSmtpsFsm::handle_wait_auth_username));
    this->add_handler(SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH,
        h(&TraditionalSmtpsFsm::handle_wait_auth_password));
    this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_auth_mail_from));
    this->add_handler(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_mail_from_mail_from));
    this->add_handler(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO,
        h(&TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to));
    this->add_handler(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::DATA,
        h(&TraditionalSmtpsFsm::handle_wait_data_data));
    this->add_handler(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_auth_mail_from));
    this->add_handler(SmtpsState::WAIT_DATA, SmtpsEvent::DATA,
        h(&TraditionalSmtpsFsm::handle_wait_data_data));
    this->add_handler(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA,
        h(&TraditionalSmtpsFsm::handle_in_message_data));
    this->add_handler(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END,
        h(&TraditionalSmtpsFsm::handle_in_message_data_end));

    for (int i = 1; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i)
        this->add_handler(static_cast<SmtpsState>(i), SmtpsEvent::QUIT,
            h(&TraditionalSmtpsFsm::handle_wait_quit_quit));
    this->add_handler(SmtpsState::WAIT_QUIT, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_auth_mail_from));

    for (int i = 0; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
        this->add_handler(static_cast<SmtpsState>(i), SmtpsEvent::ERROR,
            h(&TraditionalSmtpsFsm::handle_error));
        this->add_handler(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT,
            h(&TraditionalSmtpsFsm::handle_timeout));
    }
}

// ========== 事件处理 ==========
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::process_event(
    std::shared_ptr<SessionBase<ConnectionType>> session, SmtpsEvent event)
{
    if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
        LOG_SMTP_DETAIL_DEBUG("Current State: {}, Event: {}",
            TraditionalSmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
            TraditionalSmtpsFsm<ConnectionType>::get_event_name(event));
    }
    this->dispatch(session, static_cast<SmtpsState>(session->get_current_state()), event);
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::is_terminal_state(SmtpsState s) const {
    return s == SmtpsState::CLOSED;
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::on_invalid_transition(
    SmtpsState, SmtpsEvent,
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->set_error(SessionError::InvalidCommand);
    session->do_async_write("500 Error: Invalid command sequence\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) return;
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::auto_process_event(std::shared_ptr<SessionBase<ConnectionType>> session) {
    process_event(session,
        static_cast<SmtpsEvent>(session->get_next_event()));
}

// ========== 各状态处理函数 ==========

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_init_connect(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->set_current_state(static_cast<int>(SmtpsState::GREETING));
    session->do_async_write("220 SMTPS Server\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> self, const boost::system::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("Error sending greeting: {}", ec.message()); return; }
            self->set_current_state(static_cast<int>(SmtpsState::WAIT_EHLO));
            self->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_greeting_ehlo(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    LOG_SMTP_DETAIL_DEBUG("Received EHLO: {}");
    if (auto* ctx = static_cast<SmtpsContext*>(session->get_context()))
        ctx->ehlo_domain = session->get_last_command_args();

    // RFC 5321: EHLO 响应首行必须是服务器自身的域名（非回显客户端 EHLO 参数）。
    // Gmail 会校验该主机名：若服务器自称是 "*.google.com"（回显客户端）会触发安全中止。
    auto cfg = std::atomic_load(&session->get_server()->m_config);
    std::string helo = cfg->helo_hostname.empty() ? cfg->system_domain : cfg->helo_hostname;
    if (helo.empty()) helo = session->get_last_command_args();

    std::string response = "250-" + helo + " Hello\r\n"
        "250-SIZE 10240000\r\n"
        "250-8BITMIME\r\n";
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>)
        response += "250-STARTTLS\r\n";
    {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        if (ctx->listener_config.auth_policy != InboundAuthPolicy::OFF)
            response += "250-AUTH LOGIN PLAIN\r\n";
    }
    response += "250 SMTPUTF8\r\n";

    session->do_async_write(response,
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("Error sending EHLO: {}", ec.message()); return; }
            s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_starttls(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->do_async_write("220 Ready to start TLS\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> self, const boost::system::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("Error sending STARTTLS: {}", ec.message()); return; }
            self->set_current_state(static_cast<int>(SmtpsState::INIT));
            auto server = static_cast<SmtpsServer*>(self->get_server());
            auto trace = self->take_trace_buffer();   // 交接给 TLS 会话，延续对话记录
            auto tcp_sock = self->release_connection()->release_socket();
            server->handoff_starttls_socket(std::move(tcp_sock), std::move(trace));
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_auth(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<SmtpsContext*>(session->get_context());

    // AUTH PLAIN step2
    if (ctx->plain_auth_expected) {
        ctx->plain_auth_expected = false;
        std::string decoded = mail_system::outbound::base64_decode(session->get_last_command_args());
        auto null1 = decoded.find('\0');
        if (null1 != std::string::npos) {
            auto null2 = decoded.find('\0', null1 + 1);
            std::string username = decoded.substr(null1 + 1, null2 - null1 - 1);
            std::string password = (null2 != std::string::npos) ? decoded.substr(null2 + 1) : "";
            ctx->client_username = username;
            if (username.find('@') == std::string::npos) {
                auto cfg = std::atomic_load(&session->get_server()->m_config);
                username += "@" + cfg->system_domain;
                ctx->client_username = username;
            }
            this->auth_user_async(session, username, password,
                [session, ctx](bool ok, int shard) {
                    if (ok) {
                        ctx->is_authenticated = true; ctx->shard_index = shard;
                        session->do_async_write("235 Authentication successful\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    } else {
                        if (session->record_auth_failure_and_check()) { session->close(); return; }
                        session->do_async_write("535 Authentication failed\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    }
                });
            return;
        }
    }

    // AUTH PLAIN 1-step
    std::string upper_args = session->get_last_command_args();
    std::transform(upper_args.begin(), upper_args.end(), upper_args.begin(), ::toupper);
    if (upper_args.find("PLAIN") == 0) {
        std::string token = session->get_last_command_args().length() > 6 ? session->get_last_command_args().substr(6) : "";
        if (token.empty()) {
            ctx->plain_auth_expected = true;
            session->do_async_write("334 \r\n",
                [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                    s->do_async_read();
                });
            return;
        }
        std::string decoded = mail_system::outbound::base64_decode(token);
        auto null1 = decoded.find('\0');
        if (null1 != std::string::npos) {
            auto null2 = decoded.find('\0', null1 + 1);
            std::string username = decoded.substr(null1 + 1, null2 - null1 - 1);
            std::string password = (null2 != std::string::npos) ? decoded.substr(null2 + 1) : "";
            ctx->client_username = username;
            if (username.find('@') == std::string::npos) {
                auto cfg = std::atomic_load(&session->get_server()->m_config);
                username += "@" + cfg->system_domain;
                ctx->client_username = username;
            }
            this->auth_user_async(session, username, password,
                [session, ctx](bool ok, int shard) {
                    if (ok) {
                        ctx->is_authenticated = true; ctx->shard_index = shard;
                        session->do_async_write("235 Authentication successful\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    } else {
                        if (session->record_auth_failure_and_check()) { session->close(); return; }
                        session->do_async_write("535 Authentication failed\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    }
                });
            return;
        }
        handle_wait_auth_auth_login(session);
        return;
    }
    handle_wait_auth_auth_login(session);
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_auth_login(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->do_async_write("334 VXNlcm5hbWU6\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("AUTH username prompt: {}", ec.message()); return; }
            s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH_USERNAME));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_username(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    std::string decoded = mail_system::outbound::base64_decode(session->get_last_command_args());
    static_cast<SmtpsContext*>(session->get_context())->client_username = decoded;
    session->do_async_write("334 UGFzc3dvcmQ6\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("AUTH password prompt: {}", ec.message()); return; }
            s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH_PASSWORD));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_password(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    std::string username = static_cast<SmtpsContext*>(session->get_context())->client_username;
    std::string password = mail_system::outbound::base64_decode(session->get_last_command_args());
    if (username.find('@') == std::string::npos) {
        auto cfg = std::atomic_load(&session->get_server()->m_config);
        username += "@" + cfg->system_domain;
        static_cast<SmtpsContext*>(session->get_context())->client_username = username;
    }
    this->auth_user_async(session, username, password,
        [session](bool ok, int shard) {
            auto* ctx = static_cast<SmtpsContext*>(session->get_context());
            if (ok) {
                ctx->is_authenticated = true; ctx->shard_index = shard;
                session->do_async_write("235 Authentication successful\r\n",
                    [session](auto s, auto& ec) {
                        if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                        s->drain_buffered_commands();
                        if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                    });
            } else {
                if (session->record_auth_failure_and_check()) { session->close(); return; }
                session->do_async_write("535 Authentication failed\r\n",
                    [session](auto s, auto& ec) {
                        if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                        s->drain_buffered_commands();
                        if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                    });
            }
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_mail_from(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    // AUTH policy check — 外部发件人是否需要认证：
    // - listener auth_policy=ON（465/587 提交端口）始终要求认证
    // - listener auth_policy=AUTO（25）按 inbound_auth_policy 决定，默认 off = 无需认证
    //   之前错误地按 listener AUTO 处理（require_auth=!is_trusted_server，而该值从未被置真），
    //   导致所有外部入站投递都被 530 拒绝
    {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        const auto& lc = ctx->listener_config;
        auto cfg = std::atomic_load(&session->get_server()->m_config);

        bool require_auth = false;
        switch (lc.auth_policy) {
        case InboundAuthPolicy::ON:
            require_auth = true;
            break;
        case InboundAuthPolicy::AUTO:
            switch (cfg->inbound_auth_policy) {
            case InboundAuthPolicy::ON:   require_auth = true; break;
            case InboundAuthPolicy::AUTO: require_auth = !ctx->is_trusted_server; break;
            case InboundAuthPolicy::OFF:  require_auth = false; break;
            }
            break;
        case InboundAuthPolicy::OFF:
            break;
        }

        if (require_auth && !ctx->is_authenticated) {
            session->do_async_write("530 5.7.1 Authentication required\r\n",
                [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                    s->do_async_read();
                });
            return;
        }
    }

    auto addr_start = session->get_last_command_args().find('<');
    auto addr_end   = session->get_last_command_args().find('>', addr_start != std::string::npos ? addr_start + 1 : 0);
    if (addr_start != std::string::npos && addr_end != std::string::npos) {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        if (ctx->is_authenticated && !ctx->client_username.empty()) {
            ctx->sender_address = ctx->client_username;
        } else {
            ctx->sender_address = session->get_last_command_args().substr(addr_start + 1, addr_end - addr_start - 1);
        }
        ctx->recipient_addresses.clear();
        ctx->spf_checked = false;

        // SPF check — 异步（worker 线程池执行 DNS 查询，不阻塞 io_context）
        auto cfg = std::atomic_load(&session->get_server()->m_config);
        bool need_spf = !cfg->perf_mode && cfg->inbound_spf_mode != "off" &&
                        !ctx->sender_address.empty() && ctx->sender_address != "<>";
        if (!need_spf) {
            session->do_async_write("250 Ok\r\n",
                [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                    s->do_async_read();
                });
            return;
        }

        auto* dns = session->get_server()->get_dns_resolver().get();
        if (!dns) {
            session->do_async_write("250 Ok\r\n",
                [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                    s->do_async_read();
                });
            return;
        }

        std::string client_ip = session->get_client_ip();
        std::string sender = ctx->sender_address;
        std::string ehlo = ctx->ehlo_domain;
        std::string spf_mode = cfg->inbound_spf_mode;

        // 发起 DNS 前显式置阻塞：状态机函数返回后 io_context 线程不会继续处理本 session
        // （do_async_read 流水线循环检查 is_paused），DNS 回调独占操作，无需 post 回 io_context
        session->set_paused(true);
        inbound::InboundVerifier::check_spf_only_async(*dns, client_ip, sender, ehlo,
            [session, sender, spf_mode](inbound::SpfResult spf) {
                session->set_paused(false);
                auto* c = static_cast<SmtpsContext*>(session->get_context());
                c->spf_checked = true;
                c->spf_result = spf.result;
                c->spf_reason = spf.reason;
                std::string reject;
                if (spf.result == "fail" && spf_mode == "hard")
                    reject = "SPF hard-fail for " + sender;
                if (!reject.empty()) {
                    session->do_async_write("550 5.7.1 SPF verification failed: " + reject + "\r\n",
                        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                            if (ec) return;
                            s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                            s->do_async_read();
                        });
                } else {
                    session->do_async_write("250 Ok\r\n",
                        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                            if (ec) return;
                            s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                            s->drain_buffered_commands();
                            if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                        });
                }
            });
    } else {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) return;
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                s->do_async_read();
            });
    }
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_mail_from_mail_from(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    handle_wait_auth_mail_from(session);
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_rcpt_to_rcpt_to(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto addr_start = session->get_last_command_args().find('<');
    auto addr_end   = session->get_last_command_args().find('>', addr_start != std::string::npos ? addr_start + 1 : 0);
    if (addr_start == std::string::npos || addr_end == std::string::npos) {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    std::string recipient = algorithm::trim(
        session->get_last_command_args().substr(addr_start + 1, addr_end - addr_start - 1));
    if (recipient.empty()) {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    auto* ctx = static_cast<SmtpsContext*>(session->get_context());

    // 认证客户端（465/587 提交）：允许任意收件人（含外部域名，按提交模式转发）
    if (ctx->is_authenticated) {
        ctx->recipient_addresses.push_back(recipient);
        session->do_async_write("250 Ok\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) return;
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                s->do_async_read();
            });
        return;
    }

    // 未认证（25 端口 MTA 入站）：禁止中继，只收本地已注册用户
    auto cfg = std::atomic_load(&session->get_server()->m_config);
    std::string local = recipient;
    std::string domain;
    auto at = recipient.rfind('@');
    if (at != std::string::npos) {
        local = recipient.substr(0, at);
        domain = recipient.substr(at + 1);
    }
    local = algorithm::trim(local);

    // 外部域名（非本系统域名）→ 拒绝中继
    if (algorithm::to_lower(domain) != algorithm::to_lower(cfg->system_domain)) {
        LOG_SMTP_INFO("RCPT relay denied: {} (domain {})", recipient, domain);
        session->do_async_write("550 5.7.1 Relay access denied\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    // 本地域名但无本地部分 → 语法错误
    if (local.empty()) {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    // 本地域名 → 校验用户存在性（DB 或缓存）
    this->user_exists_async(session, recipient,
        [session, recipient](bool exists) {
            auto* c = static_cast<SmtpsContext*>(session->get_context());
            const char* resp = exists ? "250 Ok\r\n" : "550 5.1.1 User unknown\r\n";
            if (!exists) LOG_SMTP_WARN("RCPT user unknown: {}", recipient);
            if (exists) c->recipient_addresses.push_back(recipient);
            session->do_async_write(resp,
                [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                    s->drain_buffered_commands();
                    if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                });
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_data_data(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (smtp_session) smtp_session->create_mail_on_data_command();

    session->do_async_write("354 Start mail input; end with <CRLF>.<CRLF>\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) return;
            s->set_current_state(static_cast<int>(SmtpsState::IN_MESSAGE));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_in_message_data(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->do_async_read();
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_in_message_data_end(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<SmtpsContext*>(session->get_context());
    std::string resp = ctx->abort_reason.empty() ? "552 Message size exceeds limit\r\n" : ctx->abort_reason + "\r\n";
    if (ctx->body_limit_exceeded) {
        cleanup_streamed_attachments(ctx);
        session->do_async_write(resp,
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable { s->close(); });
        return;
    }

    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (!smtp_session) { session->close(); return; }

    // 正文没能落盘就绝不能往下走去回 250：那等于骗发送方 MTA 把邮件从队列里删掉。
    // 回 451 让对方稍后重投。
    if (!smtp_session->commit_body()) {
        cleanup_streamed_attachments(ctx);
        smtp_session->discard_current_mail();
        session->do_async_write("451 4.3.0 Failed to store message, try again later\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable {
                s->close();
            });
        return;
    }

    try {
    auto cfg = std::atomic_load(&session->get_server()->m_config);

    // MIME 预解析：仅当消息 ≤ 阈值才 eager 解析（大消息跳过，交给 IMAP lazy 解析）
    // 供 IMAP BODYSTRUCTURE / BODY[section] 直接使用。
    //
    // 用 mmap 而非读进 std::string：正文刚由本会话写完并 fsync 过，这里只是
    // 换个视角再看一遍同一份数据，没必要在堆上再复制一份（每封最多 1 MiB，
    // 并发时叠加，而这台机器可用内存不到 1 GB）。
    std::unique_ptr<MappedFile> body_map;
    if (smtp_session->get_mail() && !smtp_session->get_mail()->body_path.empty()) {
        std::error_code fec;
        auto fsz = std::filesystem::file_size(smtp_session->get_mail()->body_path, fec);
        if (!fec && fsz > 0 && static_cast<std::uintmax_t>(cfg->inbound_mime_parse_limit_bytes) >= fsz) {
            std::string map_err;
            body_map = MappedFile::open(smtp_session->get_mail()->body_path, map_err);
            if (!body_map) {
                LOG_FILE_IO_ERROR("Failed to map mail body for MIME parse: {}", map_err);
            }
        }
    }
    if (body_map && !body_map->empty()) {
        parse_mime_tree(body_map->view(), smtp_session->get_mail()->mime_root);
        // 写 sidecar JSON 文件，供 IMAP 直接读取（失败不阻塞收信）
        save_mime_tree(smtp_session->get_mail()->body_path,
                       smtp_session->get_mail()->mime_root);
    }

    // Inbound verification (DKIM/DMARC/SPF) — 异步（worker 线程池执行 DNS/验签，不阻塞 io_context）
    bool needs_verify = !cfg->perf_mode && (
        (!ctx->spf_checked && cfg->inbound_spf_mode != "off") ||
         cfg->inbound_dkim_mode != "off" || cfg->inbound_dmarc_mode != "off");

    // 入队 + 响应（after_enqueue / after_persist）。vr 有值则先记录校验结果。
    auto finalize_mail = [session, smtp_session](std::optional<inbound::VerificationResult> vr) {
        try {
            if (vr) {
                auto* c = static_cast<SmtpsContext*>(session->get_context());
                c->dkim_result = vr->dkim.result;
                c->dmarc_result = vr->dmarc.result;
                if (!c->spf_checked) { c->spf_result = vr->spf.result; c->spf_checked = true; }
                c->verification_run = true;
            }
            auto submit_result = smtp_session->submit_mail_to_queue();
            if (!submit_result.accepted) {
                smtp_session->discard_current_mail();
                session->do_async_write("451 Requested action aborted: insufficient storage or backend pressure\r\n",
                    [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable { s->close(); });
                return;
            }
            const bool ack_after_enqueue =
                std::atomic_load(&session->get_server()->m_config)->inbound_ack_mode == InboundAckMode::AFTER_ENQUEUE;
            if (ack_after_enqueue) {
                session->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                smtp_session->reset_mail_state();
                session->get_server()->increment_mails_accepted();
                session->do_async_write("250 OK\r\n",
                    [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                        if (ec) return;
                        s->do_async_read();
                    });
                return;
            }
            // after_persist: wait for persistence, then reply
            auto pool = session->get_server()->m_workerThreadPool;
            pool->post([s = session]() mutable {
                auto* smtp_s = dynamic_cast<SmtpsSession<ConnectionType>*>(s.get());
                if (!smtp_s) { if (s) s->close(); return; }
                if (!smtp_s->has_pending_mail_submission()) { s->close(); return; }

                const auto wait_to = std::chrono::milliseconds(
                    std::atomic_load(&s->get_server()->m_config)->inbound_persist_wait_timeout_ms);
                auto deadline = std::chrono::steady_clock::now() + wait_to;
                auto backoff = std::chrono::milliseconds(50);
                while (std::chrono::steady_clock::now() < deadline) {
                    auto status = smtp_s->get_pending_mail_persist_status();
                    if (status == persist_storage::PersistStatus::SUCCESS) {
                        s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                        smtp_s->reset_mail_state();
                        s->get_server()->increment_mails_accepted();
                        s->do_async_write("250 OK\r\n",
                            [](std::shared_ptr<SessionBase<ConnectionType>> ss, const boost::system::error_code& ec) mutable {
                                if (ec) return;
                                ss->do_async_read();
                            });
                        return;
                    }
                    if (status == persist_storage::PersistStatus::FAILED ||
                        status == persist_storage::PersistStatus::CANCELLED) {
                        smtp_s->clear_pending_mail_submission();
                        s->do_async_write("451 Requested action aborted: local processing error\r\n",
                            [](std::shared_ptr<SessionBase<ConnectionType>> ss, const boost::system::error_code&) mutable { ss->close(); });
                        return;
                    }
                    std::this_thread::sleep_for(backoff);
                    backoff = std::min(backoff * 2, std::chrono::milliseconds(400));
                }
                smtp_s->cancel_pending_mail_submission();
                smtp_s->clear_pending_mail_submission();
                s->do_async_write("451 Requested action aborted: local processing timeout\r\n",
                    [](std::shared_ptr<SessionBase<ConnectionType>> ss, const boost::system::error_code&) mutable { ss->close(); });
            });
        } catch (const std::exception& e) {
            LOG_SMTP_DETAIL_ERROR("DATA_END processing exception: {}", e.what());
            if (smtp_session) smtp_session->discard_current_mail();
            session->do_async_write("451 Requested action aborted: local processing error\r\n",
                [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable { s->close(); });
        }
    };

    if (needs_verify && !ctx->verification_run) {
        std::string client_ip = session->get_client_ip();
        std::string mail_from = ctx->sender_address;
        std::string helo = ctx->ehlo_domain;
        std::string headers = ctx->header_buffer;
        const std::string body_path = (smtp_session->get_mail() && !smtp_session->get_mail()->body_path.empty())
            ? smtp_session->get_mail()->body_path : std::string();

        auto* dns = session->get_server()->get_dns_resolver().get();
        if (dns && !body_path.empty()) {
            inbound::SpfResult pre_spf{ctx->spf_result, ""};
            session->set_paused(true);   // 校验期间暂停流水线，回调中恢复
            inbound::InboundVerifier::verify_all_from_file_async(*dns,
                client_ip, mail_from, helo, headers, body_path, *cfg,
                [session, finalize_mail = std::move(finalize_mail)](inbound::VerificationResult vr) mutable {
                    // c-ares 回调独占操作（session 已 paused），直接执行，无需 post 回 io_context
                    session->set_paused(false);
                    finalize_mail(std::move(vr));
                },
                ctx->spf_checked ? &pre_spf : nullptr);
            return;
        }
        // dns 不可用或 body_path 空 → 跳过校验，直接入队
    }

    // 同步路径：不需要校验 或 无法校验 → 直接入队 + 响应
    finalize_mail(std::nullopt);
    } catch (const std::exception& e) {
        LOG_SMTP_DETAIL_ERROR("DATA_END processing exception: {}", e.what());
        if (smtp_session) smtp_session->discard_current_mail();
        session->do_async_write("451 Requested action aborted: local processing error\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable { s->close(); });
        return;
    }
}
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_quit_quit(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->set_trace_clean_close();   // 正常 QUIT → 连接追踪丢弃
    session->do_async_write("221 Bye\r\n",
        [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) LOG_SMTP_DETAIL_ERROR("QUIT reply error: {}", ec.message());
            if (s) s->close();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_timeout(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    session->handle_read("");
    if (static_cast<SmtpsEvent>(session->get_next_event()) != SmtpsEvent::TIMEOUT) {
        auto_process_event(session);
        return;
    }
    session->do_async_read();
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_error(
    std::shared_ptr<SessionBase<ConnectionType>> session)
{
    LOG_SMTP_DETAIL_WARN("SMTP error: state={} session->get_last_command_args()=[{}] stay={}",
        TraditionalSmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
        session->get_last_command_args(), session->stay_times_);
    session->stay_times_++;
    if (session->stay_times_ > 3) {
        session->close();
    } else {
        session->do_async_write("500 Error: " + session->get_last_command_args() + "\r\n",
            [](std::shared_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
    }
}

// ========== 从 SmtpsFsm 迁移的方法实现 ==========

template <typename ConnectionType>
std::string TraditionalSmtpsFsm<ConnectionType>::get_state_name(SmtpsState state) {
    static const std::unordered_map<SmtpsState, std::string> state_names = {
        {SmtpsState::INIT, "INIT"},
        {SmtpsState::GREETING, "GREETING"},
        {SmtpsState::WAIT_EHLO, "WAIT_EHLO"},
        {SmtpsState::WAIT_AUTH, "WAIT_AUTH"},
        {SmtpsState::WAIT_AUTH_USERNAME, "WAIT_AUTH_USERNAME"},
        {SmtpsState::WAIT_AUTH_PASSWORD, "WAIT_AUTH_PASSWORD"},
        {SmtpsState::WAIT_MAIL_FROM, "WAIT_MAIL_FROM"},
        {SmtpsState::WAIT_RCPT_TO, "WAIT_RCPT_TO"},
        {SmtpsState::WAIT_DATA, "WAIT_DATA"},
        {SmtpsState::IN_MESSAGE, "IN_MESSAGE"},
        {SmtpsState::WAIT_QUIT, "WAIT_QUIT"},
        {SmtpsState::CLOSED, "CLOSED"}
    };
    auto it = state_names.find(state);
    if (it != state_names.end()) {
        return it->second;
    }
    return "UNKNOWN_STATE";
}

template <typename ConnectionType>
std::string TraditionalSmtpsFsm<ConnectionType>::get_event_name(SmtpsEvent event) {
    static const std::unordered_map<SmtpsEvent, std::string> event_names = {
        {SmtpsEvent::CONNECT, "CONNECT"},
        {SmtpsEvent::EHLO, "EHLO"},
        {SmtpsEvent::AUTH, "AUTH"},
        {SmtpsEvent::STARTTLS, "STARTTLS"},
        {SmtpsEvent::MAIL_FROM, "MAIL_FROM"},
        {SmtpsEvent::RCPT_TO, "RCPT_TO"},
        {SmtpsEvent::DATA, "DATA"},
        {SmtpsEvent::DATA_END, "DATA_END"},
        {SmtpsEvent::QUIT, "QUIT"},
        {SmtpsEvent::ERROR, "ERROR"},
        {SmtpsEvent::TIMEOUT, "TIMEOUT"}
    };
    auto it = event_names.find(event);
    if (it != event_names.end()) {
        return it->second;
    }
    return "UNKNOWN_EVENT";
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::auth_user_async(
    std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& mail_address,
    const std::string& password, AuthCallback cb)
{
    if (!session) { LOG_AUTH_ERROR("Session is null in auth_user"); cb(false, 0); return; }

    int shard = 0;
    if (m_shardRouter) { int r = m_shardRouter->route(mail_address); if (r >= 0) shard = r; }

    // 查缓存 —— 命中则同步回调，无需暂停流水线
    AuthCacheEntry ce;
    if (m_authCache->lookup(mail_address, ce)) {
        if (ce.status != 1) { cb(false, 0); return; }
        shard = ce.shard;
        bool ok = verify_password(password, ce.password_hash);
        cb(ok, shard);
        return;
    }

    // 缓存未命中，查 DB
    auto conn_raw = acquire_connection(shard);
    if (!conn_raw.is_valid()) { LOG_AUTH_ERROR("Failed to get DB connection"); cb(false, 0); return; }
    auto conn = std::make_shared<ScopedConnection>(std::move(conn_raw));

    // 暂停流水线：DB 查询期间不消费新命令
    // 注意：调用者必须在回调中调用 session->drain_buffered_commands()
    session->set_paused(true);

    std::string sql = db::sql::build_auth_user_query();
    (*conn)->async_query(sql, {mail_address},
        [this, session, mail_address, password, shard, conn,
         cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (!result || result->get_row_count() == 0) {
                LOG_AUTH_WARN("User not found: {}", mail_address);
                cb(false, 0);
                return;
            }
            int status = std::stoi(result->get_value(0, "status"));
            if (status != 1) {
                LOG_AUTH_WARN("User account disabled: {}", mail_address);
                cb(false, 0);
                return;
            }
            std::string stored = result->get_value(0, "password");
            m_authCache->store(mail_address, {stored, status, 0, shard});
            bool ok = verify_password(password, stored);
            if (ok) {
                (*conn)->async_execute(db::sql::build_update_last_login(), {mail_address},
                                       [cb = std::move(cb), ok, shard](bool) { cb(ok, shard); });
            } else {
                cb(false, 0);
            }
        });
}


template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::user_exists_async(
    std::shared_ptr<SessionBase<ConnectionType>> session, const std::string& mail_address,
    std::function<void(bool exists)> cb)
{
    if (!session) { cb(false); return; }

    int shard = 0;
    if (m_shardRouter) { int r = m_shardRouter->route(mail_address); if (r >= 0) shard = r; }

    // 快路径：缓存命中（含负缓存 status=0）直接回调
    AuthCacheEntry ce;
    if (m_recipientCache->lookup(mail_address, ce)) { cb(ce.status == 1); return; }

    // 缓存未命中，查 DB
    std::shared_ptr<DBPool> pool = m_shardRouter ? m_shardRouter->get_db_pool(static_cast<size_t>(shard)) : nullptr;
    if (!pool) { LOG_SMTP_WARN("RCPT check no DB pool for shard {}", shard); cb(false); return; }
    auto conn_raw = pool->acquire_connection();
    if (!conn_raw.is_valid()) { LOG_SMTP_WARN("RCPT check failed to get DB connection"); cb(false); return; }
    auto conn = std::make_shared<ScopedConnection>(std::move(conn_raw));

    // 暂停流水线：DB 查询期间不消费新命令，回调中 drain_buffered_commands()
    session->set_paused(true);

    std::string sql = db::sql::build_recipient_exists_query();
    (*conn)->async_query(sql, {mail_address},
        [this, session, mail_address, conn,
         cb = std::move(cb)](std::shared_ptr<IDBResult> result) mutable {
            if (!result || result->get_row_count() == 0) {
                // 负缓存：不存在
                m_recipientCache->store(mail_address, {"", 0, 0, 0});
                cb(false);
                return;
            }
            int status = std::stoi(result->get_value(0, "status"));
            m_recipientCache->store(mail_address, {"", status, 0, 0});
            cb(status == 1);
        });
}


template <typename ConnectionType>
std::future<bool> TraditionalSmtpsFsm<ConnectionType>::save_mail_metadata_async(
    mail* data, const std::string& file_path_prefix)
{
    std::future<bool> future;

    if (!data) {
        LOG_DATABASE_ERROR("Mail data is null in save_mail_metadata_async");
        return std::future<bool>();
    }

    if (!m_workerThreadPool) {
        LOG_DATABASE_ERROR("WorkerThreadPool is null in save_mail_metadata_async");
        return std::future<bool>();
    }

    mail& mail_data = *data;

    auto task = [this, file_path_prefix, mail_data]() -> bool {
        auto conn = this->acquire_connection(0);
        if (!conn.is_valid()) {
            LOG_DATABASE_ERROR("Failed to get database connection in async task");
            return false;
        }

        auto* conn_ptr = conn.operator->();

        bool success = true;

        // 第一步：插入邮件元数据到 mails 表
        std::string body_path = file_path_prefix;
        std::string mail_sql = db::sql::build_insert_mail_with_status(
            mail_data.id, mail_data.subject, body_path,
            mail_data.status, conn_ptr);
        LOG_DATABASE_DEBUG("Executing SQL: {}", mail_sql);
        if (!se(conn_ptr,mail_sql)) {
            LOG_DATABASE_ERROR("Failed to insert mail metadata. Error: {}", conn_ptr->get_last_error());
            return false;
        }

        // 第二步：插入邮件收发件人关系到 mail_recipients 表
        std::string recipient_sql = db::sql::build_insert_recipients_simple(
            mail_data, conn_ptr);
        LOG_DATABASE_DEBUG("Executing SQL: {}", recipient_sql);
        if (!recipient_sql.empty() && !se(conn_ptr,recipient_sql)) {
            LOG_DATABASE_ERROR("Failed to insert mail recipients. Error: {}", conn_ptr->get_last_error());
            // 如果插入收件人失败，删除已插入的邮件元数据
            se(conn_ptr,db::sql::build_delete_mail_by_id(mail_data.id));
            return false;
        }

        // 第三步：插入附件元数据
        if (!mail_data.attachments.empty()) {
            std::string att_sql = db::sql::build_insert_attachments(
                mail_data.id, mail_data.attachments, conn_ptr);
            LOG_DATABASE_DEBUG("Executing SQL: {}", att_sql);
            if (!se(conn_ptr,att_sql)) {
                LOG_DATABASE_ERROR("Failed to insert attachment metadata. Error: {}", conn_ptr->get_last_error());
                success = false;
            }
        }

        return success;
    };

    future = this->m_workerThreadPool->submit(std::move(task));

    return future;
}

template <typename ConnectionType>
std::future<bool> TraditionalSmtpsFsm<ConnectionType>::save_attachment_metadata_async(
    const attachment& att, size_t mail_id)
{
    if (!m_workerThreadPool) {
        LOG_DATABASE_ERROR("DBPool or WorkerThreadPool is null in save_attachment_metadata_async");
        return std::future<bool>();
    }

    auto task = [this, att, mail_id]() -> bool {
        auto conn = this->acquire_connection(0);
        if (!conn.is_valid()) {
            LOG_DATABASE_ERROR("Failed to get database connection in save_attachment_metadata_async");
            return false;
        }

        auto* conn_ptr = conn.operator->();
        if (!conn_ptr) {
            LOG_DATABASE_ERROR("Failed to cast to MySQLConnection");
            return false;
        }

        std::string att_sql = db::sql::build_insert_attachment_single(
            mail_id, att, conn_ptr);
        LOG_DATABASE_DEBUG("Executing SQL: {}", att_sql);
        if (!se(conn_ptr,att_sql)) {
            LOG_DATABASE_ERROR("Failed to insert attachment metadata. Error: {}", conn_ptr->get_last_error());
            return false;
        }
        return true;
    };

    return this->m_workerThreadPool->submit(std::move(task));
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::remove_metadata_by_file_path(
    const std::vector<std::string>& file_paths)
{
    auto conn = acquire_connection(0);
    if (!conn.is_valid()) {
        LOG_DATABASE_ERROR("Failed to get database connection in remove_metadata_by_file_path");
        return;
    }

    // 注意：数据库中 body_path 字段存储的是文件路径
    std::string sql = "DELETE FROM mails WHERE body_path = ?";
    for (const auto& file_path : file_paths) {
        if (!se(conn.operator->(), sql, {file_path})) {
            LOG_DATABASE_ERROR("Failed to delete mail metadata for file path: {}", file_path);
        }
    }
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::save_mail_body_to_file(
    mail* data, const std::string& file_path)
{
    if (!data) {
        LOG_FILE_IO_ERROR("Mail data is null in save_mail_body_to_file");
        return false;
    }

    LOG_FILE_IO_DEBUG("Saving mail body to file: {}, body size: {}", file_path, data->body.size());

    std::ofstream out(file_path);
    if (!out.is_open()) {
        LOG_FILE_IO_ERROR("Failed to open file for writing: {}", file_path);
        return false;
    }

    if (data->body.empty()) {
        out << data->header;
        out.close();
        LOG_FILE_IO_DEBUG("Mail body is empty, only header saved to file: {}", file_path);
        return true;
    }
    out << data->body;
    out.close();
    LOG_FILE_IO_DEBUG("Mail body saved successfully to file: {}", file_path);
    return true;
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::save_attachment_to_file(
    attachment& att, const std::string& file_path)
{
    std::ofstream out(file_path, std::ios::binary);
    if (!out.is_open()) {
        LOG_FILE_IO_ERROR("Failed to open attachment file for writing: {}", file_path);
        return false;
    }
    out.write(att.content.data(), static_cast<std::streamsize>(att.content.size()));
    out.close();
    att.filepath = file_path;
    att.file_size = att.content.size();
    att.content.clear(); // 释放内存
    LOG_FILE_IO_DEBUG("Attachment saved to file: {}", file_path);
    return true;
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_failed_saves(
    std::vector<std::future<bool>>& futures, const std::vector<std::string>& file_paths)
{
    for (size_t i = 0; i < futures.size(); ++i) {
        if (futures[i].valid()) {
            bool success = futures[i].get();
            if (!success && i < file_paths.size()) {
                LOG_FILE_IO_ERROR("Database save failed, removing file: {}", file_paths[i]);
                std::remove(file_paths[i].c_str());
            }
        }
    }
}

} // namespace mail_system

#endif // TRADITIONAL_SMTPS_FSM_TPP
