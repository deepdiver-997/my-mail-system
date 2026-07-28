#ifndef SMTPS_FSM_H
#define SMTPS_FSM_H

#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/auth_cache.h"
#include "mail_system/back/persist_storage/persistent_queue.h"
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "mail_system/back/common/bcrypt.h"
#include "mail_system/back/router/i_shard_router.h"
#include <fstream>

namespace mail_system {

// SMTPS 状态枚举
enum class SmtpsState {
    INIT = 0,
    GREETING = 1,
    WAIT_EHLO = 2,
    WAIT_AUTH = 3,
    WAIT_AUTH_USERNAME = 4,
    WAIT_AUTH_PASSWORD = 5,
    WAIT_MAIL_FROM = 6,
    WAIT_RCPT_TO = 7,
    WAIT_DATA = 8,
    IN_MESSAGE = 9,
    WAIT_QUIT = 10,
    CLOSED = 11
};

// SMTPS 事件枚举
enum class SmtpsEvent {
    CONNECT = 0,
    EHLO = 1,
    AUTH = 2,
    MAIL_FROM = 3,
    RCPT_TO = 4,
    DATA = 5,
    DATA_END = 6,
    QUIT = 7,
    STARTTLS = 8,
    ERROR = 9,
    TIMEOUT = 10
};

// SMTPS 上下文
struct SmtpsContext {
    ListenerConfig listener_config;               // 当前连接所属监听器配置
    bool is_authenticated = false;               // AUTH 是否通过，决定后续命令是否允许
    bool plain_auth_expected = false;            // AUTH PLAIN 多步流程：等待客户端发送 base64 凭证
    std::string client_username;                 // AUTH 登录名；MAIL FROM 缺省时作为发件人
    std::string sender_address;                  // MAIL FROM 解析出的地址
    std::vector<std::string> recipient_addresses;// RCPT TO 收件人列表
    std::string mail_data;                       // 旧逻辑保留：完整邮件字符串缓冲（非流式时）
    std::string parsed_subject;                  // 从头部解析出的Subject
    std::string source_message_id;               // 从头部解析出的Message-ID

    // streaming / MIME parsing helpers
    bool header_parsed = false;                  // DATA 阶段是否已解析完邮件头
    bool streaming_enabled = false;              // 是否启用流式写盘（检测到大体或 multipart 后）
    bool multipart = false;                      // 邮件是否为 multipart/*
    bool has_attachment = false;                 // 是否检测到 Content-Disposition: attachment
    std::string boundary;                        // multipart 边界字符串
    std::string header_buffer;                   // 收集头部行直到遇到空行
    std::string line_buffer;                     // 行级缓冲，处理 CRLF 与前导点去除
    std::string text_body_buffer;                // 纯文本体累积（非流式或未触发刷盘时）
    size_t text_body_size = 0;                   // 已接收正文总字节数（逻辑计数）
    size_t buffered_body_size = 0;               // 已缓存在 text_body_buffer 的字节数
    bool body_limit_exceeded = false;            // 超过大小限制时标记，后续可能拒收
    std::string abort_reason;                    // 中断原因描述，便于日志

    std::string current_part_headers;            // 当前 MIME 部分的头部缓存
    bool in_part_header = false;                 // 是否仍在当前部分头部区域
    bool current_part_is_attachment = false;     // 当前部分是否为附件
    std::string current_part_encoding;           // 当前部分的 Content-Transfer-Encoding
    std::string current_part_mime;               // 当前部分的 Content-Type
    std::string current_attachment_filename;     // 当前附件文件名（来自 MIME 头）
    std::string current_attachment_path;         // 当前附件落盘路径
    std::ofstream current_attachment_stream;     // 备用流句柄（现以缓冲方式为主）
    std::string base64_remainder;                // Base64 断行余数，拼接下一块使用
    size_t current_attachment_size = 0;          // 当前附件累计写入大小
    std::vector<attachment> streamed_attachments;// 已完成的附件元数据，DATA_END 时搬运到 mail

    // 入站验证相关
    std::string ehlo_domain;                   // EHLO/HELO 客户端声明的域名
    bool is_trusted_server = false;            // EHLO 验证通过（PTR 匹配），auto 模式跳过 AUTH
    std::string auth_results_header;           // 验证后注入的 Authentication-Results 头
    bool verification_run = false;             // 本次事务是否已执行验证
    bool spf_checked = false;                  // SPF 已在 MAIL FROM 阶段验证
    std::string spf_result;                    // MAIL FROM 阶段的 SPF 结果
    std::string spf_reason;                    // SPF 失败原因
    int shard_index = 0;                       // 由 shard router 在认证时分配

