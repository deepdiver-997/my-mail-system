#include "mail_system/back/mailServer/outbound/outbox_repository.h"

#include "mail_system/back/common/logger.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/mailServer/outbound/outbound_utils.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace mail_system {
namespace outbound {

namespace {
constexpr int kDefaultMaxAttempts = 8;
constexpr int kDeadlockRetryMaxAttempts = 3;
constexpr int kDeadlockRetryBaseDelayMs = 40;

bool is_deadlock_error(const std::string& error_message) {
    return error_message.find("Deadlock found") != std::string::npos ||
           error_message.find("errno: 1213") != std::string::npos ||
           error_message.find("ERROR 1213") != std::string::npos;
}

bool execute_with_deadlock_retry(IDBConnection* conn,
                                 const std::string& sql,
                                 const char* operation_name,
                                 std::uint64_t outbox_id) {
    if (!conn) return false;

    for (int attempt = 1; attempt <= kDeadlockRetryMaxAttempts; ++attempt) {
        if (conn->execute(sql)) return true;

        const auto error_message = conn->get_last_error();
        if (!is_deadlock_error(error_message) || attempt == kDeadlockRetryMaxAttempts)
            return false;

        const int delay_ms = kDeadlockRetryBaseDelayMs * (1 << (attempt - 1));
        LOG_OUTBOUND_WARN("OutboxRepository: deadlock on {}, outbox_id={}, retry={}/{}, delay_ms={}, error={}",
                        operation_name, outbox_id, attempt, kDeadlockRetryMaxAttempts, delay_ms, error_message);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return false;
}
}

// ---- enqueue_from_mail ----
bool OutboxRepository::enqueue_from_mail(DBPool& db,
                                         const mail& mail_data,
                                         const std::string& local_domain,
                                         std::vector<std::uint64_t>* outbox_ids) {
    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) {
        LOG_OUTBOUND_ERROR("OutboxRepository: failed to get DB connection");
        return false;
    }

    bool inserted_any = false;

    for (const auto& recipient : mail_data.to) {
        const auto recipient_domain = extract_domain(recipient);
        if (recipient_domain.empty() || recipient_domain == local_domain) continue;

        std::string sql = db::sql::build_insert_outbox_pending(mail_data.id, mail_data.from, recipient,
                                                                kDefaultMaxAttempts, conn);

        if (!conn->execute(sql)) {
            LOG_OUTBOUND_ERROR("OutboxRepository: failed to insert outbox row, mail_id={}, recipient={}, error={}",
                             mail_data.id, recipient, conn->get_last_error());
            continue;
        }

        inserted_any = true;
        if (outbox_ids) {
            auto result = conn->query(db::sql::build_select_last_insert_id());
            if (result && result->get_row_count() > 0) {
                auto row = result->get_row(0);
                auto it = row.find("id");
                if (it != row.end())
                    outbox_ids->push_back(static_cast<std::uint64_t>(std::stoull(it->second)));
            }
        }
    }
    return inserted_any;
}

// ---- claim_batch ----
std::vector<OutboxRecord> OutboxRepository::claim_batch(DBPool& db,
                                                        const std::string& worker_id,
                                                        std::size_t limit,
                                                        int lease_seconds) {
    std::vector<OutboxRecord> claimed;
    if (limit == 0) return claimed;

    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) return claimed;

    auto result = conn->query(db::sql::build_outbox_claim_select(static_cast<int>(limit)));
    if (!result || result->get_row_count() == 0) return claimed;

    const auto rows = result->get_all_rows();
    for (const auto& row : rows) {
        if (row.find("id") == row.end() || row.find("mail_id") == row.end()) continue;

        const auto outbox_id = static_cast<std::uint64_t>(std::stoull(row.at("id")));
        std::string update_sql = db::sql::build_outbox_claim_update(outbox_id, worker_id, lease_seconds, conn);

        if (!execute_with_deadlock_retry(conn, update_sql, "claim_batch", outbox_id)) continue;

        auto row_count_result = conn->query(db::sql::build_select_row_count());
        if (!row_count_result || row_count_result->get_row_count() == 0) continue;
        auto affected_row = row_count_result->get_row(0);
        if (affected_row.find("affected") == affected_row.end() ||
            std::stoi(affected_row.at("affected")) <= 0) continue;

        OutboxRecord item;
        item.id = outbox_id;
        item.mail_id = static_cast<std::uint64_t>(std::stoull(row.at("mail_id")));
        item.sender = row.count("sender") ? row.at("sender") : std::string{};
        item.recipient = row.count("recipient") ? row.at("recipient") : std::string{};
        item.attempt_count = row.count("attempt_count") ? (std::stoi(row.at("attempt_count")) + 1) : 1;
        item.max_attempts = row.count("max_attempts") ? std::stoi(row.at("max_attempts")) : kDefaultMaxAttempts;
        item.body_path = row.count("body_path") ? row.at("body_path") : std::string{};
        claimed.push_back(std::move(item));
    }
    return claimed;
}

