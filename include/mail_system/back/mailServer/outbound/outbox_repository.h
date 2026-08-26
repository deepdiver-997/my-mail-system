#ifndef MAIL_SYSTEM_OUTBOX_REPOSITORY_H
#define MAIL_SYSTEM_OUTBOX_REPOSITORY_H

#include "mail_system/back/entities/mail.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mail_system {

// IDBConnection/DBPool now in pr::, brought into mail_system via db headers

namespace outbound {

enum class OutboxStatus : int {
    PENDING = 0,
    SENDING = 1,
    SENT = 2,
    RETRY = 3,
    DEAD = 4,
};

struct OutboxRecord {
    std::uint64_t id{0};
    std::uint64_t mail_id{0};
    std::string sender;
    std::string recipient;
    std::string body_path;
    int attempt_count{0};
    int max_attempts{8};
};

// 无状态工具类：所有方法在调用时接收 DBPool，不持有连接池
class OutboxRepository {
public:
    OutboxRepository() = default;

    bool enqueue_from_mail(DBPool& db,
                           const mail& mail_data,
                           const std::string& local_domain,
                           std::vector<std::uint64_t>* outbox_ids = nullptr);

    std::vector<OutboxRecord> claim_batch(DBPool& db,
                                          const std::string& worker_id,
                                          std::size_t limit,
                                          int lease_seconds);

    std::unique_ptr<mail> load_mail(DBPool& db, std::uint64_t mail_id);

    bool release_local_reservations(DBPool& db,
                                    const std::vector<std::uint64_t>& outbox_ids);

    bool mark_sent(DBPool& db,
                   std::uint64_t outbox_id,
                   const std::string& smtp_response);
    bool mark_retry(DBPool& db,
                    std::uint64_t outbox_id,
                    const std::string& error_message,
                    int retry_delay_seconds);
    bool mark_dead(DBPool& db,
                   std::uint64_t outbox_id,
                   const std::string& error_message);
    bool requeue_expired_leases(DBPool& db);

private:
    static std::string extract_domain(const std::string& email);
    static std::string escape_or_empty(IDBConnection* conn,
                                       const std::string& value);
};

} // namespace outbound
} // namespace mail_system

#endif