    // 附件缓冲区相关（采用与邮件相同的缓冲策略）
    size_t attachment_buffer_size = 0;           // 当前附件缓冲区大小
    std::unique_ptr<char[]> attachment_buffer;   // 附件数据缓冲指针
    size_t attachment_buffer_used = 0;           // 附件缓冲已使用字节数
    size_t attachment_buffer_expand_count = 0;   // 附件缓冲扩容次数，超过阈值转刷盘

    void clear() {
        is_authenticated = false;
        client_username.clear();
        sender_address.clear();
        recipient_addresses.clear();
        mail_data.clear();
        parsed_subject.clear();
        source_message_id.clear();
        header_parsed = false;
        streaming_enabled = false;
        multipart = false;
        has_attachment = false;
        text_body_size = 0;
        buffered_body_size = 0;
        body_limit_exceeded = false;
        abort_reason.clear();
        boundary.clear();
        header_buffer.clear();
        line_buffer.clear();
        text_body_buffer.clear();
        current_part_headers.clear();
        in_part_header = false;
        current_part_is_attachment = false;
        current_part_encoding.clear();
        current_part_mime.clear();
        current_attachment_filename.clear();
        current_attachment_path.clear();
        base64_remainder.clear();
        current_attachment_size = 0;
        streamed_attachments.clear();
        attachment_buffer_used = 0;
        attachment_buffer_expand_count = 0;
        ehlo_domain.clear();
        is_trusted_server = false;
        auth_results_header.clear();
        verification_run = false;
        spf_checked = false;
        spf_result.clear();
        spf_reason.clear();
        shard_index = 0;
        if (current_attachment_stream.is_open()) {
            current_attachment_stream.close();
        }
    }
};

// 状态处理函数类型定义
template <typename ConnectionType>
using StateHandler = std::function<void(std::shared_ptr<SessionBase<ConnectionType>>)>;
template <typename ConnectionType>
using SessionHandler = std::function<void(std::shared_ptr<SessionBase<ConnectionType>>)>;

// SMTPS状态机接口
template <typename ConnectionType>
class SmtpsFsm {
protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<IMailboxCache> m_mailboxCache;
    std::shared_ptr<mail_system::persist_storage::PersistentQueue> m_persistentQueue;
public:
    SmtpsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
             std::shared_ptr<ThreadPoolBase> worker_thread_pool,
             std::shared_ptr<persist_storage::PersistentQueue> persistent_queue,
             std::shared_ptr<router::IShardRouter> shard_router)
        : m_ioThreadPool(io_thread_pool),
          m_workerThreadPool(worker_thread_pool),
          m_shardRouter(std::move(shard_router)),
          m_persistentQueue(persistent_queue) {}
    virtual ~SmtpsFsm() = default;

    void set_mailbox_cache(std::shared_ptr<IMailboxCache> cache) { m_mailboxCache = cache; }
    std::shared_ptr<IMailboxCache> get_mailbox_cache() const { return m_mailboxCache; }

    ScopedConnection acquire_connection(int shard) {
        auto pool = m_shardRouter->get_db_pool(static_cast<size_t>(shard));
        return pool->acquire_connection();
    }

    // 处理事件
    virtual void process_event(std::shared_ptr<SessionBase<ConnectionType>> session, SmtpsEvent event) = 0;

