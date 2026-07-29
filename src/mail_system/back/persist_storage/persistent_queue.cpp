#include "mail_system/back/persist_storage/persistent_queue.h"
#include "mail_system/back/algorithm/snow.h"
#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/db/sql_queries.h"
#include "framework/metrics_server.h"
#include "mail_system/back/outbound/mx_routing_utils.h"
#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/outbound/outbound_server.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <cstring>
#include <optional>
#include <sstream>
#include <unordered_map>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifdef __linux__
#include <sys/sysinfo.h>
#endif

namespace mail_system {
namespace persist_storage {

// ================================================================
// 无锁 Bloom filter 去重
//   3 次哈希, 1024 bits, 锁定写入和测试, 加锁仅用于定时清除
// ================================================================
#if ENABLE_INBOUND_DEDUP_CHECK
namespace {
class DedupFilter {
    static constexpr int kBits = 1024;
    static constexpr int kSlots = kBits / 64;
    static constexpr int kWindowSec = 600;
    std::array<std::atomic<uint64_t>, kSlots> bits_{};
    std::chrono::steady_clock::time_point last_clear_{std::chrono::steady_clock::now()};
    std::mutex clear_mu_;

    static uint64_t mix(uint64_t h) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return h;
    }

    void maybe_clear() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_clear_ > std::chrono::seconds(kWindowSec)) {
            std::lock_guard<std::mutex> lk(clear_mu_);
            if (now - last_clear_ > std::chrono::seconds(kWindowSec)) {
                for (auto& b : bits_) b.store(0, std::memory_order_release);
                last_clear_ = now;
            }
        }
    }

public:
    bool test_and_set(const std::string& key) {
        maybe_clear();
        uint64_t h = std::hash<std::string>{}(key);
        uint64_t h1 = h;
        uint64_t h2 = mix(h);
        uint64_t h3 = mix(h2);

        int s1 = (h1 >> 6) % kSlots, b1 = h1 & 63;
        int s2 = (h2 >> 6) % kSlots, b2 = h2 & 63;
        int s3 = (h3 >> 6) % kSlots, b3 = h3 & 63;

        uint64_t m1 = 1ULL << b1, m2 = 1ULL << b2, m3 = 1ULL << b3;

        bool all_set = ((bits_[s1].load(std::memory_order_relaxed) & m1) != 0) &&
                       ((bits_[s2].load(std::memory_order_relaxed) & m2) != 0) &&
                       ((bits_[s3].load(std::memory_order_relaxed) & m3) != 0);

        if (!all_set) {
            bits_[s1].fetch_or(m1, std::memory_order_relaxed);
            bits_[s2].fetch_or(m2, std::memory_order_relaxed);
            bits_[s3].fetch_or(m3, std::memory_order_relaxed);
        }
        return all_set;  // true = probable duplicate
    }
};

DedupFilter g_dedup_filter;
constexpr int kInboundDedupWindowSeconds = 600;
} // namespace
#endif

namespace {

std::string extract_domain_lower(const std::string& email) {
    const auto at_pos = email.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= email.size()) {
        return {};
    }
    std::string domain = email.substr(at_pos + 1);
    std::transform(domain.begin(), domain.end(), domain.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return domain;
}

std::optional<std::uint64_t> query_available_memory_bytes() {
#ifdef __APPLE__
    mach_port_t host_port = mach_host_self();
    vm_statistics64_data_t vm_stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host_port, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stats), &count) != KERN_SUCCESS) {
        return std::nullopt;
    }

    const auto page_size = static_cast<std::uint64_t>(vm_kernel_page_size);
    return static_cast<std::uint64_t>(vm_stats.free_count + vm_stats.inactive_count) * page_size;
#elif defined(__linux__)
    struct sysinfo info {};
    if (sysinfo(&info) != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.freeram) * static_cast<std::uint64_t>(info.mem_unit);
#else
    return std::nullopt;
#endif
}

} // namespace

PersistentQueue::PersistentQueue(
    std::shared_ptr<router::IShardRouter> shard_router,
    std::shared_ptr<ThreadPoolBase> worker_pool
) : m_shardRouter(std::move(shard_router)),
    worker_pool_(std::move(worker_pool)),
    shutdown_(false) {
    // 启动工作线程
    worker_thread_ = std::thread(&PersistentQueue::worker_loop, this);
    LOG_PERSISTENT_QUEUE_INFO("PersistentQueue initialized and worker thread started");
}

int PersistentQueue::shard_from_mail(const mail* m) const {
    if (!m_shardRouter || !m) return 0;
    const std::string& key = !m->from.empty() ? m->from :
                             (!m->to.empty() ? m->to.front() : "");
    if (key.empty()) return 0;
    int shard = m_shardRouter->route(key);
    return shard >= 0 ? shard : 0;
}

