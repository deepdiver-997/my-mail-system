#include "framework/server_base.h"
#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/router/hash_shard_router.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/router/table_shard_router.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/db/distributed_mysql_pool.h"
#include "mail_system/back/storage/async_storage_provider.h"
#include "mail_system/back/storage/distributed_file_storage_provider.h"
#if PROTORELAY_ENABLE_HDFS_WEB_STORAGE
#include "mail_system/back/storage/hdfs_web_storage_provider.h"
#endif
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mail_system/back/storage/null_storage_provider.h"
#ifdef ENABLE_S3_STORAGE
#include "mail_system/back/storage/s3_storage_provider.h"
#endif
#include "mail_system/back/db/null_db_pool.h"
#include <iostream>
#include <fstream>

namespace mail_system {

ServerBase::ServerBase(const ServerConfig& config,
     std::shared_ptr<ThreadPoolBase> ioThreadPool,
      std::shared_ptr<ThreadPoolBase> wokerThreadPool,
       std::shared_ptr<DBPool> dbPool)
    : m_ioThreadPool(ioThreadPool),
      m_workerThreadPool(wokerThreadPool),
      m_domain(config.system_domain.empty() ? std::string("example.com") : config.system_domain),
      m_config(std::make_shared<ServerConfig>(config))
{
    auto cfg = std::atomic_load(&m_config);
    try {
        Logger::get_instance().init(
            config.log_file, 1024 * 1024 * 5, 3,
            Logger::string_to_level(config.log_level),
            config.log_to_console, config.log_to_file);
        LOG_SERVER_INFO("Logger initialized with level: {}", config.log_level);

        // ---- 1. 创建 DB 池 ----
        auto main_db_pool = dbPool;
        if (config.use_database && main_db_pool == nullptr) {
            if (config.db_pool_config.achieve.find("mysql") == 0) {
                try {
                    if (config.db_pool_config.achieve == "mysql_distributed")
                        main_db_pool = DistributedMySQLPoolFactory::get_instance().create_pool(
                            config.db_pool_config, std::make_shared<MySQLService>());
                    else
                        main_db_pool = MySQLPoolFactory::get_instance().create_pool(
                            config.db_pool_config, std::make_shared<MySQLService>());
                } catch (const std::exception& e) {
                    LOG_SERVER_ERROR("Failed to create MySQL pool: {}", e.what());
                }
            }
        }
        if (!main_db_pool) {
            main_db_pool = std::make_shared<NullDBPool>();
            LOG_SERVER_INFO("Null database pool created");
        } else {
            LOG_SERVER_INFO("Database pool initialized successfully");
        }

        // ---- 2. 创建存储 ----
        const auto& sc = config.storage;
        std::shared_ptr<storage::IStorageProvider> main_storage;
        switch (storage::provider_type_from_string(sc.provider)) {
        case storage::StorageProviderType::Distributed:
            main_storage = std::make_shared<storage::DistributedFileStorageProvider>(
                sc.distributed.roots, sc.distributed.replica_count);
            break;
        case storage::StorageProviderType::HdfsWeb:
#if PROTORELAY_ENABLE_HDFS_WEB_STORAGE
            main_storage = std::make_shared<storage::HdfsWebStorageProvider>(
                sc.hdfs.endpoint, sc.hdfs.base_path, sc.hdfs.user,
                sc.hdfs.replication, static_cast<long>(sc.hdfs.timeout_ms),
                sc.hdfs.max_write_buffer_bytes);
            break;
#else
            throw std::runtime_error("hdfs_web requires ENABLE_HDFS_WEB_STORAGE=ON");
#endif
        case storage::StorageProviderType::Null:
            main_storage = std::make_shared<storage::NullStorageProvider>();
            break;
        case storage::StorageProviderType::S3:
#ifdef ENABLE_S3_STORAGE
            main_storage = std::make_shared<storage::S3StorageProvider>(
                sc.s3.endpoint, sc.s3.bucket,
                sc.s3.access_key, sc.s3.secret_key,
                sc.s3.region, static_cast<long>(sc.s3.timeout_ms),
                sc.s3.use_path_style, sc.s3.max_write_buffer_bytes);
            break;
#else
            throw std::runtime_error("s3 requires ENABLE_S3_STORAGE=ON");
#endif
        case storage::StorageProviderType::Local:
        default:
            if (!std::filesystem::exists(sc.local.mail_path))
                std::filesystem::create_directories(sc.local.mail_path);
            if (!std::filesystem::exists(sc.local.attachment_path))
                std::filesystem::create_directories(sc.local.attachment_path);
            main_storage = std::make_shared<storage::LocalFileStorageProvider>(
                sc.local.mail_path, sc.local.attachment_path);
            break;
        }
        {
            storage::IoError err;
            if (!main_storage->ensure_ready(err))
                throw std::runtime_error("Storage init failed: " + err.message);
        }

        // ---- 2b. 远程后端套真异步装饰器 ----
        // S3/WebHDFS 的 PUT/GET 是毫秒~百毫秒级阻塞网络操作：commit_async 与
        // 三个 async 读投递到 worker 线程池执行，io 线程不再被卡（调用点已按
        // pause 独占约定改造）。本地后端保持内联——stat/mmap/进 page cache 的
        // 写都是 µs 级，多一次线程跳转纯属浪费。
        // executor 惰性取 pool：装配此刻 worker 池尚未创建（见 ---- 5 ----），
        // 首次存储操作远晚于初始化完成，届时必然已就绪；无池配置内联兜底。
        if (sc.provider == "s3" || sc.provider == "hdfs_web") {
            main_storage = std::make_shared<storage::AsyncStorageProvider>(
                main_storage,
                [this](std::function<void()> task) {
                    if (m_workerThreadPool) {
                        m_workerThreadPool->post(std::move(task));
                    } else {
                        task();
                    }
                });
        }

        // ---- 3. 创建 Shard Router ----
        auto& rc = config.router_config;
        std::vector<std::shared_ptr<DBPool>> shard_db_pools;
        std::vector<std::shared_ptr<storage::IStorageProvider>> shard_storages;

        if (!rc.shards.empty()) {
            for (size_t i = 0; i < rc.shards.size() && i < rc.shard_count; ++i) {
                auto& se = rc.shards[i];
                std::shared_ptr<DBPool> sdb;
                if (!se.db_config_file.empty()) {
                    DBPoolConfig scfg;
                    if (scfg.loadFromJson(se.db_config_file)) {
                        if (scfg.achieve == "mysql_distributed")
                            sdb = DistributedMySQLPoolFactory::get_instance().create_pool(
                                scfg, std::make_shared<MySQLService>());
                        else
                            sdb = MySQLPoolFactory::get_instance().create_pool(
                                scfg, std::make_shared<MySQLService>());
                    }
                }
                shard_db_pools.push_back(sdb ? sdb : main_db_pool);

                std::shared_ptr<storage::IStorageProvider> sst;
                if (!se.storage_root.empty()) {
                    auto mail_dir = se.storage_root + "/mail";
                    auto att_dir  = se.storage_root + "/attachments";
                    if (!std::filesystem::exists(mail_dir))
                        std::filesystem::create_directories(mail_dir);
                    if (!std::filesystem::exists(att_dir))
                        std::filesystem::create_directories(att_dir);
                    sst = std::make_shared<storage::LocalFileStorageProvider>(mail_dir, att_dir);
                }
                shard_storages.push_back(sst ? sst : main_storage);
            }
        } else {
            shard_db_pools = {main_db_pool};
            shard_storages = {main_storage};
        }

        if (rc.type == "table") {
            if (!main_db_pool)
                throw std::runtime_error("table router requires a database pool");
            m_shardRouter = std::make_shared<router::TableShardRouter>(
                main_db_pool,
                rc.table_name, rc.email_column, rc.shard_column,
                rc.shard_count, rc.cache_capacity,
                shard_db_pools, shard_storages);
        } else if (rc.type == "static") {
            m_shardRouter = std::make_shared<router::StaticShardRouter>(
                rc.static_mappings, rc.default_shard,
                shard_db_pools, shard_storages);
        } else {
            m_shardRouter = std::make_shared<router::HashShardRouter>(
                rc.shard_count, shard_db_pools, shard_storages);
        }
        LOG_SERVER_INFO("Shard router initialized: type={} shard_count={}",
                        m_shardRouter->name(), m_shardRouter->shard_count());

        // ---- 4. 线程池 ----
        if (config.io_thread_count > 0 && m_ioThreadPool == nullptr) {
            m_ioThreadPool = std::make_shared<IOThreadPool>(config.io_thread_count);
            m_ioThreadPool->start();
        }
        if (config.worker_thread_count > 0 && m_workerThreadPool == nullptr) {
            m_workerThreadPool = std::make_shared<BoostThreadPool>(config.worker_thread_count);
            m_workerThreadPool->start();
        }

        LOG_SERVER_INFO("Server initialized with {} listener(s)", config.listeners.size());
        m_state = ServerState::Paused;
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error initializing server: {}", e.what());
        throw;
    }
}

ServerBase::~ServerBase() {
    try { stop(); } catch (...) {}
}

void ServerBase::run() {
    while (m_state.load() == ServerState::Running || m_state.load() == ServerState::Paused)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void ServerBase::stop(ServerState state) {
    auto cur = m_state.load();
    if (cur != ServerState::Running && cur != ServerState::Pausing) return;
    m_state.store(state);

    stop_metrics_server();

    if (m_ioThreadPool) m_ioThreadPool->stop();
    if (m_workerThreadPool) m_workerThreadPool->stop();
    LOG_SERVER_INFO("ThreadPools stopped");

    m_intrusionDetector.persist();
    LOG_SERVER_INFO("Server stopped");
}

ServerState ServerBase::get_state() const { return m_state.load(); }

void ServerBase::start_metrics_server() {
    auto cfg = std::atomic_load(&m_config);
    if (!cfg->metrics_enabled || m_metricsServer) return;
    try {
        auto& io_ctx = std::static_pointer_cast<IOThreadPool>(m_ioThreadPool)->get_io_context();
        m_metricsServer = std::make_shared<MetricsServer>(
            io_ctx,
            cfg->metrics_port, cfg->metrics_bind_address,
            [this]() -> std::string {
                if (m_configFilePath.empty()) return "config file path not set";
                return reload_config(m_configFilePath) ? "OK" : "reload failed";
            },
            [this]() { refresh_metrics(); });
        m_metricsServer->start();
        refresh_metrics();
        LOG_SERVER_INFO("Metrics server started on {}:{}",
                        cfg->metrics_bind_address, cfg->metrics_port);
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Failed to start metrics server: {}", e.what());
        m_metricsServer.reset();
    }
}

void ServerBase::stop_metrics_server() {
    if (m_metricsServer) { m_metricsServer->stop(); m_metricsServer.reset(); }
}

void ServerBase::push_metric_gauge(const std::string& name,
                                     const MetricsServer::LabelMap& labels, double v) {
    if (auto m = m_metricsServer) m->set_gauge(name, labels, v);
}
void ServerBase::push_metric_counter(const std::string& name,
                                       const MetricsServer::LabelMap& labels, uint64_t v) {
    if (auto m = m_metricsServer) m->inc_counter(name, labels, v);
}
void ServerBase::push_metric_observe(const std::string& name,
                                       const MetricsServer::LabelMap& labels, double v) {
    if (auto m = m_metricsServer) m->observe(name, labels, v);
}

void ServerBase::increment_connection_count() {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    push_metric_gauge("protorelay_active_connections", {}, active_connections_.load());
}
void ServerBase::decrement_connection_count() {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
    push_metric_gauge("protorelay_active_connections", {}, active_connections_.load());
}
void ServerBase::increment_connections_total() {
    auto v = connections_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    push_metric_counter("protorelay_connections_total", {}, v);
}
void ServerBase::increment_connections_rejected() {
    auto v = connections_rejected_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    push_metric_counter("protorelay_connections_rejected_total", {}, v);
}
void ServerBase::increment_mails_accepted() {
    auto v = mails_accepted_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    push_metric_counter("protorelay_mails_accepted_total", {}, v);
}

void ServerBase::refresh_metrics() {
    if (!m_metricsServer) return;
    if (m_shardRouter) {
        size_t n = m_shardRouter->shard_count();
        for (size_t i = 0; i < n; ++i) {
            MetricsServer::LabelMap labels{{"shard", std::to_string(i)}};
            auto db = m_shardRouter->get_db_pool(i);
            if (db) {
                m_metricsServer->set_gauge("protorelay_db_pool_size", labels, db->get_pool_size());
                m_metricsServer->set_gauge("protorelay_db_available", labels, db->get_available_connections());
                m_metricsServer->set_gauge("protorelay_db_active", labels, db->get_active_connections());
                m_metricsServer->set_gauge("protorelay_db_pool_max", labels, db->get_max_pool_size());
            }
            auto st = m_shardRouter->get_storage(i);
            m_metricsServer->set_gauge("protorelay_storage_ready", labels, st ? 1 : 0);
        }
    }
}

bool ServerBase::reload_config(const std::string& json_file) {
    m_configFilePath = json_file;
    ServerConfig new_cfg = *std::atomic_load(&m_config);
    if (!new_cfg.loadFromFile(json_file)) {
        LOG_SERVER_ERROR("Config reload failed: could not parse {}", json_file);
        return false;
    }
    auto old_cfg = std::atomic_load(&m_config);
    if (old_cfg->address != new_cfg.address ||
        old_cfg->listeners.size() != new_cfg.listeners.size()) {
        LOG_SERVER_WARN("Config reload rejected: structural fields changed (requires restart)");
        return false;
    }
    for (size_t i = 0; i < old_cfg->listeners.size(); ++i) {
        if (old_cfg->listeners[i].port != new_cfg.listeners[i].port ||
            old_cfg->listeners[i].type != new_cfg.listeners[i].type) {
            LOG_SERVER_WARN("Config reload rejected: listener structure changed");
            return false;
        }
    }
    std::atomic_store(&m_config, std::make_shared<ServerConfig>(new_cfg));
    auto applied = std::atomic_load(&m_config);
    Logger::get_instance().set_level(Logger::string_to_level(applied->log_level));
    LOG_SERVER_INFO("Config reloaded: log_level={}, auth={}",
                    applied->log_level, inbound_auth_policy_to_string(applied->inbound_auth_policy));
    return true;
}

// deprecated stubs
std::string ServerBase::build_metrics_response() const { return ""; }
std::string ServerBase::build_status_response() const { return ""; }

} // namespace mail_system
