#include "mail_system/back/outbound/smtp_outbound_client.h"

#include "framework/metrics_server.h"
#include "mail_system/back/outbound/cares_dns_resolver.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/outbound/mx_routing_utils.h"
#include "mail_system/back/outbound/outbound_utils.h"
#include "mail_system/back/outbound/smtp_outbound_transaction.h"
#include "mail_system/back/outbound/smtp_transport_utils.h"
#include "framework/thread_pool/io_thread_pool.h"

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace mail_system {
namespace outbound {

namespace {
constexpr std::size_t kClaimBatchSize = 32;
constexpr int kLeaseSeconds = 45;
constexpr int kDefaultRetryDelaySeconds = 30;
std::atomic<std::uint64_t> g_dispatch_attempt_seq{0};

namespace st = smtp_transport;
using smtp_transport::ContinueFn;

std::string join_ports(const std::vector<std::uint16_t>& ports) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        oss << ports[i];
        if (i + 1 < ports.size()) {
            oss << ",";
        }
    }
    return oss.str();
}

std::chrono::milliseconds compute_adaptive_wait(const OutboundConfig& cfg,
                                                std::size_t empty_claim_rounds,
                                                bool has_memory_pending) {
    const int busy_sleep_ms = std::max(1, cfg.busy_sleep_ms);
    const int backoff_base_ms = std::max(1, cfg.backoff_base_ms);
    const int backoff_max_ms = std::max(busy_sleep_ms, cfg.backoff_max_ms);
    const std::size_t backoff_shift_cap = cfg.backoff_shift_cap;

    if (has_memory_pending) {
        return std::chrono::milliseconds(busy_sleep_ms);
    }

    const std::size_t shift = std::min(empty_claim_rounds, backoff_shift_cap);
    int wait_ms = backoff_base_ms << shift;
    wait_ms = std::max(busy_sleep_ms, std::min(wait_ms, backoff_max_ms));
    return std::chrono::milliseconds(wait_ms);
}

std::string rewrite_sender_domain(const std::string& sender, const std::string& domain_override) {
    if (domain_override.empty()) {
        return sender;
    }
    const auto at_pos = sender.find('@');
    if (at_pos == std::string::npos) {
        return sender;
    }
    return sender.substr(0, at_pos + 1) + domain_override;
}

SmtpExecResult run_plain_smtp_flow(const OutboxRecord& record,
                                   const mail* hot_mail,
                                   const OutboundConfig& config,
                                   const std::string& target_host,
                                   const ContinueFn& should_continue) {
    SmtpExecResult result;
    if (should_continue && !should_continue()) {
        result.error_message = "outbound client stopping";
        return result;
    }

    if (config.ports.empty()) {
        result.error_message = "config.ports is empty";
        return result;
    }

    LOG_OUTBOUND_DEBUG("Outbound SMTP start: outbox_id={}, mail_id={}, recipient={}, target_host={}, ports=[{}]",
                     record.id,
                     record.mail_id,
                     record.recipient,
                     target_host,
                     join_ports(config.ports));

    const std::string helo_domain = config.helo_domain.empty() ? "outbound.local" : config.helo_domain;
    const std::string envelope_sender = rewrite_sender_domain(record.sender, config.mail_from_domain);
    const std::string header_from = config.rewrite_header_from ? envelope_sender : record.sender;

    boost::asio::io_context io_context;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none);

    boost::system::error_code addr_ec;
    auto target_ip = boost::asio::ip::make_address(target_host, addr_ec);
    if (addr_ec) {
        result.error_message = "invalid target address: " + target_host;
        return result;
    }

    std::uint16_t connected_port = 0;
    for (auto port : config.ports) {
        if (should_continue && !should_continue()) {
            result.error_message = "outbound client stopping";
            break;
        }

        boost::asio::ip::tcp::socket socket(io_context);
        std::string connect_error;
        if (!smtp_transport::run_interruptible_ec(
                socket,
                [&](auto handler) { socket.async_connect(boost::asio::ip::tcp::endpoint(target_ip, port), std::move(handler)); },
                should_continue,
                smtp_transport::kIoOperationTimeout,
                "smtp connect",
                connect_error)) {
            if (connect_error.find("interrupted") != std::string::npos) {
                result.error_message = "outbound client stopping";
                break;
            }
            continue;
        }

        connected_port = port;
        LOG_OUTBOUND_INFO("Outbound SMTP connected: outbox_id={}, host={}, port={}",
                        record.id,
                        target_host,
                        connected_port);

        if (port == 465) {
            boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls_stream(std::move(socket), ssl_ctx);
            std::string handshake_error;
            if (!smtp_transport::run_interruptible_ec(
                    tls_stream,
                    [&](auto handler) { tls_stream.async_handshake(boost::asio::ssl::stream_base::client, std::move(handler)); },
                    should_continue,
                    smtp_transport::kIoOperationTimeout,
                    "implicit TLS handshake",
                    handshake_error)) {
                if (handshake_error.find("interrupted") != std::string::npos) {
                    result.success = false;
                    result.error_message = "outbound client stopping";
                    break;
                }
                result.success = false;
                result.error_message = handshake_error;
                continue;
            }
            LOG_OUTBOUND_INFO("Outbound SMTP TLS handshake succeeded: outbox_id={}, host={}, port={}",
                            record.id,
                            target_host,
                            port);

            boost::asio::streambuf tls_buffer;
            result = execute_smtp_transaction(tls_stream,
                                              tls_buffer,
                                              record,
                                              hot_mail,
                                              helo_domain,
                                              envelope_sender,
                                              header_from,
                                              config,
                                              false,
                                              true,
                                              should_continue);
        } else {
            boost::asio::streambuf plain_buffer;
            result = execute_smtp_transaction(socket,
                                              plain_buffer,
                                              record,
                                              hot_mail,
                                              helo_domain,
                                              envelope_sender,
                                              header_from,
                                              config,
                                              true,
                                              true,
                                              should_continue);
            if (result.error_message == kStartTlsReadyToken) {
                if (should_continue && !should_continue()) {
                    result.success = false;
                    result.error_message = "outbound client stopping";
                    break;
                }

                boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls_stream(std::move(socket), ssl_ctx);
                std::string handshake_error;
                if (!smtp_transport::run_interruptible_ec(
                        tls_stream,
                        [&](auto handler) { tls_stream.async_handshake(boost::asio::ssl::stream_base::client, std::move(handler)); },
                        should_continue,
                        smtp_transport::kIoOperationTimeout,
                        "STARTTLS handshake",
                        handshake_error)) {
                    if (handshake_error.find("interrupted") != std::string::npos) {
                        result.success = false;
                        result.error_message = "outbound client stopping";
                        break;
                    }
                    result.success = false;
                    result.error_message = handshake_error;
                    continue;
                }
                LOG_OUTBOUND_INFO("Outbound SMTP TLS handshake succeeded: outbox_id={}, host={}, port={}",
                                record.id,
                                target_host,
                                port);

                boost::asio::streambuf tls_buffer;
                result = execute_smtp_transaction(tls_stream,
                                                  tls_buffer,
                                                  record,
                                                  hot_mail,
                                                  helo_domain,
                                                  envelope_sender,
                                                  header_from,
                                                  config,
                                                  false,
                                                  false,
                                                  should_continue);
            }
        }

        if (result.success || result.permanent_failure) {
            break;
        }
    }

    if (connected_port == 0) {
        result.error_message = "failed to connect all configured outbound ports";
        result.retry_delay_seconds = kDefaultRetryDelaySeconds;
        LOG_OUTBOUND_WARN("Outbound SMTP connect failed: outbox_id={}, host={}, ports=[{}]",
                        record.id,
                        target_host,
                        join_ports(config.ports));
        return result;
    }

    if (result.success) {
        LOG_OUTBOUND_INFO("Outbound SMTP success: outbox_id={}, recipient={}, response={}",
                        record.id,
                        record.recipient,
                        result.response);
    }
    return result;
}

}

