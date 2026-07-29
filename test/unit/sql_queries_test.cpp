#include "mail_system/back/db/sql_queries.h"
#include "mail_system/back/entities/mail.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(const char* name, bool condition, const std::string& detail = {}) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL: " << name;
    if (!detail.empty()) std::cerr << " — " << detail;
    std::cerr << '\n';
}

void check_contains(const char* name, const std::string& sql, const std::string& expected_substr) {
    if (sql.find(expected_substr) != std::string::npos) return;
    ++g_failures;
    std::cerr << "FAIL: " << name << " — expected substring '" << expected_substr
              << "' not found in:\n  " << sql << '\n';
}

// ---- Mail persistence ----

void test_build_insert_mail() {
    std::string sql = mail_system::db::sql::build_insert_mail(
        12345ULL, "Test Subject", "/tmp/body.eml", 1718234567, nullptr);
    check_contains("insert_mail_has_table", sql, "INSERT INTO mails");
    check_contains("insert_mail_has_id", sql, "12345");
    check_contains("insert_mail_has_subject", sql, "'Test Subject'");
    check_contains("insert_mail_has_body_path", sql, "'/tmp/body.eml'");
    check_contains("insert_mail_has_from_unixtime", sql, "FROM_UNIXTIME(1718234567)");
}

void test_build_insert_recipients() {
    mail m;
    m.id = 100ULL;
    m.from = "alice@example.com";
    m.to = {"bob@example.com", "charlie@qq.com"};
    m.ids = {200ULL, 201ULL};
    m.source_message_id = "<msg123@example.com>";

    std::string sql = mail_system::db::sql::build_insert_recipients(m, "example.com", nullptr);
    check_contains("insert_rcpt_has_table", sql, "INSERT INTO mail_recipients");
    check_contains("insert_rcpt_has_mail_id", sql, "100");
    check_contains("insert_rcpt_has_sender", sql, "'alice@example.com'");
    check_contains("insert_rcpt_bob", sql, "'bob@example.com'");
    check_contains("insert_rcpt_charlie", sql, "'charlie@qq.com'");
    // same-domain status=1, external status=2
    check_contains("insert_rcpt_local_status", sql, ", 1, '");
    check_contains("insert_rcpt_external_status", sql, ", 2, '");
    check_contains("insert_rcpt_has_msgid", sql, "'<msg123@example.com>'");
}

void test_build_insert_recipient_single() {
    std::string sql = mail_system::db::sql::build_insert_recipient_single(
        100ULL, 300ULL, "alice@x.com", "bob@x.com", 1, "<mid>", nullptr);
    check_contains("insert_rcpt_single_table", sql, "INSERT INTO mail_recipients");
    check_contains("insert_rcpt_single_id", sql, "300");
    check_contains("insert_rcpt_single_mail_id", sql, "100");
    check_contains("insert_rcpt_single_sender", sql, "'alice@x.com'");
    check_contains("insert_rcpt_single_status", sql, ", 1, ");
}

void test_build_insert_mailbox() {
    std::string sql = mail_system::db::sql::build_insert_mailbox_for_recipient(
        100ULL, "bob@example.com", nullptr);
    check_contains("insert_mbox_table", sql, "INSERT INTO mail_mailbox");
    check_contains("insert_mbox_join", sql, "JOIN mailboxes mb");
    check_contains("insert_mbox_box_type", sql, "box_type = 1");
    check_contains("insert_mbox_recipient", sql, "'bob@example.com'");
}

void test_build_insert_attachments() {
    std::vector<attachment> atts;
    attachment a1;
    a1.filename = "report.pdf";
    a1.filepath = "/data/r1.pdf";
    a1.file_size = 1024;
    a1.mime_type = "application/pdf";
    atts.push_back(a1);

    std::string sql = mail_system::db::sql::build_insert_attachments(100ULL, atts, nullptr);
    check_contains("insert_att_table", sql, "INSERT INTO attachments");
    check_contains("insert_att_filename", sql, "'report.pdf'");
    check_contains("insert_att_mime", sql, "'application/pdf'");
    check_contains("insert_att_size", sql, "1024");

    // Empty list returns empty string
    std::string empty = mail_system::db::sql::build_insert_attachments(100ULL, {}, nullptr);
    check("insert_att_empty", empty.empty(), "expected empty string for no attachments");
}