PersistentQueue::~PersistentQueue() {
    shutdown();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

SubmitOwnedMailResult PersistentQueue::submit_owned_mail(std::unique_ptr<mail> mail_data) {
    SubmitOwnedMailResult result;
    result.rejected_mail = std::move(mail_data);

    if (!result.rejected_mail) {
        result.error = "mail is null";
        LOG_PERSISTENT_QUEUE_ERROR("Cannot submit mail, owned mail is null");
        return result;
    }

    if (is_shutdown()) {
        result.error = "persistent queue is shutdown";
        LOG_PERSISTENT_QUEUE_ERROR("Cannot submit mail, PersistentQueue is shutdown");
        return result;
    }

    if (std::string reason; should_reject_submission(*result.rejected_mail, reason)) {
        result.error = std::move(reason);
        LOG_PERSISTENT_QUEUE_WARN("PersistentQueue rejected mail_id={}, reason={}",
                                  result.rejected_mail->id,
                                  result.error);
        return result;
    }

    result.ticket.mail_id = result.rejected_mail->id;
    result.ticket.status = result.rejected_mail->persist_status;
    result.ticket.cancel_requested = std::make_shared<std::atomic<bool>>(false);

    {
        auto* item = new QueueItem{std::move(result.rejected_mail), result.ticket};
        while (!task_queue_.push(item)) {
            // 队列满，短暂让出
            std::this_thread::yield();
        }
        queued_task_count_.fetch_add(1, std::memory_order_relaxed);
        inflight_mail_count_.fetch_add(1, std::memory_order_relaxed);
    }
    push_queue_metrics();
    wakeup_flag_.store(true, std::memory_order_release);
    result.accepted = true;
    LOG_PERSISTENT_QUEUE_INFO("Mail submitted to PersistentQueue, mail_id={}, queued={}, inflight={}",
                              result.ticket.mail_id,
                              queue_size(),
                              inflight_mail_count_.load(std::memory_order_relaxed));
    return result;
}

bool PersistentQueue::submit_mails(std::vector<mail*>& mail_list) {
    LOG_PERSISTENT_QUEUE_WARN("submit_mails(raw pointers) is deprecated and disabled for ownership safety");
    return false;
}

void PersistentQueue::delete_task(mail* mail_data) {
    if (!mail_data) {
        LOG_PERSISTENT_QUEUE_ERROR("Cannot delete task: mail_data is null");
        return;
    }

    int shard = shard_from_mail(mail_data);
    auto conn = m_shardRouter->get_db_pool(shard)->acquire_connection();
    if (!conn.is_valid()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for deleting mail ID {}", mail_data->id);
        return;
    }

    try {
        auto* conn_ptr = conn.operator->();
        // 第一步：删除邮件收发件人关系记录
        std::string recipient_sql = db::sql::build_delete_recipients_by_mail(mail_data->id);
        if (!conn_ptr->execute(recipient_sql)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail recipients for mail ID {}", mail_data->id);
        }

        // 第二步：删除邮件元数据（会级联删除附件）
        std::string mail_sql = db::sql::build_delete_mail_by_id(mail_data->id);
        if (!conn_ptr->execute(mail_sql)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail metadata for mail ID {}", mail_data->id);
        } else {
            LOG_PERSISTENT_QUEUE_INFO("Successfully deleted mail ID {}", mail_data->id);
        }
    } catch (const std::exception& e) {
        LOG_PERSISTENT_QUEUE_ERROR("Exception during delete_task for mail ID {}: {}",
                                  mail_data->id, e.what());
    }
}

void PersistentQueue::delete_multi_tasks(std::vector<mail*>& mail_list) {
    if (mail_list.empty()) {
        return;
    }

    int shard = shard_from_mail(mail_list.front());
    auto conn = m_shardRouter->get_db_pool(shard)->acquire_connection();
    if (!conn.is_valid()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for batch delete");
        return;
    }

    try {
        // 构建批量删除SQL - 收集所有要删除的邮件ID
        std::string id_list;
        for (size_t i = 0; i < mail_list.size(); ++i) {
            if (mail_list[i]) {
                id_list += std::to_string(mail_list[i]->id);
                if (i < mail_list.size() - 1) {
                    id_list += ",";
                }
            }
        }

        if (!id_list.empty()) {
            auto* conn_ptr = conn.operator->();
            // 第一步：删除邮件收发件人关系记录
            std::string recipient_sql = db::sql::build_delete_mail_recipients_by_id_list(id_list);
            if (!conn_ptr->execute(recipient_sql)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to batch delete mail recipients");
            }

            // 第二步：删除邮件元数据（会级联删除附件）
            std::string mail_sql = db::sql::build_delete_mails_by_id_list(id_list);
            if (!conn_ptr->execute(mail_sql)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to batch delete mail metadata");
            } else {
                LOG_PERSISTENT_QUEUE_INFO("Successfully batch deleted {} mail records", mail_list.size());
            }
        }
    } catch (const std::exception& e) {
        LOG_PERSISTENT_QUEUE_ERROR("Exception during delete_multi_tasks: {}", e.what());
    }
}

size_t PersistentQueue::queue_size() const {
    return queued_task_count_.load(std::memory_order_relaxed);
}

void PersistentQueue::set_outbound_server(std::shared_ptr<mail_system::outbound::OutboundServer> server) {
    outbound_server_ = std::move(server);
}

void PersistentQueue::set_local_domain(std::string local_domain) {
    if (!local_domain.empty()) {
        local_domain_ = std::move(local_domain);
    }
}

void PersistentQueue::set_pressure_config(PersistentQueuePressureConfig config) {
    pressure_config_ = config;
}

void PersistentQueue::shutdown() {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    wakeup_flag_.store(true, std::memory_order_release);
}

bool PersistentQueue::process_task() {
    QueueItem* item = nullptr;
    int empty_spins = 0;
    while (!shutdown_.load(std::memory_order_relaxed)) {
        if (task_queue_.pop(item) && item) break;
        wakeup_flag_.store(false, std::memory_order_release);
        if (task_queue_.pop(item) && item) break;
        // 指数退让：空转几次后逐渐加长休眠
        if (empty_spins < 16) {
            std::this_thread::yield();
        } else if (empty_spins < 256) {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        ++empty_spins;
    }
    if (!item) return false;

    auto mail_data = std::move(item->mail_data);
    auto ticket = std::move(item->ticket);
    delete item;
    queued_task_count_.fetch_sub(1, std::memory_order_relaxed);
    push_queue_metrics();

    if (!mail_data) {
            inflight_mail_count_.fetch_sub(1, std::memory_order_relaxed);
            push_queue_metrics();
            LOG_PERSISTENT_QUEUE_WARN("Skipped empty mail task");
            return true;
        }

        if (mail_data->persist_status != PersistStatus::PENDING) {
            LOG_PERSISTENT_QUEUE_WARN("Mail ID {} has already been processed with status {}, skipping", mail_data->id, static_cast<int>(mail_data->persist_status));
            inflight_mail_count_.fetch_sub(1, std::memory_order_relaxed);
            push_queue_metrics();
            return true;
        }

    worker_pool_->submit([this, ticket, mail_data = std::move(mail_data)]() mutable {
        std::string error;
        const auto finish = [this]() {
            inflight_mail_count_.fetch_sub(1, std::memory_order_relaxed);
            push_queue_metrics();
        };

        if (!mail_data) {
            finish();
            return;
        }

        if (ticket.is_cancel_requested()) {
            mail_data->persist_status = PersistStatus::CANCELLED;
            cleanup_mail_files(mail_data.get());
            finish();
            return;
        }

        mail_data->persist_status = PersistStatus::PROCESSING;

        if (ticket.is_cancel_requested()) {
            mail_data->persist_status = PersistStatus::CANCELLED;
            cleanup_mail_files(mail_data.get());
            finish();
            return;
        }

        std::vector<outbound::OutboxRecord> reserved_records;
        if (persist_mail_transactional(mail_data.get(),
                                       /*reserve_owner=*/"",
                                       /*reserve_lease_seconds=*/0,
                                       &reserved_records,
                                       error)) {
            const auto mail_id = mail_data->id;
            if (ticket.is_cancel_requested()) {
                mail_data->persist_status = PersistStatus::CANCELLED;
                cleanup_failed_mail(mail_data.get());
                finish();
                return;
            }

            mail_data->persist_status = PersistStatus::SUCCESS;
            if (mail_data->deduplicated_inbound) {
                cleanup_mail_files(mail_data.get());
            } else if (outbound::has_external_recipient(*mail_data, local_domain_) && outbound_server_) {
                // 将 outbox 记录转为 MailDeliveryTask 提交到 OutboundServer
                for (auto& rec : reserved_records) {
                    auto task = std::make_unique<outbound::MailDeliveryTask>();
                    task->mail_id = rec.mail_id;
                    task->record_id = rec.id;
                    task->sender = rec.sender;
                    task->recipient = rec.recipient;
                    task->mail_ptr = std::make_shared<mail>(*mail_data);
                    task->attempt_count = rec.attempt_count;
                    task->max_attempts = rec.max_attempts;
                    outbound_server_->submit(std::move(task));
                }
            }
            // 本地收件/入站邮件：保留 body 文件供 IMAP 读取，不清理
            LOG_PERSISTENT_QUEUE_INFO("Successfully processed mail ID {}", mail_id);
        } else {
            mail_data->persist_status = PersistStatus::FAILED;
            cleanup_failed_mail(mail_data.get());
            LOG_PERSISTENT_QUEUE_ERROR("Processing failed for mail ID {}: {}", mail_data->id, error);
        }

        finish();
    });

    return true;
}

bool PersistentQueue::batch_insert_metadata(mail* mail_data, IDBConnection* conn, std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }
    if (!conn || !conn->is_connected()) {
        error = "Database connection is null";
        return false;
    }

    try {
        std::string mail_sql = db::sql::build_insert_mail(mail_data->id,
                                                           mail_data->subject,
                                                           mail_data->body_path,
                                                           mail_data->send_time,
                                                           conn);

        if (!conn->execute(mail_sql)) {
            error = "Failed to insert mail metadata";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mail metadata with sql: {}", mail_sql);
            return false;
        }

        std::string recipient_sql = db::sql::build_insert_recipients(*mail_data, local_domain_, conn);

        if (!conn->execute(recipient_sql)) {
            error = "Failed to insert mail recipients";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mail recipients with sql: {}", recipient_sql);
            return false;
        }

        // 对本域收件人：把邮件关联到用户的收件箱（mail_mailbox 表）
        // IMAP SELECT INBOX 依赖这个表来列出邮件
        for (size_t i = 0; i < mail_data->to.size(); ++i) {
            const std::string& recipient = mail_data->to[i];
            const auto domain = extract_domain_lower(recipient);
            auto system_domain = local_domain_;
            std::transform(system_domain.begin(), system_domain.end(), system_domain.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (domain.empty() || domain != system_domain) {
                continue;  // 外域收件人，不需要 mail_mailbox
            }

            std::string mailbox_sql = db::sql::build_insert_mailbox_for_recipient(mail_data->id, recipient, conn);

            if (!conn->execute(mailbox_sql)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to insert mail_mailbox for mail_id={} recipient={}",
                                          mail_data->id, recipient);
                // 不影响整体事务 —— 外发仍然需要正常出队
            }
        }

        LOG_PERSISTENT_QUEUE_INFO("Successfully inserted metadata for mail ID {} with {} recipients",
                                 mail_data->id, mail_data->to.size());
        return true;
    } catch (const std::exception& e) {
        error = std::string("Exception during metadata insertion: ") + e.what();
        LOG_PERSISTENT_QUEUE_ERROR("Exception during metadata insertion for mail ID {}: {}",
                                  mail_data->id, e.what());
        return false;
    }
}

#if ENABLE_INBOUND_DEDUP_CHECK
bool PersistentQueue::is_probable_duplicate_mail(mail* mail_data, IDBConnection* conn) {
    if (!mail_data || !conn || !conn->is_connected() || mail_data->to.empty()) {
        return false;
    }

    // 1. Bloom filter: 快速内存去重（无锁，10 分钟窗口）
    if (!mail_data->source_message_id.empty()) {
        // 为每个收件人构造唯一 key，全命中才判定重复
        bool all_seen = true;
        for (const auto& r : mail_data->to) {
            std::string key = mail_data->from + "\n" + r + "\n" + mail_data->source_message_id;
            if (!g_dedup_filter.test_and_set(key)) all_seen = false;
        }
        if (all_seen) return true;

        // 2. DB 回退：跨进程/重启后的去重
        std::string sql = db::sql::build_dedup_by_subject_sender(
            mail_data->subject, mail_data->from, kInboundDedupWindowSeconds, conn);
        auto result = conn->query(sql);
        if (result && result->get_row_count() > 0) {
            for (const auto& r : mail_data->to) {
                auto rc = conn->query(db::sql::build_dedup_by_subject_sender_recipient(
                    mail_data->subject, mail_data->from, r, kInboundDedupWindowSeconds, conn));
                if (!(rc && rc->get_row_count() > 0)) return false;
            }
            return true;
        }
    }

    // 3. 无 Message-ID 时的回退：sender+subject+recipient DB 查询
    if (mail_data->subject.empty()) return false;
    for (const auto& r : mail_data->to) {
        auto rc = conn->query(db::sql::build_dedup_by_subject_sender_recipient(
            mail_data->subject, mail_data->from, r, kInboundDedupWindowSeconds, conn));
        if (!(rc && rc->get_row_count() > 0)) return false;
    }
    return true;
}
#endif

bool PersistentQueue::is_duplicate_by_source_message_id(mail* mail_data, IDBConnection* conn) {
    if (!mail_data || !conn || !conn->is_connected() || mail_data->source_message_id.empty() || mail_data->to.empty()) {
        return false;
    }

    for (const auto& recipient_raw : mail_data->to) {
        const std::string sql = db::sql::build_dedup_by_message_id(
            mail_data->from, recipient_raw, mail_data->source_message_id, conn);
        auto result = conn->query(sql);
        if (!(result && result->get_row_count() > 0)) {
            return false;
        }
    }

    return true;
}

bool PersistentQueue::batch_insert_attachments(mail* mail_data, IDBConnection* conn, std::string& error) {
    if (!mail_data || mail_data->attachments.empty()) {
        return true;
    }
    if (!conn || !conn->is_connected()) {
        error = "Database connection is null";
        return false;
    }

    try {
        std::string sql = db::sql::build_insert_attachments(mail_data->id, mail_data->attachments, conn);

        if (!conn->execute(sql)) {
            error = "Failed to insert mail's attachment metadata";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert attachment metadata for mail ID {} with sql: {}",
                                      mail_data->id, sql);
            return false;
        }

        LOG_PERSISTENT_QUEUE_INFO("Successfully inserted {} attachments for mail ID {}", 
                                 mail_data->attachments.size(), mail_data->id);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Exception during attachment insertion: ") + e.what();
        LOG_PERSISTENT_QUEUE_ERROR("Exception during attachment insertion for mail ID {}: {}", 
                                  mail_data->id, e.what());
        return false;
    }
}