SmtpOutboundClient::SmtpOutboundClient(std::shared_ptr<router::IShardRouter> shard_router,
                                       std::shared_ptr<ThreadPoolBase> io_thread_pool,
                                       std::shared_ptr<ThreadPoolBase> worker_thread_pool,
                                       std::shared_ptr<IDnsResolver> dns_resolver,
                                       std::shared_ptr<std::atomic<bool>> server_interrupt_flag,
                                       OutboundConfig config,
                                       std::string local_domain)
    : m_shardRouter(std::move(shard_router)),
      io_thread_pool_(std::move(io_thread_pool)),
      worker_thread_pool_(std::move(worker_thread_pool)),
      dns_resolver_(std::move(dns_resolver)),
      server_interrupt_flag_(std::move(server_interrupt_flag)),
      config_(std::move(config)),
      local_domain_(std::move(local_domain)) {
    if (config_.ports.empty())
        config_.ports.push_back(25);
    config_.max_attempts = std::max<size_t>(1, config_.max_attempts);

    std::ostringstream oss;
    oss << "outbound-worker-" << std::this_thread::get_id();
    worker_id_ = oss.str();

    if (m_shardRouter) {
        steal_shard_order_ = m_shardRouter->shard_priority_order();
        home_shard_ = steal_shard_order_.empty() ? 0 : steal_shard_order_.front();
    }

    config_.busy_sleep_ms   = std::max(1, config_.busy_sleep_ms);
    config_.backoff_base_ms = std::max(1, config_.backoff_base_ms);
    config_.backoff_max_ms  = std::max(config_.busy_sleep_ms, config_.backoff_max_ms);

    if (config_.dkim_enabled) {
        LOG_OUTBOUND_INFO("DKIM config loaded: selector={}, domain={}, key_file={}",
                          config_.dkim_selector, config_.dkim_domain,
                          config_.dkim_private_key_file);
    }
}