// ---- Outbox ----

void test_build_insert_outbox_pending() {
    std::string sql = mail_system::db::sql::build_insert_outbox_pending(
        100ULL, "alice@x.com", "bob@y.com", 8, nullptr);
    check_contains("outbox_pending_table", sql, "INSERT INTO mail_outbox");
    check_contains("outbox_pending_status", sql, "status, priority, attempt_count, max_attempts, next_attempt_at) VALUES");
    check_contains("outbox_pending_nolease", sql, "NOW())");
    check_contains("outbox_pending_sender", sql, "'alice@x.com'");
    check_contains("outbox_pending_no_lease", sql, "NOW())");
}

void test_build_insert_outbox_reserved() {
    std::string sql = mail_system::db::sql::build_insert_outbox_reserved(
        100ULL, "alice@x.com", "bob@y.com", "worker-01", 30, nullptr);
    check_contains("outbox_resv_table", sql, "INSERT INTO mail_outbox");
    check_contains("outbox_resv_has_lease_cols", sql, "lease_owner, lease_until) VALUES");
    check_contains("outbox_resv_lease_owner", sql, "'worker-01'");
    check_contains("outbox_resv_lease_sec", sql, "INTERVAL 30 SECOND");
}

void test_build_outbox_claim_select() {
    std::string sql = mail_system::db::sql::build_outbox_claim_select(10);
    check_contains("claim_select_from", sql, "FROM mail_outbox o LEFT JOIN mails m");
    check_contains("claim_select_where", sql, "WHERE (status = ");
    check_contains("claim_select_limit", sql, "LIMIT 10");
}

void test_build_outbox_claim_update() {
    std::string sql = mail_system::db::sql::build_outbox_claim_update(500ULL, "w1", 30, nullptr);
    check_contains("claim_update_table", sql, "UPDATE mail_outbox");
    check_contains("claim_update_id", sql, "WHERE id = 500");
    check_contains("claim_update_worker", sql, "'w1'");
}

void test_build_outbox_release() {
    std::vector<std::uint64_t> ids{1, 2, 3};
    std::string sql = mail_system::db::sql::build_outbox_release_reservations(ids);
    check_contains("release_table", sql, "UPDATE mail_outbox");
    check_contains("release_in", sql, "WHERE id IN (1,2,3)");
    check_contains("release_greatest", sql, "GREATEST");

    // Empty list returns empty string
    std::string empty = mail_system::db::sql::build_outbox_release_reservations({});
    check("release_empty", empty.empty());
}

void test_build_outbox_mark_sent() {
    std::string sql = mail_system::db::sql::build_outbox_mark_sent(500ULL, "250 OK", nullptr);
    check_contains("mark_sent_table", sql, "UPDATE mail_outbox");
    check_contains("mark_sent_sent_at", sql, "sent_at = NOW()");
    check_contains("mark_sent_response", sql, "'250 OK'");
}

void test_build_outbox_mark_dead() {
    std::string sql = mail_system::db::sql::build_outbox_mark_dead(500ULL, "timeout", nullptr);
    check_contains("mark_dead_null_lease", sql, "lease_owner = NULL, lease_until = NULL, updated_at = NOW()");
    check_contains("mark_dead_error", sql, "'timeout'");
}

void test_build_outbox_requeue() {
    std::string sql = mail_system::db::sql::build_outbox_requeue_expired_leases();
    check_contains("requeue_table", sql, "UPDATE mail_outbox");
    check_contains("requeue_next_attempt", sql, "next_attempt_at = NOW()");
    check_contains("requeue_lease", sql, "lease_until IS NOT NULL AND lease_until <= NOW()");
}

// ---- Mail load ----

void test_build_load_mail_metadata() {
    std::string sql = mail_system::db::sql::build_load_mail_metadata(100ULL);
    check_contains("load_mail_table", sql, "FROM mails");
    check_contains("load_mail_id", sql, "100");
    check_contains("load_mail_limit", sql, "LIMIT 1");
}