void PersistentQueue::worker_loop() {
    LOG_PERSISTENT_QUEUE_INFO("PersistentQueue worker thread started");
    while (process_task()) {
        // process_task() blocks on condition_variable, so no busy spin in idle state.
    }
    // 不在这里打印退出日志，因为可能在析构阶段调用，此时 Logger 可能已 shutdown
}

bool PersistentQueue::enqueue_outbox_tasks(mail* mail_data,
                                           IDBConnection* conn,
                                           const std::string& reserve_owner,
                                           int reserve_lease_seconds,
                                           std::vector<outbound::OutboxRecord>* reserved_records,
                                           std::string& error) {
    if (!mail_data || !conn || !conn->is_connected()) {
        error = "Mail data or connection is null";
        return false;
    }

    bool inserted_any = false;
    const bool reserve_for_local = !reserve_owner.empty() && reserve_lease_seconds > 0;

    for (const auto& recipient : mail_data->to) {
        const auto recipient_domain = extract_domain_lower(recipient);
        auto local_domain = local_domain_;
        std::transform(local_domain.begin(), local_domain.end(), local_domain.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (recipient_domain.empty() || recipient_domain == local_domain) {
            continue;
        }

        std::string sql = reserve_for_local
            ? db::sql::build_insert_outbox_reserved(mail_data->id, mail_data->from, recipient,
                                                     reserve_owner, reserve_lease_seconds, conn)
            : db::sql::build_insert_outbox_pending(mail_data->id, mail_data->from, recipient, 8, conn);

        if (!conn->execute(sql)) {
            error = "Failed to insert outbox row";
            LOG_PERSISTENT_QUEUE_ERROR("Failed to insert outbox row for mail ID {} recipient {}: {}",
                                      mail_data->id,
                                      recipient,
                                      conn->get_last_error());
            return false;
        }

        inserted_any = true;
        if (reserved_records) {
            auto result = conn->query(db::sql::build_select_last_insert_id());
            if (!(result && result->get_row_count() > 0)) {
                error = "Failed to fetch LAST_INSERT_ID for outbox row";
                return false;
            }
            auto row = result->get_row(0);
            auto it = row.find("id");
            if (it == row.end()) {
                error = "LAST_INSERT_ID result missing id";
                return false;
            }

            outbound::OutboxRecord record;
            record.id = static_cast<std::uint64_t>(std::stoull(it->second));
            record.mail_id = mail_data->id;
            record.sender = mail_data->from;
            record.recipient = recipient;
            record.body_path = mail_data->body_path;
            record.attempt_count = reserve_for_local ? 1 : 0;
            record.max_attempts = 8;
            reserved_records->push_back(std::move(record));
        }
    }

    if (!inserted_any) {
        return true;
    }

    return true;
}

bool PersistentQueue::persist_mail_transactional(mail* mail_data,
                                                 const std::string& reserve_owner,
                                                 int reserve_lease_seconds,
                                                 std::vector<outbound::OutboxRecord>* reserved_records,
                                                 std::string& error) {
    if (!mail_data) {
        error = "Mail data is null";
        return false;
    }

    int shard = shard_from_mail(mail_data);
    auto scoped = m_shardRouter->get_db_pool(shard)->acquire_connection();
    if (!scoped.is_valid()) {
        error = "Failed to get database connection";
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for message-id dedup check, mail_id={}",
                                   mail_data->id);
        return false;
    }
    auto* conn_ptr = scoped.operator->();

    if (is_duplicate_by_source_message_id(mail_data, conn_ptr)) {
        mail_data->deduplicated_inbound = true;
        LOG_PERSISTENT_QUEUE_INFO("Message-ID dedup hit, skip all persistence for mail_id={}, message_id=[{}]",
                                  mail_data->id,
                                  mail_data->source_message_id);
        return true;
    }

    if (!conn_ptr->begin_transaction()) {
        error = "Failed to begin transaction";
        return false;
    }

    const auto rollback = [&]() {
        if (!conn_ptr->rollback()) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to rollback transaction for mail_id={}", mail_data->id);
        }
    };

    if (!batch_insert_metadata(mail_data, conn_ptr, error)) {
        rollback();
        return false;
    }

    if (!batch_insert_attachments(mail_data, conn_ptr, error)) {
        rollback();
        return false;
    }

    if (!enqueue_outbox_tasks(mail_data, conn_ptr, reserve_owner, reserve_lease_seconds, reserved_records, error)) {
        rollback();
        return false;
    }

    if (!conn_ptr->commit()) {
        error = "Failed to commit transaction";
        rollback();
        return false;
    }

    // 跨分片本域投递：写入收件人所在分片（独立事务）
    for (size_t i = 0; i < mail_data->to.size(); ++i) {
        const auto& recipient = mail_data->to[i];
        const auto domain = extract_domain_lower(recipient);
        if (domain.empty()) continue;
        auto local_lower = local_domain_;
        std::transform(local_lower.begin(), local_lower.end(), local_lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (domain != local_lower) continue;  // 外域走 outbox，不在此处理

        const uint64_t recipient_id = i < mail_data->ids.size() ? mail_data->ids[i] : 0;
        if (!persist_to_recipient_shard(mail_data, recipient, recipient_id, shard)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to persist mail_id={} to recipient_shard for recipient={}",
                                      mail_data->id, recipient);
        }
    }

    return true;
}

