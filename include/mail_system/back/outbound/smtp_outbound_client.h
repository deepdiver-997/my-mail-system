#ifndef MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H
#define MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H

#include "mail_system/back/outbound/dns_resolver.h"
#include "mail_system/back/outbound/outbound_config.h"

namespace mail_system { class MetricsServer; }
#include "mail_system/back/outbound/outbox_repository.h"
#include "mail_system/back/router/i_shard_router.h"
#include "framework/thread_pool/thread_pool_base.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace outbound {

enum class DeliveryState : int {
    PENDING = 0,
    SUCCESS = 1,
    FAILED = 2,
};

struct DeliveryCompletion {
    std::uint64_t outbox_id{0};
    std::uint64_t mail_id{0};
    std::uint64_t dispatch_attempt_id{0};
    bool success{false};
    bool permanent_failure{false};
    int retry_delay_seconds{30};
    std::string smtp_response;
    std::string error_message;
};

struct HotMailDispatch {
    std::unique_ptr<mail> mail_ptr;
    std::vector<OutboxRecord> reserved_records;
};

struct HotDeliveryTask {
    OutboxRecord record;
    std::shared_ptr<mail> mail_ctx;
};

class SmtpOutboundClient {
public:
    SmtpOutboundClient(std::shared_ptr<router::IShardRouter> shard_router,
                       std::shared_ptr<ThreadPoolBase> io_thread_pool,
                       std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                       std::shared_ptr<IDnsResolver> dns_resolver,
                       std::shared_ptr<std::atomic<bool>> server_interrupt_flag,
                       OutboundConfig config,
                       std::string local_domain);

    ~SmtpOutboundClient();

    void start();
    void stop();

    bool accept_mail_ownership(std::unique_ptr<mail> mail_ptr);
    bool accept_reserved_mail_ownership(std::unique_ptr<mail> mail_ptr,
                                        std::vector<OutboxRecord> reserved_records);
    void notify_outbox_ready();
    const std::string& worker_id() const { return worker_id_; }
    int local_reservation_lease_seconds() const;
    std::shared_ptr<IDnsResolver> get_dns_resolver() const { return dns_resolver_; }

    void inject_metrics(std::weak_ptr<MetricsServer> m) { metrics_ = std::move(m); }

private:
    void run_loop();
    void drain_notifications();
    std::size_t drain_hot_records();
    void schedule_claimed_records(std::vector<OutboxRecord> claimed_records);
    void drain_completion_queue();
    void dispatch_delivery_task(const OutboxRecord& record,
                                std::shared_ptr<mail> hot_mail_ctx = nullptr);
    void push_completion(DeliveryCompletion completion);
    bool try_enqueue_hot_dispatch(std::unique_ptr<mail>& mail_ptr,
                                  std::vector<OutboxRecord>& reserved_records);

private:
    std::shared_ptr<router::IShardRouter> m_shardRouter;
    std::shared_ptr<ThreadPoolBase> io_thread_pool_;
    std::shared_ptr<ThreadPoolBase> worker_thread_pool_;
    std::shared_ptr<IDnsResolver> dns_resolver_;
    std::shared_ptr<std::atomic<bool>> server_interrupt_flag_;
    OutboundConfig config_;
    OutboxRepository repository_;
    std::weak_ptr<MetricsServer> metrics_;
    std::string local_domain_;
    std::string worker_id_;
    size_t home_shard_{0};                           // 本地优先 shard
    std::vector<size_t> steal_shard_order_;          // 偷任务时的 shard 顺序（按延迟/优先级）

    std::thread orchestrator_thread_;
    std::atomic<bool> running_{false};

    std::mutex notify_mutex_;
    std::condition_variable notify_cv_;
    std::queue<HotMailDispatch> ownership_queue_;
    std::queue<HotDeliveryTask> hot_record_queue_;
    std::unordered_map<std::uint64_t, std::shared_ptr<mail>> hot_mail_cache_;
    std::unordered_map<std::uint64_t, std::size_t> hot_mail_pending_dispatch_counts_;

    std::mutex completion_mutex_;
    std::queue<DeliveryCompletion> completion_queue_;
};

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_SMTP_OUTBOUND_CLIENT_H