SmtpOutboundClient::~SmtpOutboundClient() {
    stop();
}

void SmtpOutboundClient::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    orchestrator_thread_ = std::thread([this]() { run_loop(); });
    std::ostringstream ports;
    for (std::size_t i = 0; i < config_.ports.size(); ++i) {
        ports << config_.ports[i];
        if (i + 1 < config_.ports.size()) {
            ports << ",";
        }
    }
    LOG_OUTBOUND_INFO("SmtpOutboundClient started, local_domain={}, config.ports=[{}], max_delivery_attempts={}",
                    local_domain_,
                    ports.str(),
                    config_.max_attempts);
}

void SmtpOutboundClient::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    notify_cv_.notify_all();
    if (orchestrator_thread_.joinable()) {
        orchestrator_thread_.join();
    }
    LOG_OUTBOUND_INFO("SmtpOutboundClient stopped");
}

bool SmtpOutboundClient::accept_mail_ownership(std::unique_ptr<mail> mail_ptr) {
    return accept_reserved_mail_ownership(std::move(mail_ptr), {});
}

bool SmtpOutboundClient::accept_reserved_mail_ownership(std::unique_ptr<mail> mail_ptr,
                                                        std::vector<OutboxRecord> reserved_records) {
    if (!mail_ptr) {
        return false;
    }

    if (!has_external_recipient(*mail_ptr, local_domain_)) {
        LOG_OUTBOUND_DEBUG("Outbound flow skip: mail_id={}, reason=no-external-recipient, local_domain={}",
                         mail_ptr->id,
                         local_domain_);
        return true;
    }

    if (!running_.load()) {
        LOG_OUTBOUND_WARN("Outbound flow rejected ownership because client is not running, mail_id={}", mail_ptr->id);
        return false;
    }

    return try_enqueue_hot_dispatch(mail_ptr, reserved_records);
}

void SmtpOutboundClient::notify_outbox_ready() {
    notify_cv_.notify_one();
}

int SmtpOutboundClient::local_reservation_lease_seconds() const {
    return kLeaseSeconds;
}