bool PersistentQueue::persist_to_recipient_shard(mail* mail_data,
                                                 const std::string& recipient,
                                                 uint64_t recipient_id,
                                                 int sender_shard) {
    if (!m_shardRouter || !mail_data) return false;

    int recipient_shard = m_shardRouter->route(recipient);
    if (recipient_shard < 0 || recipient_shard == sender_shard) {
        return true;  // 同一分片，主事务已处理
    }

    auto scoped = m_shardRouter->get_db_pool(recipient_shard)->acquire_connection();
    if (!scoped.is_valid()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get DB connection for recipient shard {}, mail_id={}",
                                   recipient_shard, mail_data->id);
        return false;
    }
    auto* conn_ptr = scoped.operator->();

    if (!conn_ptr->begin_transaction()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to begin transaction on recipient shard {}, mail_id={}",
                                   recipient_shard, mail_data->id);
        return false;
    }

    const auto rollback = [&]() {
        if (!conn_ptr->rollback()) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to rollback on recipient shard {}, mail_id={}",
                                      recipient_shard, mail_data->id);
        }
    };

    // 1. 插入 mails 记录（与发送者分片相同的邮件元数据）
    std::string mail_sql = db::sql::build_insert_mail(mail_data->id,
                                                       mail_data->subject,
                                                       mail_data->body_path,
                                                       mail_data->send_time,
                                                       conn_ptr);
    if (!conn_ptr->execute(mail_sql)) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mails on recipient shard {}, mail_id={}: {}",
                                   recipient_shard, mail_data->id, conn_ptr->get_last_error());
        rollback();
        return false;
    }

    // 2. 插入 mail_recipients 记录（仅该收件人的行）
    const int local_status = 1;  // 本域收件人
    std::string recipient_sql = db::sql::build_insert_recipient_single(
        mail_data->id, recipient_id, mail_data->from, recipient,
        local_status, mail_data->source_message_id, conn_ptr);

    if (!conn_ptr->execute(recipient_sql)) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to insert mail_recipients on recipient shard {}, mail_id={}: {}",
                                   recipient_shard, mail_data->id, conn_ptr->get_last_error());
        rollback();
        return false;
    }

    // 3. 插入 mail_mailbox（关联到收件人收件箱）
    std::string mailbox_sql = db::sql::build_insert_mailbox_for_recipient(
        mail_data->id, recipient, conn_ptr);

    if (!conn_ptr->execute(mailbox_sql)) {
        LOG_PERSISTENT_QUEUE_WARN("Failed to insert mail_mailbox on recipient shard {} for mail_id={} recipient={}: {}",
                                  recipient_shard, mail_data->id, recipient, conn_ptr->get_last_error());
        rollback();
        return false;
    }

    if (!conn_ptr->commit()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to commit on recipient shard {}, mail_id={}: {}",
                                   recipient_shard, mail_data->id, conn_ptr->get_last_error());
        rollback();
        return false;
    }

    LOG_PERSISTENT_QUEUE_INFO("Cross-shard persist succeeded: mail_id={} recipient={} sender_shard={} -> recipient_shard={}",
                              mail_data->id, recipient, sender_shard, recipient_shard);
    return true;
}

