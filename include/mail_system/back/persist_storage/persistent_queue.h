#ifndef PERSISTENT_QUEUE_H
#define PERSISTENT_QUEUE_H

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/mysql_pool.h"
#include "mail_system/back/mailServer/outbound/outbox_repository.h"
#include "mail_system/back/mailServer/outbound_server.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/common/logger.h"
#include <boost/lockfree/queue.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mail_system { class MetricsServer; }

namespace mail_system {
namespace persist_storage {

struct PersistSubmissionTicket {
    std::uint64_t mail_id{0};
    mail_system::persist_storage::SharedPersistStatus status{};
    std::shared_ptr<std::atomic<bool>> cancel_requested{std::make_shared<std::atomic<bool>>(false)};

    bool valid() const {
        return mail_id != 0;
    }

    void request_cancel() const {
        if (cancel_requested) {
            cancel_requested->store(true, std::memory_order_release);
        }
    }

    bool is_cancel_requested() const {
        return cancel_requested && cancel_requested->load(std::memory_order_acquire);
    }
};

struct SubmitOwnedMailResult {
    bool accepted{false};
    PersistSubmissionTicket ticket{};
    std::unique_ptr<mail> rejected_mail{};
    std::string error{};
};

struct PersistentQueuePressureConfig {
    std::size_t max_inflight_mails{2048};
    std::size_t min_available_memory_mb{256};
    std::size_t min_db_available_connections{1};
};

class PersistentQueue {
public:
    PersistentQueue(
        std::shared_ptr<router::IShardRouter> shard_router,
        std::shared_ptr<ThreadPoolBase> worker_pool
    );

    ~PersistentQueue();

    // ---- 公开接口 ----
    SubmitOwnedMailResult submit_owned_mail(std::unique_ptr<mail> mail_data);
    bool submit_mails(std::vector<mail*>& mail_list);
    void delete_task(mail* mail_data);
    void delete_multi_tasks(std::vector<mail*>& mail_list);

    size_t queue_size() const;
    size_t inflight_count() const {
        return inflight_mail_count_.load(std::memory_order_relaxed);
    }

    void set_outbound_server(std::shared_ptr<mail_system::outbound::OutboundServer> server);
    void set_local_domain(std::string local_domain);
    void set_batch_pop_size(size_t batch_size);
    void set_pressure_config(PersistentQueuePressureConfig config);
    // 出站投递最大尝试次数：写进 mail_outbox.max_attempts 行（供 claim/重试/DEAD 判断）
    void set_max_attempts(size_t max_attempts);

    void shutdown();
    bool is_shutdown() const { return shutdown_.load(std::memory_order_acquire); }

    void inject_metrics(std::weak_ptr<MetricsServer> m) { metrics_ = std::move(m); }

    // 持久化回调: void(bool success, std::string error)
    using PersistCallback = std::function<void(bool, std::string)>;

private:
    void push_queue_metrics();
    // ---- 工作线程 ----
    void worker_loop();
    bool process_task();

    // ---- 持久化内部方法（async 回调链，ScopedConnection 由顶层持有） ----
    void persist_mail_transactional_async(mail* mail_data,
                                          const std::string& reserve_owner,
                                          int reserve_lease_seconds,
                                          std::vector<outbound::OutboxRecord>* reserved_records,
                                          PersistCallback cb);
    void batch_insert_metadata_async(mail* mail_data, class IDBConnection* conn,
                                     PersistCallback cb);
    void batch_insert_attachments_async(mail* mail_data, class IDBConnection* conn,
                                        PersistCallback cb);
    void enqueue_outbox_tasks_async(mail* mail_data,
                                    class IDBConnection* conn,
                                    const std::string& reserve_owner,
                                    int reserve_lease_seconds,
                                    std::vector<outbound::OutboxRecord>* reserved_records,
                                    PersistCallback cb);
#if ENABLE_INBOUND_DEDUP_CHECK
    void is_probable_duplicate_mail_async(mail* mail_data, class IDBConnection* conn,
                                          std::function<void(bool)> cb);
#endif
    void is_duplicate_by_source_message_id_async(mail* mail_data, class IDBConnection* conn,
                                                  std::function<void(bool)> cb);

    // ---- 清理 ----
    void cleanup_mail_files(mail* mail_data);
    void cleanup_failed_mail(mail* mail_data);

    // ---- 背压检查 ----
    bool should_reject_submission(const mail& mail_data, std::string& reason);
    bool is_db_under_pressure(std::string& reason) const;
    bool is_memory_under_pressure(std::string& reason) const;

    // ---- 分片辅助 ----
    int shard_from_mail(const mail* m) const;

    // ---- 跨分片本域投递 ----
    // 将邮件元数据、收件人关系、mailbox 关联写入收件人所在分片（独立事务）
    // 用于收件人与发件人在不同分片的情况，避免走 SMTP 投递
    bool persist_to_recipient_shard(mail* mail_data, const std::string& recipient,
                                    std::uint64_t recipient_id, int sender_shard);

    // ---- 成员变量 ----
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<ThreadPoolBase> worker_pool_;
    std::shared_ptr<mail_system::outbound::OutboundServer> outbound_server_;
    std::string local_domain_{"example.com"};
    size_t max_attempts_{8};               // outbox 行 max_attempts（默认对齐 kDefaultMaxAttempts）
    PersistentQueuePressureConfig pressure_config_{};

    // 无锁队列：多生产者(session)单消费者(worker thread)
    struct QueueItem {
        std::unique_ptr<mail> mail_data;
        PersistSubmissionTicket ticket;
    };
    boost::lockfree::queue<QueueItem*, boost::lockfree::capacity<16384>> task_queue_{};
    std::atomic<size_t> queued_task_count_{0};
    std::atomic<bool> wakeup_flag_{false};  // worker 自旋等待的低开销唤醒
    std::atomic<size_t> inflight_mail_count_{0};
    std::atomic<size_t> batch_pop_size_{16};

    std::atomic<bool> shutdown_;
    std::thread worker_thread_;
    std::weak_ptr<MetricsServer> metrics_;
};

} // namespace persist_storage
} // namespace mail_system

#endif // PERSISTENT_QUEUE_H