void SmtpOutboundClient::run_loop() {
    std::size_t empty_claim_rounds = 0;

    while (running_.load()) {
        drain_notifications();
        drain_completion_queue();

        // 回收所有 shard 的过期租约
        for (auto shard_idx : steal_shard_order_) {
            auto db = m_shardRouter->get_db_pool(shard_idx);
            if (db) repository_.requeue_expired_leases(*db);
        }

        const auto hot_dispatched = drain_hot_records();
        if (hot_dispatched > 0) {
            empty_claim_rounds = 0;
        } else {
            // 本地优先 → 按优先级顺序偷任务
            std::vector<OutboxRecord> records;
            for (auto shard_idx : steal_shard_order_) {
                auto db = m_shardRouter->get_db_pool(shard_idx);
                if (!db) continue;
                records = repository_.claim_batch(*db, worker_id_, kClaimBatchSize, kLeaseSeconds);
                if (!records.empty()) break;
            }
            if (!records.empty()) {
                empty_claim_rounds = 0;
                schedule_claimed_records(std::move(records));
            } else {
                ++empty_claim_rounds;
            }
        }

        std::unique_lock<std::mutex> lock(notify_mutex_);
        const auto wait_duration =
            compute_adaptive_wait(config_,
                                  empty_claim_rounds,
                                  !ownership_queue_.empty() || !hot_record_queue_.empty());
        notify_cv_.wait_for(lock,
                            wait_duration,
                            [this]() {
                                if (!running_.load() || !ownership_queue_.empty() || !hot_record_queue_.empty()) {
                                    return true;
                                }
                                std::lock_guard<std::mutex> completion_lock(completion_mutex_);
                                return !completion_queue_.empty();
                            });
    }

    drain_notifications();
    drain_completion_queue();
}

void SmtpOutboundClient::drain_notifications() {
    std::queue<HotMailDispatch> local_queue;
    {
        std::lock_guard<std::mutex> lock(notify_mutex_);
        std::swap(local_queue, ownership_queue_);
    }

    while (!local_queue.empty()) {
        auto dispatch = std::move(local_queue.front());
        local_queue.pop();

        if (!dispatch.mail_ptr) {
            continue;
        }
        auto hot_mail = std::shared_ptr<mail>(dispatch.mail_ptr.release());
        if (!hot_mail) {
            continue;
        }
        const auto mail_id = hot_mail->id;
        hot_mail_cache_[mail_id] = hot_mail;
        hot_mail_pending_dispatch_counts_[mail_id] += dispatch.reserved_records.size();
        for (auto& record : dispatch.reserved_records) {
            HotDeliveryTask task;
            task.record = std::move(record);
            task.mail_ctx = hot_mail;
            hot_record_queue_.push(std::move(task));
        }
    }
}

std::size_t SmtpOutboundClient::drain_hot_records() {
    std::size_t dispatched = 0;
    while (!hot_record_queue_.empty()) {
        auto task = std::move(hot_record_queue_.front());
        hot_record_queue_.pop();
        dispatch_delivery_task(task.record, std::move(task.mail_ctx));
        ++dispatched;
    }
    return dispatched;
}

void SmtpOutboundClient::schedule_claimed_records(std::vector<OutboxRecord> claimed_records) {
    if (claimed_records.empty()) {
        return;
    }

    std::unordered_map<std::uint64_t, std::vector<OutboxRecord>> grouped_records;
    std::vector<std::uint64_t> group_order;
    grouped_records.reserve(claimed_records.size());
    group_order.reserve(claimed_records.size());

    for (auto& record : claimed_records) {
        auto [it, inserted] = grouped_records.try_emplace(record.mail_id);
        if (inserted) {
            group_order.push_back(record.mail_id);
        }
        it->second.push_back(std::move(record));
    }

    for (auto mail_id : group_order) {
        auto it = grouped_records.find(mail_id);
        if (it == grouped_records.end()) {
            continue;
        }

        auto mail_ptr = repository_.load_mail(*m_shardRouter->get_db_pool(0), mail_id);
        if (!mail_ptr) {
            LOG_OUTBOUND_WARN("Outbound flow failed to hydrate mail from DB, fallback to direct dispatch, mail_id={}, claimed_records={}",
                              mail_id,
                              it->second.size());
            for (const auto& record : it->second) {
                dispatch_delivery_task(record);
            }
            continue;
        }

        if (!try_enqueue_hot_dispatch(mail_ptr, it->second)) {
            LOG_OUTBOUND_WARN("Outbound flow failed to enqueue claimed records as hot dispatch, fallback to direct dispatch, mail_id={}, claimed_records={}",
                              mail_id,
                              it->second.size());
            for (const auto& record : it->second) {
                dispatch_delivery_task(record);
            }
        }
    }
}