void PersistentQueue::cleanup_mail_files(mail* mail_data) {
    if (!mail_data) {
        return;
    }

    if (!mail_data->body_path.empty()) {
        if (m_shardRouter->get_storage(0)) {
            std::string error;
            if (!m_shardRouter->get_storage(0)->remove_object(mail_data->body_path, error)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail body object: {}, error={}",
                                          mail_data->body_path,
                                          error);
            }
        } else if (std::remove(mail_data->body_path.c_str()) != 0) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail body file: {}", mail_data->body_path);
        }
    }

    for (const auto& attachment : mail_data->attachments) {
        if (attachment.filepath.empty()) {
            continue;
        }
        if (m_shardRouter->get_storage(0)) {
            std::string error;
            if (!m_shardRouter->get_storage(0)->remove_object(attachment.filepath, error)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to delete attachment object: {}, error={}",
                                          attachment.filepath,
                                          error);
            }
        } else if (std::remove(attachment.filepath.c_str()) != 0) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete attachment file: {}", attachment.filepath);
        }
    }
}

void PersistentQueue::cleanup_failed_mail(mail* mail_data) {
    if (!mail_data) {
        return;
    }

    int shard = shard_from_mail(mail_data);
    auto scoped = m_shardRouter->get_db_pool(shard)->acquire_connection();
    if (!scoped.is_valid()) {
        LOG_PERSISTENT_QUEUE_ERROR("Failed to get database connection for cleaning up failed mail ID {}", mail_data->id);
        cleanup_mail_files(mail_data);
        return;
    }

    try {
        auto* conn_ptr = scoped.operator->();

        // 删除附件元数据
        if (!mail_data->attachments.empty()) {
            std::string att_sql = db::sql::build_delete_attachments_by_mail(mail_data->id);
            if (!conn_ptr->execute(att_sql)) {
                LOG_PERSISTENT_QUEUE_WARN("Failed to delete attachment metadata for failed mail ID {}", mail_data->id);
            }
        }

        // 删除邮件收发件人关系记录
        std::string recipient_sql = db::sql::build_delete_recipients_by_mail(mail_data->id);
        if (!conn_ptr->execute(recipient_sql)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail recipients for failed mail ID {}", mail_data->id);
        }

        // 删除邮件元数据
        std::string mail_sql = db::sql::build_delete_mail_by_id(mail_data->id);
        if (!conn_ptr->execute(mail_sql)) {
            LOG_PERSISTENT_QUEUE_WARN("Failed to delete mail metadata for failed mail ID {}", mail_data->id);
        }
    } catch (const std::exception& e) {
        LOG_PERSISTENT_QUEUE_ERROR("Exception during cleanup_failed_mail for mail ID {}: {}",
                                  mail_data->id, e.what());
    }

    cleanup_mail_files(mail_data);
}