// ---- load_mail ----
std::unique_ptr<mail> OutboxRepository::load_mail(DBPool& db, std::uint64_t mail_id) {
    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) return nullptr;

    auto mail_result = conn->query(db::sql::build_load_mail_metadata(mail_id));
    if (!mail_result || mail_result->get_row_count() == 0) return nullptr;

    auto mail_row = mail_result->get_row(0);
    auto loaded_mail = std::make_unique<mail>();
    loaded_mail->id = mail_id;
    loaded_mail->subject = mail_row.count("subject") ? mail_row.at("subject") : std::string{};
    loaded_mail->body_path = mail_row.count("body_path") ? mail_row.at("body_path") : std::string{};
    if (mail_row.count("send_time_epoch") && !mail_row.at("send_time_epoch").empty())
        loaded_mail->send_time = static_cast<time_t>(std::stoll(mail_row.at("send_time_epoch")));
    loaded_mail->mail_over = true;
    (void)ensure_mail_raw_payload_loaded(*loaded_mail);

    auto rcpt_result = conn->query(db::sql::build_load_mail_recipients(mail_id));
    if (rcpt_result) {
        for (const auto& row : rcpt_result->get_all_rows()) {
            if (row.count("id") && !row.at("id").empty())
                loaded_mail->ids.push_back(static_cast<std::size_t>(std::stoull(row.at("id"))));
            if (loaded_mail->from.empty() && row.count("sender"))
                loaded_mail->from = row.at("sender");
            if (row.count("recipient"))
                loaded_mail->to.push_back(row.at("recipient"));
            if (row.count("status") && !row.at("status").empty())
                loaded_mail->status = std::stoi(row.at("status"));
            if (loaded_mail->source_message_id.empty() && row.count("source_message_id") && !row.at("source_message_id").empty())
                loaded_mail->source_message_id = row.at("source_message_id");
            if (row.count("send_time_epoch") && !row.at("send_time_epoch").empty())
                loaded_mail->send_time = static_cast<time_t>(std::stoll(row.at("send_time_epoch")));
        }
    }

    auto att_result = conn->query(db::sql::build_load_mail_attachments(mail_id));
    if (att_result) {
        for (const auto& row : att_result->get_all_rows()) {
            attachment att;
            att.mail_id = static_cast<std::size_t>(mail_id);
            if (row.count("id") && !row.at("id").empty())
                att.id = static_cast<std::size_t>(std::stoull(row.at("id")));
            if (row.count("filename")) att.filename = row.at("filename");
            if (row.count("filepath")) att.filepath = row.at("filepath");
            if (row.count("file_size") && !row.at("file_size").empty())
                att.file_size = static_cast<std::size_t>(std::stoull(row.at("file_size")));
            if (row.count("mime_type")) att.mime_type = row.at("mime_type");
            if (row.count("upload_time_epoch") && !row.at("upload_time_epoch").empty())
                att.upload_time = static_cast<time_t>(std::stoll(row.at("upload_time_epoch")));
            loaded_mail->attachments.push_back(std::move(att));
        }
    }
    return loaded_mail;
}

// ---- release_local_reservations ----
bool OutboxRepository::release_local_reservations(DBPool& db,
                                                  const std::vector<std::uint64_t>& outbox_ids) {
    if (outbox_ids.empty()) return true;

    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) return false;

    std::string sql = db::sql::build_outbox_release_reservations(outbox_ids);
    return conn->execute(sql);
}

// ---- mark_sent ----
bool OutboxRepository::mark_sent(DBPool& db,
                                 std::uint64_t outbox_id,
                                 const std::string& smtp_response) {
    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) return false;

    std::string sql = db::sql::build_outbox_mark_sent(outbox_id, smtp_response, conn);
    return execute_with_deadlock_retry(conn, sql, "mark_sent", outbox_id);
}

// ---- mark_retry ----
bool OutboxRepository::mark_retry(DBPool& db,
                                  std::uint64_t outbox_id,
                                  const std::string& error_message,
                                  int retry_delay_seconds) {
    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) return false;

    auto status = OutboxStatus::RETRY;
    std::string check_sql = db::sql::build_outbox_select_attempts(outbox_id);
    auto result = conn->query(check_sql);
    if (result && result->get_row_count() > 0) {
        auto row = result->get_row(0);
        const int attempts = row.count("attempt_count") ? std::stoi(row.at("attempt_count")) : 0;
        const int max_attempts = row.count("max_attempts") ? std::stoi(row.at("max_attempts")) : kDefaultMaxAttempts;
        if (attempts >= max_attempts) status = OutboxStatus::DEAD;
    }

    std::string sql = db::sql::build_outbox_mark_retry_or_dead(outbox_id,
        static_cast<int>(status), error_message, retry_delay_seconds, conn);
    return execute_with_deadlock_retry(conn, sql, "mark_retry", outbox_id);
}

// ---- mark_dead ----
bool OutboxRepository::mark_dead(DBPool& db,
                                 std::uint64_t outbox_id,
                                 const std::string& error_message) {
    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) return false;

    std::string sql = db::sql::build_outbox_mark_dead(outbox_id, error_message, conn);
    return execute_with_deadlock_retry(conn, sql, "mark_dead", outbox_id);
}

// ---- requeue_expired_leases ----
bool OutboxRepository::requeue_expired_leases(DBPool& db) {
    auto connection = db.acquire_connection();
    auto* conn = connection->operator->();
    if (!conn || !conn->is_connected()) return false;

    std::string sql = db::sql::build_outbox_requeue_expired_leases();
    return execute_with_deadlock_retry(conn, sql, "requeue_expired_leases", 0);
}

// ---- helpers ----
std::string OutboxRepository::extract_domain(const std::string& email) {
    const auto at_pos = email.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= email.size()) return {};
    return email.substr(at_pos + 1);
}

std::string OutboxRepository::escape_or_empty(IDBConnection* conn, const std::string& value) {
    if (!conn) return value;
    return conn->escape_string(value);
}

} // namespace outbound
} // namespace mail_system
