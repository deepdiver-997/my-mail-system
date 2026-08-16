#ifndef SQL_QUERIES_H
#define SQL_QUERIES_H

#include "mail_system/back/db/db_service.h"
#include "mail_system/back/entities/mail.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mail_system {
namespace db {
namespace sql {

// ============================================================
// 邮件持久化 (mails / mail_recipients / mail_mailbox)
// ============================================================

// 插入邮件元数据 (完整版: 含 send_time)
std::string build_insert_mail(std::uint64_t mail_id,
                               const std::string& subject,
                               const std::string& body_path,
                               std::time_t send_time,
                               IDBConnection* conn);

// 插入邮件元数据 (简化版: 含 status，用于 FSM DATA_END 路径)
std::string build_insert_mail_with_status(std::uint64_t mail_id,
                                           const std::string& subject,
                                           const std::string& body_path,
                                           int status,
                                           IDBConnection* conn);

// 插入收件人关系（批量 VALUES）
std::string build_insert_recipients(const mail& mail_data,
                                     const std::string& local_domain,
                                     IDBConnection* conn);

// 单个收件人的 mail_recipients 插入
std::string build_insert_recipient_single(std::uint64_t mail_id,
                                           std::uint64_t recipient_row_id,
                                           const std::string& sender,
                                           const std::string& recipient,
                                           int status,
                                           const std::string& source_message_id,
                                           IDBConnection* conn);

// 插入 mail_mailbox（关联到收件人收件箱 box_type=1）
std::string build_insert_mailbox_for_recipient(std::uint64_t mail_id,
                                                const std::string& recipient_address,
                                                IDBConnection* conn);

// 插入附件元数据（批量 VALUES）
std::string build_insert_attachments(std::uint64_t mail_id,
                                      const std::vector<attachment>& attachments,
                                      IDBConnection* conn);

// 插入单个附件元数据（包含 upload_time）
std::string build_insert_attachment_single(std::uint64_t mail_id,
                                            const attachment& att,
                                            IDBConnection* conn);

// ============================================================
// 出站队列 (mail_outbox)
// ============================================================

// 插入出站记录（PENDING 状态）
std::string build_insert_outbox_pending(std::uint64_t mail_id,
                                         const std::string& sender,
                                         const std::string& recipient,
                                         int max_attempts,
                                         IDBConnection* conn);

// 插入出站记录（SENDING 状态，预留 lease）
std::string build_insert_outbox_reserved(std::uint64_t mail_id,
                                          const std::string& sender,
                                          const std::string& recipient,
                                          const std::string& lease_owner,
                                          int lease_seconds,
                                          IDBConnection* conn);

// 领取待投递批次
std::string build_outbox_claim_select(int limit);

// 标记单条 outbox 为 SENDING（竞争更新）
std::string build_outbox_claim_update(std::uint64_t outbox_id,
                                       const std::string& worker_id,
                                       int lease_seconds,
                                       IDBConnection* conn);

// 释放本地预留
std::string build_outbox_release_reservations(const std::vector<std::uint64_t>& outbox_ids);

// 标记已发送
std::string build_outbox_mark_sent(std::uint64_t outbox_id,
                                    const std::string& smtp_response,
                                    IDBConnection* conn);

// 查询 attempt 信息
std::string build_outbox_select_attempts(std::uint64_t outbox_id);

// 标记重试/死亡
std::string build_outbox_mark_retry_or_dead(std::uint64_t outbox_id,
                                             int status,
                                             const std::string& error_message,
                                             int retry_delay_seconds,
                                             IDBConnection* conn);

// 标记死亡
std::string build_outbox_mark_dead(std::uint64_t outbox_id,
                                    const std::string& error_message,
                                    IDBConnection* conn);

// 回收过期租约
std::string build_outbox_requeue_expired_leases();

// ============================================================
// 邮件加载（出站时从 DB 重建 mail 对象）
// ============================================================

std::string build_load_mail_metadata(std::uint64_t mail_id);
std::string build_load_mail_recipients(std::uint64_t mail_id);
std::string build_load_mail_attachments(std::uint64_t mail_id);

// ============================================================
// 入站去重
// ============================================================

// 基于主题+发送者的模糊去重
std::string build_dedup_by_subject_sender(const std::string& subject,
                                           const std::string& sender,
                                           int window_seconds,
                                           IDBConnection* conn);

// 基于主题+发送者+收件人的精确去重
std::string build_dedup_by_subject_sender_recipient(const std::string& subject,
                                                     const std::string& sender,
                                                     const std::string& recipient,
                                                     int window_seconds,
                                                     IDBConnection* conn);

// 基于 Message-ID 去重
std::string build_dedup_by_message_id(const std::string& sender,
                                       const std::string& recipient,
                                       const std::string& message_id,
                                       IDBConnection* conn);

// ============================================================
// 认证（SMTP AUTH / IMAP LOGIN）
// ============================================================

// 参数化查询模板（使用 ? 占位符）
std::string build_auth_user_query();
std::string build_update_last_login();

// RCPT TO 校验：本地收件人是否存在（未认证 MTA 入站路径）
std::string build_recipient_exists_query();

// ============================================================
// IMAP 命令
// ============================================================

std::string build_imap_list_mailboxes();
std::string build_imap_get_mailbox_by_name();
std::string build_imap_get_inbox_id();
std::string build_imap_get_mailbox_mails();
std::string build_imap_mailbox_exists_count();
std::string build_imap_mailbox_unseen_count();
std::string build_imap_mailbox_uidnext();
std::string build_imap_update_mail_flag_deleted();
std::string build_imap_update_mail_flag_starred();
std::string build_imap_append_mail_metadata();
std::string build_imap_append_mail_recipient();
std::string build_imap_append_mailbox();
std::string build_imap_select_status_total();
std::string build_imap_select_status_recent();
std::string build_imap_expunge_delete_mailbox();
std::string build_imap_expunge_select_ids();
std::string build_imap_create_mailbox();
std::string build_imap_rename_mailbox();
std::string build_imap_delete_mailbox_messages();
std::string build_imap_check_mailbox_is_system();
std::string build_imap_delete_mailbox();
std::string build_imap_copy_check_exists();
std::string build_imap_copy_insert_mailbox();

// ============================================================
// 分片路由
// ============================================================

std::string build_shard_lookup(const std::string& table_name,
                                const std::string& email_column,
                                const std::string& shard_column,
                                const std::string& escaped_email);

// ============================================================
// 清理
// ============================================================

// 简化版 recipients 插入（FSM 路径，无 id/status 列）
std::string build_insert_recipients_simple(const mail& mail_data, IDBConnection* conn);

std::string build_delete_attachments_by_mail(std::uint64_t mail_id);
std::string build_delete_recipients_by_mail(std::uint64_t mail_id);
std::string build_delete_mail_by_id(std::uint64_t mail_id);
std::string build_delete_mail_by_body_path();
std::string build_delete_mail_recipients_by_id_list(const std::string& id_list);
std::string build_delete_mails_by_id_list(const std::string& id_list);

// ============================================================
// 工具
// ============================================================

std::string build_select_last_insert_id();
std::string build_select_row_count();

} // namespace sql
} // namespace db
} // namespace mail_system

#endif // SQL_QUERIES_H