bool PersistentQueue::should_reject_submission(const mail& mail_data, std::string& reason) {
    (void)mail_data;
    if (pressure_config_.max_inflight_mails > 0 &&
        inflight_mail_count_.load(std::memory_order_relaxed) >= pressure_config_.max_inflight_mails) {
        reason = "persist inflight limit reached";
        return true;
    }

    if (is_db_under_pressure(reason)) {
        return true;
    }

    if (is_memory_under_pressure(reason)) {
        return true;
    }

    return false;
}

bool PersistentQueue::is_db_under_pressure(std::string& reason) const {
    if (pressure_config_.min_db_available_connections == 0) return false;
    auto db = m_shardRouter->get_db_pool(0);
    if (!db) return false;

    const auto available = db->get_available_connections();
    if (available >= pressure_config_.min_db_available_connections) {
        return false;
    }

    reason = "database available connections below threshold";
    return true;
}

bool PersistentQueue::is_memory_under_pressure(std::string& reason) const {
    if (pressure_config_.min_available_memory_mb == 0) {
        return false;
    }

    const auto available_bytes = query_available_memory_bytes();
    if (!available_bytes.has_value()) {
        return false;
    }

    const auto threshold_bytes = static_cast<std::uint64_t>(pressure_config_.min_available_memory_mb) * 1024ULL * 1024ULL;
    if (available_bytes.value() >= threshold_bytes) {
        return false;
    }

    reason = "available memory below threshold";
    return true;
}