void SmtpOutboundClient::drain_completion_queue() {
    std::queue<DeliveryCompletion> local_queue;
    {
        std::lock_guard<std::mutex> lock(completion_mutex_);
        std::swap(local_queue, completion_queue_);
    }

    while (!local_queue.empty()) {
        auto completion = std::move(local_queue.front());
        local_queue.pop();

        // 指标: 投递结果计数
        if (auto m = metrics_.lock()) {
            MetricsServer::LabelMap lbls;
            if (completion.success)
                lbls["result"] = "success";
            else if (completion.permanent_failure)
                lbls["result"] = "perm_fail";
            else
                lbls["result"] = "temp_fail";
            m->inc_counter("protorelay_outbound_delivery_total", lbls);
        }

        bool ok = false;
        if (completion.success) {
            ok = repository_.mark_sent(*m_shardRouter->get_db_pool(0), completion.outbox_id, completion.smtp_response);
        } else if (completion.permanent_failure) {
            ok = repository_.mark_dead(*m_shardRouter->get_db_pool(0), completion.outbox_id, completion.error_message);
        } else {
            ok = repository_.mark_retry(*m_shardRouter->get_db_pool(0), completion.outbox_id,
                                       completion.error_message,
                                       completion.retry_delay_seconds);
        }

        if (!ok) {
            LOG_OUTBOUND_ERROR("SmtpOutboundClient: failed to persist outbox state, outbox_id={}, attempt_id={}",
                             completion.outbox_id,
                             completion.dispatch_attempt_id);
        }

        // 保守策略：当前实现在完成一次状态回写后释放缓存对象。
        auto cache_it = hot_mail_cache_.find(completion.mail_id);
        auto pending_it = hot_mail_pending_dispatch_counts_.find(completion.mail_id);
        if (cache_it != hot_mail_cache_.end()) {
            if (completion.success) {
                cache_it->second->status = 1;
            } else {
                cache_it->second->status = 2;
            }
        }
        if (pending_it != hot_mail_pending_dispatch_counts_.end()) {
            if (pending_it->second > 0) {
                --pending_it->second;
            }
            if (pending_it->second == 0) {
                hot_mail_pending_dispatch_counts_.erase(pending_it);
                if (cache_it != hot_mail_cache_.end()) {
                    hot_mail_cache_.erase(cache_it);
                }
            }
        } else if (cache_it != hot_mail_cache_.end()) {
            hot_mail_cache_.erase(cache_it);
        }
    }
}

bool SmtpOutboundClient::try_enqueue_hot_dispatch(std::unique_ptr<mail>& mail_ptr,
                                                  std::vector<OutboxRecord>& reserved_records) {
    if (!mail_ptr) {
        return false;
    }

    if (!running_.load()) {
        LOG_OUTBOUND_WARN("Outbound flow rejected hot dispatch because client is not running, mail_id={}", mail_ptr->id);
        return false;
    }

    (void)ensure_mail_raw_payload_loaded(*mail_ptr);

    const auto mail_id = mail_ptr->id;
    const auto record_count = reserved_records.size();
    {
        std::lock_guard<std::mutex> lock(notify_mutex_);
        HotMailDispatch dispatch;
        dispatch.mail_ptr = std::move(mail_ptr);
        dispatch.reserved_records = std::move(reserved_records);
        ownership_queue_.push(std::move(dispatch));
    }
    LOG_OUTBOUND_DEBUG("Outbound flow accepted hot dispatch: mail_id={}, reserved_records={}",
                       mail_id,
                       record_count);
    notify_cv_.notify_one();
    return true;
}

