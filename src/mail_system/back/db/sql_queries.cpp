#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/mailServer/outbound/outbox_repository.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace mail_system {
namespace db {
namespace sql {

namespace {
    std::string extract_domain_lower(const std::string& email) {
        const auto at_pos = email.find('@');
        if (at_pos == std::string::npos || at_pos + 1 >= email.size()) return {};
        std::string domain = email.substr(at_pos + 1);
        std::transform(domain.begin(), domain.end(), domain.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return domain;
    }
}

// ============================================================
// 邮件持久化
// ============================================================

std::string clip_subject_for_db(const std::string& subject) {
    // 预算按 escape_string 后的字符数计：mysql_real_escape_string 会把
    // ' " \ NUL \r \n ^Z 各变成 2 字符，其余（含多字节 UTF-8）按 1 计。
    // 逐字符扣预算保证拼接后的 SQL 里 subject 段不超 kMaxSubjectChars，
    // 并在不完整的多字节序列边界停住，避免截出半个字符。
    std::size_t budget = kMaxSubjectChars;
    std::size_t end = 0;
    while (end < subject.size() && budget > 0) {
        const unsigned char c = static_cast<unsigned char>(subject[end]);
        std::size_t seq_len = 1;
        if ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        if (end + seq_len > subject.size()) break;  // 尾部不完整序列
        const std::size_t cost =
            (c == '\'' || c == '"' || c == '\\' || c == 0 || c == '\n' || c == '\r' || c == 0x1A) ? 2 : 1;
        if (cost > budget) break;
        budget -= cost;
        end += seq_len;
    }
    if (end == subject.size()) return subject;
    return subject.substr(0, end);
}

std::string build_insert_mail(std::uint64_t mail_id,
                               const std::string& subject,
                               const std::string& body_path,
                               std::time_t send_time,
                               IDBConnection* conn) {
    return "INSERT INTO mails (id, subject, body_path, send_time) VALUES (" +
           std::to_string(mail_id) + ", '" +
           (conn ? conn->escape_string(clip_subject_for_db(subject)) : clip_subject_for_db(subject)) + "', '" +
           (conn ? conn->escape_string(body_path) : body_path) + "', FROM_UNIXTIME(" +
           std::to_string(static_cast<long long>(send_time)) + "))";
}

// 简化版：只插 id/subject/body_path/status（FSM DATA_END 路径使用）
std::string build_insert_mail_with_status(std::uint64_t mail_id,
                                           const std::string& subject,
                                           const std::string& body_path,
                                           int status,
                                           IDBConnection* conn) {
    return "INSERT INTO mails (id, subject, body_path, status) VALUES (" +
           std::to_string(mail_id) + ", '" +
           (conn ? conn->escape_string(clip_subject_for_db(subject)) : clip_subject_for_db(subject)) + "', '" +
           (conn ? conn->escape_string(body_path) : body_path) + "', " +
           std::to_string(status) + ")";
}

std::string build_insert_recipients(const mail& mail_data,
                                     const std::string& local_domain,
                                     IDBConnection* conn) {
    const std::string source_message_id = conn ? conn->escape_string(mail_data.source_message_id) : mail_data.source_message_id;
    const std::string escaped_sender = conn ? conn->escape_string(mail_data.from) : mail_data.from;

    auto local_lower = local_domain;
    std::transform(local_lower.begin(), local_lower.end(), local_lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::string sql = "INSERT INTO mail_recipients (id, mail_id, sender, recipient, status, source_message_id) VALUES";
    for (size_t i = 0; i < mail_data.to.size(); ++i) {
        const auto domain = extract_domain_lower(mail_data.to[i]);
        const int status = (!domain.empty() && domain == local_lower) ? 1 : 2;
        const std::string escaped_rcpt = conn ? conn->escape_string(mail_data.to[i]) : mail_data.to[i];

        sql += " (" + std::to_string(mail_data.ids[i]) + ", " +
               std::to_string(mail_data.id) + ", '" +
               escaped_sender + "', '" +
               escaped_rcpt + "', " +
               std::to_string(status) + ", '" +
               source_message_id + "'),";
    }
    sql.back() = ';';
    return sql;
}

std::string build_insert_recipient_single(std::uint64_t mail_id,
                                           std::uint64_t recipient_row_id,
                                           const std::string& sender,
                                           const std::string& recipient,
                                           int status,
                                           const std::string& source_message_id,
                                           IDBConnection* conn) {
    return "INSERT INTO mail_recipients (id, mail_id, sender, recipient, status, source_message_id) VALUES (" +
           std::to_string(recipient_row_id) + ", " +
           std::to_string(mail_id) + ", '" +
           (conn ? conn->escape_string(sender) : sender) + "', '" +
           (conn ? conn->escape_string(recipient) : recipient) + "', " +
           std::to_string(status) + ", '" +
           (conn ? conn->escape_string(source_message_id) : source_message_id) + "')";
}

std::string build_insert_mailbox_for_recipient(std::uint64_t mail_id,
                                                const std::string& recipient_address,
                                                IDBConnection* conn) {
    return "INSERT INTO mail_mailbox (mail_id, mailbox_id, user_id) "
           "SELECT " + std::to_string(mail_id) + ", mb.id, u.id "
           "FROM users u "
           "JOIN mailboxes mb ON mb.user_id = u.id AND mb.box_type = 1 "
           "WHERE u.mail_address = '" +
           (conn ? conn->escape_string(recipient_address) : recipient_address) + "'";
}

std::string build_insert_attachments(std::uint64_t mail_id,
                                      const std::vector<attachment>& attachments,
                                      IDBConnection* conn) {
    if (attachments.empty()) return {};
    std::string sql = "INSERT INTO attachments (mail_id, filename, filepath, file_size, mime_type) VALUES";
    for (const auto& att : attachments) {
        sql += " (" + std::to_string(mail_id) + ", '" +
               (conn ? conn->escape_string(att.filename) : att.filename) + "', '" +
               (conn ? conn->escape_string(att.filepath) : att.filepath) + "', " +
               std::to_string(att.file_size) + ", '" +
               (conn ? conn->escape_string(att.mime_type) : att.mime_type) + "'),";
    }
    sql.back() = ';';
    return sql;
}

// ============================================================
// 出站队列
// ============================================================

std::string build_insert_outbox_pending(std::uint64_t mail_id,
                                         const std::string& sender,
                                         const std::string& recipient,
                                         int max_attempts,
                                         IDBConnection* conn) {
    std::ostringstream s;
    s << "INSERT INTO mail_outbox (mail_id, sender, recipient, status, priority, "
      << "attempt_count, max_attempts, next_attempt_at) VALUES ("
      << mail_id << ", '"
      << (conn ? conn->escape_string(sender) : sender) << "', '"
      << (conn ? conn->escape_string(recipient) : recipient) << "', "
      << static_cast<int>(outbound::OutboxStatus::PENDING) << ", 0, 0, "
      << max_attempts << ", NOW())";
    return s.str();
}

std::string build_insert_outbox_reserved(std::uint64_t mail_id,
                                          const std::string& sender,
                                          const std::string& recipient,
                                          const std::string& lease_owner,
                                          int lease_seconds,
                                          IDBConnection* conn) {
    std::ostringstream s;
    s << "INSERT INTO mail_outbox (mail_id, sender, recipient, status, priority, "
      << "attempt_count, max_attempts, next_attempt_at, lease_owner, lease_until) VALUES ("
      << mail_id << ", '"
      << (conn ? conn->escape_string(sender) : sender) << "', '"
      << (conn ? conn->escape_string(recipient) : recipient) << "', "
      << static_cast<int>(outbound::OutboxStatus::SENDING) << ", 0, 1, 8, NOW(), '"
      << (conn ? conn->escape_string(lease_owner) : lease_owner) << "', "
      << "DATE_ADD(NOW(), INTERVAL " << std::max(1, lease_seconds) << " SECOND))";
    return s.str();
}

std::string build_outbox_claim_select(int limit) {
    std::ostringstream s;
    s << "SELECT o.id, o.mail_id, o.sender, o.recipient, o.attempt_count, o.max_attempts, m.body_path "
      << "FROM mail_outbox o LEFT JOIN mails m ON m.id = o.mail_id "
      << "WHERE (status = " << static_cast<int>(outbound::OutboxStatus::PENDING)
      << " OR status = " << static_cast<int>(outbound::OutboxStatus::RETRY) << ") "
      << "AND next_attempt_at <= NOW() "
      << "AND (lease_until IS NULL OR lease_until <= NOW()) "
      << "ORDER BY o.priority DESC, o.id ASC LIMIT " << limit;
    return s.str();
}

std::string build_outbox_claim_update(std::uint64_t outbox_id,
                                       const std::string& worker_id,
                                       int lease_seconds,
                                       IDBConnection* conn) {
    std::ostringstream s;
    s << "UPDATE mail_outbox SET status = " << static_cast<int>(outbound::OutboxStatus::SENDING)
      << ", lease_owner = '" << (conn ? conn->escape_string(worker_id) : worker_id) << "'"
      << ", lease_until = DATE_ADD(NOW(), INTERVAL " << lease_seconds << " SECOND)"
      << ", attempt_count = attempt_count + 1, updated_at = NOW()"
      << " WHERE id = " << outbox_id
      << " AND (status = " << static_cast<int>(outbound::OutboxStatus::PENDING)
      << " OR status = " << static_cast<int>(outbound::OutboxStatus::RETRY) << ")"
      << " AND (lease_until IS NULL OR lease_until <= NOW())";
    return s.str();
}

std::string build_outbox_release_reservations(const std::vector<std::uint64_t>& outbox_ids) {
    if (outbox_ids.empty()) return {};
    std::ostringstream id_list;
    for (size_t i = 0; i < outbox_ids.size(); ++i) {
        id_list << outbox_ids[i];
        if (i + 1 < outbox_ids.size()) id_list << ",";
    }
    std::ostringstream s;
    s << "UPDATE mail_outbox SET status = " << static_cast<int>(outbound::OutboxStatus::PENDING)
      << ", attempt_count = GREATEST(attempt_count - 1, 0)"
      << ", lease_owner = NULL, lease_until = NULL, updated_at = NOW()"
      << " WHERE id IN (" << id_list.str() << ")"
      << " AND status = " << static_cast<int>(outbound::OutboxStatus::SENDING);
    return s.str();
}

std::string build_outbox_mark_sent(std::uint64_t outbox_id,
                                    const std::string& smtp_response,
                                    IDBConnection* conn) {
    std::ostringstream s;
    s << "UPDATE mail_outbox SET status = " << static_cast<int>(outbound::OutboxStatus::SENT)
      << ", sent_at = NOW(), smtp_response = '"
      << (conn ? conn->escape_string(smtp_response) : smtp_response) << "'"
      << ", lease_owner = NULL, lease_until = NULL, updated_at = NOW()"
      << " WHERE id = " << outbox_id;
    return s.str();
}

std::string build_outbox_select_attempts(std::uint64_t outbox_id) {
    return "SELECT attempt_count, max_attempts FROM mail_outbox WHERE id = " + std::to_string(outbox_id);
}

std::string build_outbox_mark_retry_or_dead(std::uint64_t outbox_id,
                                             int status,
                                             const std::string& error_message,
                                             int retry_delay_seconds,
                                             IDBConnection* conn) {
    std::ostringstream s;
    s << "UPDATE mail_outbox SET status = " << status
      << ", last_error_message = '"
      << (conn ? conn->escape_string(error_message) : error_message) << "'"
      << ", lease_owner = NULL, lease_until = NULL"
      << ", next_attempt_at = DATE_ADD(NOW(), INTERVAL " << std::max(1, retry_delay_seconds) << " SECOND)"
      << ", updated_at = NOW()"
      << " WHERE id = " << outbox_id;
    return s.str();
}

std::string build_outbox_mark_dead(std::uint64_t outbox_id,
                                    const std::string& error_message,
                                    IDBConnection* conn) {
    std::ostringstream s;
    s << "UPDATE mail_outbox SET status = " << static_cast<int>(outbound::OutboxStatus::DEAD)
      << ", last_error_message = '"
      << (conn ? conn->escape_string(error_message) : error_message) << "'"
      << ", lease_owner = NULL, lease_until = NULL, updated_at = NOW()"
      << " WHERE id = " << outbox_id;
    return s.str();
}

std::string build_outbox_requeue_expired_leases() {
    std::ostringstream s;
    s << "UPDATE mail_outbox SET status = " << static_cast<int>(outbound::OutboxStatus::RETRY)
      << ", lease_owner = NULL, lease_until = NULL"
      << ", next_attempt_at = NOW(), updated_at = NOW()"
      << " WHERE status = " << static_cast<int>(outbound::OutboxStatus::SENDING)
      << " AND lease_until IS NOT NULL AND lease_until <= NOW()";
    return s.str();
}

// ============================================================
// 邮件加载
// ============================================================

std::string build_load_mail_metadata(std::uint64_t mail_id) {
    return "SELECT id, subject, body_path, UNIX_TIMESTAMP(send_time) AS send_time_epoch "
           "FROM mails WHERE id = " + std::to_string(mail_id) + " LIMIT 1";
}

std::string build_load_mail_recipients(std::uint64_t mail_id) {
    return "SELECT id, sender, recipient, status, source_message_id, UNIX_TIMESTAMP(send_time) AS send_time_epoch "
           "FROM mail_recipients WHERE mail_id = " + std::to_string(mail_id) + " ORDER BY id ASC";
}

std::string build_load_mail_attachments(std::uint64_t mail_id) {
    return "SELECT id, filename, filepath, file_size, mime_type, UNIX_TIMESTAMP(upload_time) AS upload_time_epoch "
           "FROM attachments WHERE mail_id = " + std::to_string(mail_id) + " ORDER BY id ASC";
}

// ============================================================
// 入站去重
// ============================================================

std::string build_dedup_by_subject_sender(const std::string& subject,
                                           const std::string& sender,
                                           int window_seconds,
                                           IDBConnection* conn) {
    // 库里存的是截断后的主题（clip_subject_for_db），去重比较须用同一截断结果
    return "SELECT id FROM mails WHERE subject = '" +
           (conn ? conn->escape_string(clip_subject_for_db(subject)) : clip_subject_for_db(subject)) + "' "
           "AND id IN (SELECT mail_id FROM mail_recipients WHERE sender = '" +
           (conn ? conn->escape_string(sender) : sender) + "') "
           "AND TIMESTAMPDIFF(SECOND, send_time, NOW()) BETWEEN 0 AND " +
           std::to_string(window_seconds) + " LIMIT 20";
}

std::string build_dedup_by_subject_sender_recipient(const std::string& subject,
                                                     const std::string& sender,
                                                     const std::string& recipient,
                                                     int window_seconds,
                                                     IDBConnection* conn) {
    return "SELECT 1 FROM mail_recipients r "
           "JOIN mails m ON m.id = r.mail_id "
           "WHERE r.sender='" + (conn ? conn->escape_string(sender) : sender) + "' "
           "AND r.recipient='" + (conn ? conn->escape_string(recipient) : recipient) + "' "
           "AND m.subject='" + (conn ? conn->escape_string(clip_subject_for_db(subject)) : clip_subject_for_db(subject)) + "' "
           "AND TIMESTAMPDIFF(SECOND, m.send_time, NOW()) BETWEEN 0 AND " +
           std::to_string(window_seconds) + " LIMIT 1";
}

std::string build_dedup_by_message_id(const std::string& sender,
                                       const std::string& recipient,
                                       const std::string& message_id,
                                       IDBConnection* conn) {
    return "SELECT 1 FROM mail_recipients WHERE sender='" +
           (conn ? conn->escape_string(sender) : sender) + "' "
           "AND recipient='" + (conn ? conn->escape_string(recipient) : recipient) + "' "
           "AND source_message_id='" +
           (conn ? conn->escape_string(message_id) : message_id) + "' LIMIT 1";
}

// ============================================================
// 认证
// ============================================================

std::string build_auth_user_query() {
    return "SELECT id, password, status FROM users WHERE mail_address = ?";
}

std::string build_update_last_login() {
    return "UPDATE users SET last_login_time = NOW() WHERE mail_address = ?";
}

std::string build_recipient_exists_query() {
    return "SELECT id, status FROM users WHERE mail_address = ? LIMIT 1";
}

// 每日发信配额：原子占用今日一个配额（跨天自动归零；sent_date NULL=首封按 0 处理）。
// 条件 WHERE 卡住超限（affected rows = 0）；调用方用 SELECT ROW_COUNT() 读结果。
std::string build_increment_sent_today_query() {
    return "UPDATE users "
           "SET sent_today = IF(sent_date = CURRENT_DATE, sent_today, 0) + 1, "
           "    sent_date  = CURRENT_DATE "
           "WHERE mail_address = ? "
           "  AND IF(sent_date = CURRENT_DATE, sent_today, 0) < ?";
}

// ============================================================
// IMAP
// ============================================================

std::string build_imap_list_mailboxes() {
    return "SELECT id, name, box_type FROM mailboxes WHERE user_id = ? ORDER BY id";
}

std::string build_imap_get_mailbox_by_name() {
    return "SELECT id, name, box_type FROM mailboxes WHERE user_id = ? AND name = ?";
}

std::string build_imap_get_inbox_id() {
    return "SELECT id FROM mailboxes WHERE user_id = ? AND box_type = 1";
}

std::string build_imap_get_mailbox_mails() {
    return "SELECT m.id, COALESCE(mr.sender,'') as sender, COALESCE(mr.recipient,'') as recipient, "
           "m.subject, m.body_path, "
           "mm.is_starred, mm.is_deleted, mm.is_important, "
           "COALESCE(mr.status,0) as status, UNIX_TIMESTAMP(m.send_time) AS send_time "
           "FROM mail_mailbox mm "
           "JOIN mails m ON mm.mail_id = m.id "
           "LEFT JOIN mail_recipients mr ON mr.mail_id = m.id "
           "WHERE mm.mailbox_id = ? AND mm.user_id = ? "
           "ORDER BY m.send_time DESC";
}

std::string build_imap_mailbox_exists_count() {
    return "SELECT COUNT(*) as cnt FROM mail_mailbox WHERE mailbox_id = ? AND user_id = ?";
}

std::string build_imap_mailbox_unseen_count() {
    return "SELECT COUNT(*) as cnt FROM mail_mailbox mm "
           "JOIN mail_recipients mr ON mr.mail_id = mm.mail_id AND mr.recipient = ("
           "  SELECT mail_address FROM users WHERE id = ?"
           ") "
           "WHERE mm.mailbox_id = ? AND mm.user_id = ? AND mr.status = 1";
}

std::string build_imap_uidnext_advance() {
    // 原子推进 per-mailbox UIDNEXT 高水位（RFC 3501 §2.3.1.1）：
    //   - 行锁串行化并发读者 → 跨实例安全，不再各自 MAX+1 撞值
    //   - GREATEST 保证 expunge 掉最大 mail_id 后不回退
    // 空邮箱时聚合查询仍返回一行 uidnext=1（INSERT...SELECT 兜底建行）。
    return "INSERT INTO mailbox_uidnext (mailbox_id, uidnext) "
           "SELECT ?, COALESCE(MAX(mm.mail_id), 0) + 1 "
           "FROM mail_mailbox mm WHERE mm.mailbox_id = ? "
           "ON DUPLICATE KEY UPDATE "
           "  uidnext = GREATEST(uidnext, "
           "    (SELECT COALESCE(MAX(mm.mail_id), 0) + 1 FROM mail_mailbox mm WHERE mm.mailbox_id = ?))";
}

std::string build_imap_uidnext_read() {
    return "SELECT uidnext FROM mailbox_uidnext WHERE mailbox_id = ?";
}

std::string build_imap_update_mail_flag_deleted() {
    return "UPDATE mail_mailbox SET is_deleted = ? WHERE mail_id = ? AND user_id = ? AND mailbox_id = ?";
}

std::string build_imap_update_mail_flag_starred() {
    return "UPDATE mail_mailbox SET is_starred = ? WHERE mail_id = ? AND user_id = ? AND mailbox_id = ?";
}

std::string build_imap_append_mail_metadata() {
    return "INSERT INTO mails (id, subject, body_path, send_time) VALUES (?, ?, ?, ?)";
}

std::string build_imap_append_mail_recipient() {
    return "INSERT INTO mail_recipients (id, mail_id, sender, recipient, status) VALUES (?, ?, ?, ?, ?)";
}

std::string build_imap_append_mailbox() {
    return "INSERT INTO mail_mailbox (id, mail_id, mailbox_id, user_id, is_starred, "
           "is_important, is_deleted, add_time) VALUES (?, ?, ?, ?, 0, 0, 0, NOW())";
}

std::string build_imap_select_status_total() {
    return "SELECT COUNT(*) as cnt FROM mail_mailbox WHERE mailbox_id = ? AND user_id = ?";
}

std::string build_imap_select_status_recent() {
    return "SELECT COUNT(*) as cnt FROM mail_mailbox mm "
           "JOIN mail_recipients mr ON mr.mail_id = mm.mail_id AND mr.recipient = ("
           "  SELECT mail_address FROM users WHERE id = ?"
           ") "
           "WHERE mm.mailbox_id = ? AND mm.user_id = ? AND mr.status = 1";
}

std::string build_imap_expunge_delete_mailbox() {
    return "DELETE FROM mail_mailbox WHERE mailbox_id = ? AND user_id = ? AND is_deleted = 1";
}

std::string build_imap_expunge_select_ids() {
    return "SELECT mail_id FROM mail_mailbox WHERE mailbox_id = ? AND user_id = ? AND is_deleted = 1";
}

std::string build_imap_create_mailbox() {
    return "INSERT INTO mailboxes (user_id, name, is_system, box_type) VALUES (?, ?, 0, 0)";
}

std::string build_imap_rename_mailbox() {
    return "UPDATE mailboxes SET name = ? WHERE id = ?";
}

std::string build_imap_delete_mailbox_messages() {
    return "DELETE FROM mail_mailbox WHERE mailbox_id = ?";
}

std::string build_imap_check_mailbox_is_system() {
    return "SELECT is_system FROM mailboxes WHERE id = ?";
}

std::string build_imap_delete_mailbox() {
    return "DELETE FROM mailboxes WHERE id = ?";
}

std::string build_imap_copy_check_exists() {
    return "SELECT id FROM mail_mailbox WHERE mail_id = ? AND mailbox_id = ? AND user_id = ?";
}

std::string build_imap_copy_insert_mailbox() {
    return "INSERT INTO mail_mailbox (id, mail_id, mailbox_id, user_id, is_starred, "
           "is_important, is_deleted, add_time) VALUES (?, ?, ?, ?, 0, 0, 0, NOW())";
}

// ============================================================
// 分片路由
// ============================================================

std::string build_shard_lookup(const std::string& table_name,
                                const std::string& email_column,
                                const std::string& shard_column,
                                const std::string& escaped_email) {
    return "SELECT " + shard_column + " FROM " + table_name +
           " WHERE " + email_column + " = '" + escaped_email + "'";
}

// ============================================================
// 清理
// ============================================================

std::string build_delete_attachments_by_mail(std::uint64_t mail_id) {
    return "DELETE FROM attachments WHERE mail_id = " + std::to_string(mail_id);
}

std::string build_delete_recipients_by_mail(std::uint64_t mail_id) {
    return "DELETE FROM mail_recipients WHERE mail_id = " + std::to_string(mail_id);
}

std::string build_delete_mail_by_id(std::uint64_t mail_id) {
    return "DELETE FROM mails WHERE id = " + std::to_string(mail_id);
}

std::string build_delete_mail_recipients_by_id_list(const std::string& id_list) {
    return "DELETE FROM mail_recipients WHERE mail_id IN (" + id_list + ")";
}

std::string build_delete_mails_by_id_list(const std::string& id_list) {
    return "DELETE FROM mails WHERE id IN (" + id_list + ")";
}

// ============================================================
// 工具
// ============================================================

std::string build_select_last_insert_id() {
    return "SELECT LAST_INSERT_ID() AS id";
}

std::string build_select_row_count() {
    return "SELECT ROW_COUNT() AS affected";
}

// 简化版 recipients 插入（FSM DATA_END 路径使用，无 id/status 列）
std::string build_insert_recipients_simple(const mail& mail_data, IDBConnection* conn) {
    if (mail_data.to.empty()) return {};
    std::string sql = "INSERT INTO mail_recipients (mail_id, sender, recipient, source_message_id) VALUES";
    for (size_t i = 0; i < mail_data.to.size(); ++i) {
        sql += " (" + std::to_string(mail_data.id) + ", '" +
               (conn ? conn->escape_string(mail_data.from) : mail_data.from) + "', '" +
               (conn ? conn->escape_string(mail_data.to[i]) : mail_data.to[i]) + "', '" +
               (conn ? conn->escape_string(mail_data.source_message_id) : mail_data.source_message_id) + "')";
        if (i < mail_data.to.size() - 1) sql += ", ";
    }
    return sql + ";";
}

std::string build_delete_mail_by_body_path() {
    return "DELETE FROM mails WHERE body_path = ?";
}

std::string build_insert_attachment_single(std::uint64_t mail_id,
                                            const attachment& att,
                                            IDBConnection* conn) {
    return "INSERT INTO attachments (mail_id, filename, filepath, file_size, mime_type, upload_time) VALUES (" +
           std::to_string(mail_id) + ", '" +
           (conn ? conn->escape_string(att.filename) : att.filename) + "', '" +
           (conn ? conn->escape_string(att.filepath) : att.filepath) + "', " +
           std::to_string(att.file_size) + ", '" +
           (conn ? conn->escape_string(att.mime_type) : att.mime_type) + "', " +
           std::to_string(att.upload_time) + ")";
}

} // namespace sql
} // namespace db
} // namespace mail_system