void test_build_load_mail_recipients() {
    std::string sql = mail_system::db::sql::build_load_mail_recipients(100ULL);
    check_contains("load_rcpt_table", sql, "FROM mail_recipients");
    check_contains("load_rcpt_order", sql, "ORDER BY id ASC");
}

void test_build_load_mail_attachments() {
    std::string sql = mail_system::db::sql::build_load_mail_attachments(100ULL);
    check_contains("load_att_table", sql, "FROM attachments");
    check_contains("load_att_order", sql, "ORDER BY id ASC");
}

// ---- Dedup ----

void test_build_dedup_by_subject_sender() {
    std::string sql = mail_system::db::sql::build_dedup_by_subject_sender(
        "Hello", "alice@x.com", 600, nullptr);
    check_contains("dedup_ss_table", sql, "FROM mails");
    check_contains("dedup_ss_subquery", sql, "SELECT mail_id FROM mail_recipients");
    check_contains("dedup_ss_timestampdiff", sql, "TIMESTAMPDIFF(SECOND");
    check_contains("dedup_ss_window", sql, "BETWEEN 0 AND 600");
    check_contains("dedup_ss_subject", sql, "'Hello'");
}

void test_build_dedup_by_ssr() {
    std::string sql = mail_system::db::sql::build_dedup_by_subject_sender_recipient(
        "Hello", "alice@x.com", "bob@x.com", 600, nullptr);
    check_contains("dedup_ssr_join", sql, "JOIN mails m");
    check_contains("dedup_ssr_all", sql, "r.sender='alice@x.com'");
    check_contains("dedup_ssr_rcpt", sql, "r.recipient='bob@x.com'");
    check_contains("dedup_ssr_subject", sql, "m.subject='Hello'");
}

void test_build_dedup_by_msgid() {
    std::string sql = mail_system::db::sql::build_dedup_by_message_id(
        "alice@x.com", "bob@x.com", "<abc123>", nullptr);
    check_contains("dedup_msgid_table", sql, "FROM mail_recipients");
    check_contains("dedup_msgid_sender", sql, "sender='alice@x.com'");
    check_contains("dedup_msgid_rcpt", sql, "recipient='bob@x.com'");
    check_contains("dedup_msgid_id", sql, "source_message_id='<abc123>'");
}

// ---- Auth ----

void test_build_auth_user_query() {
    std::string sql = mail_system::db::sql::build_auth_user_query();
    check_contains("auth_user_from", sql, "FROM users");
    check_contains("auth_user_where", sql, "mail_address = ?");
}

void test_build_update_last_login() {
    std::string sql = mail_system::db::sql::build_update_last_login();
    check_contains("update_login_table", sql, "UPDATE users");
    check_contains("update_login_where", sql, "mail_address = ?");
}

// ---- IMAP ----

void test_build_imap_list_mailboxes() {
    std::string sql = mail_system::db::sql::build_imap_list_mailboxes();
    check_contains("imap_list_table", sql, "FROM mailboxes");
    check_contains("imap_list_where", sql, "user_id = ?");
}

void test_build_imap_get_mailbox_mails() {
    std::string sql = mail_system::db::sql::build_imap_get_mailbox_mails();
    check_contains("imap_mails_join_mm", sql, "FROM mail_mailbox mm");
    check_contains("imap_mails_join_m", sql, "JOIN mails m");
    check_contains("imap_mails_join_mr", sql, "JOIN mail_recipients mr");
    check_contains("imap_mails_order", sql, "ORDER BY m.send_time DESC");
}

void test_build_imap_mailbox_unseen_count() {
    std::string sql = mail_system::db::sql::build_imap_mailbox_unseen_count();
    check_contains("imap_unseen_count", sql, "COUNT(*)");
    check_contains("imap_unseen_status", sql, "mr.status = 1");
}