void SmtpOutboundClient::dispatch_delivery_task(const OutboxRecord& record,
                                                std::shared_ptr<mail> hot_mail_ctx) {
    const auto attempt_id = ++g_dispatch_attempt_seq;

    auto task = [this, record, attempt_id, hot_mail_ctx = std::move(hot_mail_ctx)]() mutable {
        const auto should_continue = [this]() {
            if (!running_.load()) {
                return false;
            }
            return !server_interrupt_flag_ || server_interrupt_flag_->load();
        };

        if (!should_continue()) {
            DeliveryCompletion completion;
            completion.outbox_id = record.id;
            completion.mail_id = record.mail_id;
            completion.dispatch_attempt_id = attempt_id;
            completion.success = false;
            completion.permanent_failure = false;
            completion.retry_delay_seconds = 5;
            completion.error_message = "outbound client stopping";
            push_completion(std::move(completion));
            return;
        }

        LOG_OUTBOUND_DEBUG("Outbound flow dispatch: mail_id={}, outbox_id={}, attempt_id={}, attempt_count={}",
                 record.mail_id,
                 record.id,
                 attempt_id,
                 record.attempt_count);

        // Resolve MX targets for recipient domain before SMTP delivery.
        auto target_hosts = build_target_hosts(record, dns_resolver_.get());
        LOG_OUTBOUND_DEBUG("Outbound DNS targets: outbox_id={}, recipient={}, hosts=[{}]",
                         record.id,
                         record.recipient,
                         [&target_hosts]() {
                             std::ostringstream oss;
                             for (std::size_t i = 0; i < target_hosts.size(); ++i) {
                                 oss << target_hosts[i];
                                 if (i + 1 < target_hosts.size()) {
                                     oss << ",";
                                 }
                             }
                             return oss.str();
                         }());

        if (target_hosts.empty()) {
            DeliveryCompletion completion;
            completion.outbox_id = record.id;
            completion.mail_id = record.mail_id;
            completion.dispatch_attempt_id = attempt_id;
            completion.success = false;
            completion.permanent_failure = false;
            completion.retry_delay_seconds = kDefaultRetryDelaySeconds;
            completion.error_message = "no routable SMTP target hosts resolved";
            LOG_OUTBOUND_WARN("Outbound SMTP failed: outbox_id={}, attempt_id={}, permanent_failure={}, error={}",
                            completion.outbox_id,
                            completion.dispatch_attempt_id,
                            completion.permanent_failure,
                            completion.error_message);
            push_completion(std::move(completion));
            return;
        }

        SmtpExecResult exec_result;
        bool delivered = false;
        for (const auto& host : target_hosts) {
            if (!should_continue()) {
                exec_result.success = false;
                exec_result.permanent_failure = false;
                exec_result.error_message = "outbound client stopping";
                break;
            }

            exec_result = run_plain_smtp_flow(record,
                                              hot_mail_ctx.get(),
                                              config_,
                                              host,
                                              should_continue);
            if (exec_result.success) {
                delivered = true;
                break;
            }
            if (exec_result.permanent_failure) {
                break;
            }
        }

        if (!delivered && exec_result.success) {
            exec_result.success = false;
            exec_result.error_message = "delivery status inconsistent";
        }

        DeliveryCompletion completion;
        completion.outbox_id = record.id;
        completion.mail_id = record.mail_id;
        completion.dispatch_attempt_id = attempt_id;
        completion.success = exec_result.success;
        completion.permanent_failure = exec_result.permanent_failure;
        completion.retry_delay_seconds = exec_result.retry_delay_seconds;
        completion.smtp_response = exec_result.response;
        completion.error_message = exec_result.error_message;

        if (!completion.success) {
            LOG_OUTBOUND_WARN("Outbound SMTP failed: outbox_id={}, attempt_id={}, permanent_failure={}, error={}",
                            completion.outbox_id,
                            completion.dispatch_attempt_id,
                            completion.permanent_failure,
                            completion.error_message);
        }
        push_completion(std::move(completion));
    };

    auto io_pool = std::dynamic_pointer_cast<IOThreadPool>(io_thread_pool_);
    if (io_pool && io_pool->is_running()) {
        boost::asio::post(io_pool->get_io_context(), std::move(task));
        return;
    }

    if (worker_thread_pool_ && worker_thread_pool_->is_running()) {
        worker_thread_pool_->post(std::move(task));
        return;
    }

    DeliveryCompletion completion;
    completion.outbox_id = record.id;
    completion.mail_id = record.mail_id;
    completion.dispatch_attempt_id = attempt_id;
    completion.success = false;
    completion.error_message = "No available thread pool for outbound delivery";
    completion.permanent_failure = false;
    completion.retry_delay_seconds = 30;
    push_completion(std::move(completion));
}

void SmtpOutboundClient::push_completion(DeliveryCompletion completion) {
    {
        std::lock_guard<std::mutex> lock(completion_mutex_);
        completion_queue_.push(std::move(completion));
    }
    notify_cv_.notify_one();
}

} // namespace outbound
} // namespace mail_system