// bool PersistentQueue::save_mail_body(mail* mail_data, const std::string& file_path, std::string& error) {
//     if (!mail_data) {
//         error = "Mail data is null";
//         return false;
//     }

//     if (file_path.empty()) {
//         error = "File path is empty";
//         return false;
//     }

//     try {
//         std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
//         if (!out.is_open()) {
//             error = "Failed to open file for writing: " + file_path;
//             LOG_PERSISTENT_QUEUE_ERROR("Failed to open file for writing: {}", file_path);
//             return false;
//         }

//         // 写入邮件正文
//         if (!mail_data->body.empty()) {
//             out.write(mail_data->body.data(), static_cast<std::streamsize>(mail_data->body.size()));
//             if (!out.good()) {
//                 error = "Failed to write mail body to file: " + file_path;
//                 LOG_PERSISTENT_QUEUE_ERROR("Failed to write mail body to file: {}", file_path);
//                 out.close();
//                 return false;
//             }
//         }

//         out.close();
//         LOG_PERSISTENT_QUEUE_INFO("Successfully saved mail body to file: {}", file_path);
//         return true;
//     } catch (const std::exception& e) {
//         error = std::string("Exception while saving mail body: ") + e.what();
//         LOG_PERSISTENT_QUEUE_ERROR("Exception while saving mail body for mail ID {}: {}", 
//                                   mail_data->id, e.what());
//         return false;
//     }
// }