void test_build_imap_append_queries() {
    std::string meta = mail_system::db::sql::build_imap_append_mail_metadata();
    check_contains("imap_append_meta", meta, "INSERT INTO mails");

    std::string rcpt = mail_system::db::sql::build_imap_append_mail_recipient();
    check_contains("imap_append_rcpt", rcpt, "INSERT INTO mail_recipients");

    std::string mbox = mail_system::db::sql::build_imap_append_mailbox();
    check_contains("imap_append_mbox", mbox, "INSERT INTO mail_mailbox");
}

void test_build_imap_expunge() {
    std::string del = mail_system::db::sql::build_imap_expunge_delete_mailbox();
    check_contains("imap_expunge_del", del, "DELETE FROM mail_mailbox");
    check_contains("imap_expunge_cond", del, "is_deleted = 1");
}

void test_build_imap_copy() {
    std::string check_sql = mail_system::db::sql::build_imap_copy_check_exists();
    check_contains("imap_copy_check", check_sql, "FROM mail_mailbox");

    std::string insert_sql = mail_system::db::sql::build_imap_copy_insert_mailbox();
    check_contains("imap_copy_insert", insert_sql, "INSERT INTO mail_mailbox");
}

// ---- Shard routing ----

void test_build_shard_lookup() {
    std::string sql = mail_system::db::sql::build_shard_lookup(
        "user_shards", "email", "shard_id", "bob@example.com");
    check_contains("shard_table", sql, "FROM user_shards");
    check_contains("shard_col", sql, "SELECT shard_id");
    check_contains("shard_email", sql, "email = 'bob@example.com'");
}

// ---- Cleanup ----

void test_build_delete_queries() {
    check_contains("del_att", mail_system::db::sql::build_delete_attachments_by_mail(100),
                   "DELETE FROM attachments WHERE mail_id = 100");
    check_contains("del_rcpt", mail_system::db::sql::build_delete_recipients_by_mail(100),
                   "DELETE FROM mail_recipients WHERE mail_id = 100");
    check_contains("del_mail", mail_system::db::sql::build_delete_mail_by_id(100),
                   "DELETE FROM mails WHERE id = 100");
    check_contains("del_rcpts_list", mail_system::db::sql::build_delete_mail_recipients_by_id_list("1,2,3"),
                   "DELETE FROM mail_recipients WHERE mail_id IN (1,2,3)");
    check_contains("del_mails_list", mail_system::db::sql::build_delete_mails_by_id_list("1,2,3"),
                   "DELETE FROM mails WHERE id IN (1,2,3)");
}

// ---- Utilities ----

void test_build_select_last_insert_id() {
    std::string sql = mail_system::db::sql::build_select_last_insert_id();
    check_contains("last_insert_id", sql, "LAST_INSERT_ID()");
}

void test_build_select_row_count() {
    std::string sql = mail_system::db::sql::build_select_row_count();
    check_contains("row_count", sql, "ROW_COUNT()");
}

} // namespace

int main() {
    std::cout << "Running SQL query tests...\n";

    test_build_insert_mail();
    test_build_insert_recipients();
    test_build_insert_recipient_single();
    test_build_insert_mailbox();
    test_build_insert_attachments();

    test_build_insert_outbox_pending();
    test_build_insert_outbox_reserved();
    test_build_outbox_claim_select();
    test_build_outbox_claim_update();
    test_build_outbox_release();
    test_build_outbox_mark_sent();
    test_build_outbox_mark_dead();
    test_build_outbox_requeue();

    test_build_load_mail_metadata();
    test_build_load_mail_recipients();
    test_build_load_mail_attachments();

    test_build_dedup_by_subject_sender();
    test_build_dedup_by_ssr();
    test_build_dedup_by_msgid();

    test_build_auth_user_query();
    test_build_update_last_login();

    test_build_imap_list_mailboxes();
    test_build_imap_get_mailbox_mails();
    test_build_imap_mailbox_unseen_count();
    test_build_imap_append_queries();
    test_build_imap_expunge();
    test_build_imap_copy();

    test_build_shard_lookup();

    test_build_delete_queries();

    test_build_select_last_insert_id();
    test_build_select_row_count();

    if (g_failures == 0) {
        std::cout << "All SQL query tests passed!\n";
        return 0;
    }

    std::cerr << '\n' << g_failures << " test(s) FAILED.\n";
    return 1;
}