    // 获取状态名称
    static std::string get_state_name(SmtpsState state) {
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

    // 获取事件名称
    static std::string get_event_name(SmtpsEvent event) {
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

    std::shared_ptr<AuthCache> m_authCache = std::make_shared<AuthCache>();

    bool auth_user(SessionBase<ConnectionType>* session, const std::string& mail_address,
                   const std::string& password, int& out_shard) {
        if (!session) { LOG_AUTH_ERROR("Session is null in auth_user"); return false; }

        // shard router
        int shard = 0;
        if (m_shardRouter) { int r = m_shardRouter->route(mail_address); if (r >= 0) shard = r; }
        out_shard = shard;

        // 查缓存
        AuthCacheEntry ce;
        if (m_authCache->lookup(mail_address, ce)) {
            if (ce.status != 1) return false;
            out_shard = ce.shard;
            if (ce.password_hash.size() >= 2 && ce.password_hash[0] == '$' && ce.password_hash[1] == '2')
                return bcrypt_verify(password, ce.password_hash);
            return ce.password_hash == password;
        }

        // 缓存未命中，查 DB → 写入缓存
        auto conn = acquire_connection(shard);
        if (!conn.is_valid()) { LOG_AUTH_ERROR("Failed to get DB connection"); return false; }

        std::string sql = db::sql::build_auth_user_query();
        auto result = conn->query(sql, {mail_address});
        if (!result || result->get_row_count() == 0) { LOG_AUTH_WARN("User not found: {}", mail_address); return false; }
        int status = std::stoi(result->get_value(0, "status"));
        if (status != 1) { LOG_AUTH_WARN("User account disabled: {}", mail_address); return false; }
        std::string stored = result->get_value(0, "password");
        m_authCache->store(mail_address, {stored, status, 0, shard});
        bool ok = (stored.size() >= 2 && stored[0] == '$' && stored[1] == '2')
                        ? bcrypt_verify(password, stored)
                        : (stored == password);
        if (ok) conn->execute(db::sql::build_update_last_login(), {mail_address});
        return ok;
    }

    void get_mail_data(SessionBase<ConnectionType>* session, std::string& mail_data) {
        if (!session) {
            LOG_AUTH_ERROR("Session is null in get_mail_data");
            return;
        }

        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            LOG_AUTH_ERROR("Failed to get database connection in get_mail_data");
            return;
        }

        // 从session上下文获取发件人地址（如果已设置）
        std::string sender = session->context_.sender_address.empty() ? session->context_.client_username : session->context_.sender_address;

        // 使用参数化查询
        std::string sql = "SELECT subject, body FROM mails WHERE sender = ? ORDER BY send_time DESC LIMIT 1";
        auto result = conn->query(sql, {sender});
        if (result && result->get_row_count() > 0) {
            std::string subject = result->get_value(0, "subject");
            std::string body = result->get_value(0, "body");
            mail_data = "Subject: " + subject + "\n\n" + body;
        }
    }

    void get_mail_data(std::shared_ptr<SessionBase<ConnectionType>> session, std::string& mail_data) {
        get_mail_data(session.get(), mail_data);
    }

    // 保存邮件元数据到数据库（异步），返回future用于跟踪操作结果
    std::future<bool> save_mail_metadata_async(mail* data, const std::string& file_path_prefix) {
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
            if (!conn_ptr->execute(mail_sql)) {
                LOG_DATABASE_ERROR("Failed to insert mail metadata. Error: {}", conn_ptr->get_last_error());
                return false;
            }

            // 第二步：插入邮件收发件人关系到 mail_recipients 表
            std::string recipient_sql = db::sql::build_insert_recipients_simple(
                mail_data, conn_ptr);
            LOG_DATABASE_DEBUG("Executing SQL: {}", recipient_sql);
            if (!recipient_sql.empty() && !conn_ptr->execute(recipient_sql)) {
                LOG_DATABASE_ERROR("Failed to insert mail recipients. Error: {}", conn_ptr->get_last_error());
                // 如果插入收件人失败，删除已插入的邮件元数据
                conn_ptr->execute(db::sql::build_delete_mail_by_id(mail_data.id));
                return false;
            }

            // 第三步：插入附件元数据
            if (!mail_data.attachments.empty()) {
                std::string att_sql = db::sql::build_insert_attachments(
                    mail_data.id, mail_data.attachments, conn_ptr);
                LOG_DATABASE_DEBUG("Executing SQL: {}", att_sql);
                if (!conn_ptr->execute(att_sql)) {
                    LOG_DATABASE_ERROR("Failed to insert attachment metadata. Error: {}", conn_ptr->get_last_error());
                    success = false;
                }
            }

            return success;
        };

        future = this->m_workerThreadPool->submit(std::move(task));

        return future;
    }

    // 保存附件元数据到数据库（异步），返回 future
    std::future<bool> save_attachment_metadata_async(const attachment& att, size_t mail_id) {
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
            if (!conn_ptr->execute(att_sql)) {
                LOG_DATABASE_ERROR("Failed to insert attachment metadata. Error: {}", conn_ptr->get_last_error());
                return false;
            }
            return true;
        };

        return this->m_workerThreadPool->submit(std::move(task));
    }

    // 根据文件路径删除邮件元数据 假定数据库操作一定成功
    void remove_metadata_by_file_path(const std::vector<std::string>& file_paths) {
        auto conn = acquire_connection(0);
        if (!conn.is_valid()) {
            LOG_DATABASE_ERROR("Failed to get database connection in remove_metadata_by_file_path");
            return;
        }

        // 注意：数据库中 body_path 字段存储的是文件路径
        std::string sql = "DELETE FROM mails WHERE body_path = ?";
        for (const auto& file_path : file_paths) {
            if (!conn->execute(sql, {file_path})) {
                LOG_DATABASE_ERROR("Failed to delete mail metadata for file path: {}", file_path);
            }
        }
    }

    // 保存邮件正文到本地文件
    bool save_mail_body_to_file(mail* data, const std::string& file_path) {
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

    bool save_attachment_to_file(attachment& att, const std::string& file_path) {
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

    // 检查异步操作结果，失败则删除对应文件
    void cleanup_failed_saves(std::vector<std::future<bool>>& futures, const std::vector<std::string>& file_paths) {
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
};

} // namespace mail_system

#endif // SMTPS_FSM_H