// bool PersistentQueue::save_attachments(mail* mail_data, std::string& error) {
//     if (!mail_data || mail_data->attachments.empty()) {
//         return true; // 没有附件不算错误
//     }

//     try {
//         for (auto& att : mail_data->attachments) {
//             // 如果附件已经保存到文件（filepath已设置），跳过
//             if (!att.filepath.empty() && att.content.empty()) {
//                 LOG_PERSISTENT_QUEUE_DEBUG("Attachment {} already saved to file: {}", 
//                                           att.filename, att.filepath);
//                 continue;
//             }

//             // 如果filepath为空但有内容，需要保存
//             if (att.filepath.empty() && !att.content.empty()) {
//                 error = "Attachment filepath is empty but content exists: " + att.filename;
//                 LOG_PERSISTENT_QUEUE_ERROR("Attachment filepath is empty but content exists: {}", att.filename);
//                 return false;
//             }

//             // 如果有filepath且有content，保存content到文件
//             if (!att.filepath.empty() && !att.content.empty()) {
//                 std::ofstream out(att.filepath, std::ios::binary | std::ios::trunc);
//                 if (!out.is_open()) {
//                     error = "Failed to open attachment file for writing: " + att.filepath;
//                     LOG_PERSISTENT_QUEUE_ERROR("Failed to open attachment file for writing: {}", att.filepath);
//                     return false;
//                 }

//                 out.write(att.content.data(), static_cast<std::streamsize>(att.content.size()));
//                 if (!out.good()) {
//                     error = "Failed to write attachment to file: " + att.filepath;
//                     LOG_PERSISTENT_QUEUE_ERROR("Failed to write attachment to file: {}", att.filepath);
//                     out.close();
//                     return false;
//                 }

//                 out.close();
//                 att.file_size = att.content.size();
//                 att.content.clear(); // 释放内存
//                 LOG_PERSISTENT_QUEUE_INFO("Successfully saved attachment {} to file: {}", 
//                                          att.filename, att.filepath);
//             }
//         }

//         LOG_PERSISTENT_QUEUE_INFO("Successfully processed all attachments for mail ID {}", mail_data->id);
//         return true;
//     } catch (const std::exception& e) {
//         error = std::string("Exception while saving attachments: ") + e.what();
//         LOG_PERSISTENT_QUEUE_ERROR("Exception while saving attachments for mail ID {}: {}", 
//                                   mail_data->id, e.what());
//         return false;
//     }
// }

void PersistentQueue::push_queue_metrics() {
    if (auto m = metrics_.lock()) {
        m->set_gauge("protorelay_queue_inflight", {}, inflight_mail_count_.load());
        m->set_gauge("protorelay_queue_depth", {}, queued_task_count_.load());
    }
}

} // namespace persist_storage
} // namespace mail_system
