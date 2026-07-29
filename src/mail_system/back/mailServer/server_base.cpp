#include "mail_system/back/mailServer/server_base.h"
#include "mail_system/back/outbound/cares_dns_resolver.h"
#include "mail_system/back/router/hash_shard_router.h"
#include "mail_system/back/router/static_shard_router.h"
#include "mail_system/back/router/table_shard_router.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/db/distributed_mysql_pool.h"
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
      ssl_in_worker(false),
      m_domain(config.system_domain.empty() ? std::string("example.com") : config.system_domain),
      m_config(std::make_shared<ServerConfig>(config)),
      m_sslContext(boost::asio::ssl::context::sslv23),
      has_listener_thread(false),
      m_state(ServerState::Stopped) {
    auto cfg = std::atomic_load(&m_config);
    try {
        Logger::get_instance().init(
            config.log_file, 1024 * 1024 * 5, 3,
            Logger::string_to_level(config.log_level),
            config.log_to_console, config.log_to_file);
        LOG_SERVER_INFO("Logger initialized with level: {}", config.log_level);

        // ---- 1. 创建 DB 池 ----
        auto main_db_pool = dbPool;  // 可能从外部注入
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
            LOG_SERVER_INFO("Null database pool created (use_database=false or no DB configured)");
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
                sc.hdfs.replication, static_cast<long>(sc.hdfs.timeout_ms));
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
                sc.s3.use_path_style);
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
            std::string err;
            if (!main_storage->ensure_ready(err))
                throw std::runtime_error("Storage init failed: " + err);
        }

        // ---- 3. 创建 Shard Router（包装 DB 池和存储） ----
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

        // io context
        m_ioContext = std::make_shared<boost::asio::io_context>();
        m_resolver = std::make_shared<boost::asio::ip::tcp::resolver>(*m_ioContext);
        m_workGuard = std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(m_ioContext->get_executor());

        auto addr = boost::asio::ip::make_address(config.address);

        // SSL context (only if any SSL listener)
        bool has_ssl = false;
        for (auto& l : config.listeners)
            if (l.type == ListenerType::SSL) { has_ssl = true; break; }

        if (has_ssl) {
            m_sslContext.set_options(
                boost::asio::ssl::context::default_workarounds |
                boost::asio::ssl::context::no_sslv2 |
                boost::asio::ssl::context::no_sslv3 |
                boost::asio::ssl::context::single_dh_use);
            load_certificates(config.certFile, config.keyFile, config.dhFile);
        }

        // create acceptors from listener configs
        for (auto& l : config.listeners) {
            m_listener_configs[l.port] = l;
            auto acc = std::make_shared<boost::asio::ip::tcp::acceptor>(*m_ioContext);
            acc->open(boost::asio::ip::tcp::v4());
            acc->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            acc->bind(boost::asio::ip::tcp::endpoint(addr, l.port));
            acc->listen();
            if (l.type == ListenerType::SSL)
                m_ssl_acceptors.push_back(acc);
            else
                m_tcp_acceptors.push_back(acc);
            LOG_SERVER_INFO("{} acceptor on {}:{}",
                           listener_type_to_string(l.type), config.address, l.port);
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
    // stop() 已 join listener 线程；若未 join 则等待（最多 3 秒）
    if (m_listenerThread.joinable()) {
        try { m_listenerThread.join(); } catch (...) {}
    }
}

const ListenerConfig* ServerBase::get_listener_config(uint16_t port) const {
    auto it = m_listener_configs.find(port);
    return it != m_listener_configs.end() ? &it->second : nullptr;
}

void ServerBase::handoff_starttls_socket(std::unique_ptr<boost::asio::ip::tcp::socket>&& socket) {
    if (!socket) return;
    auto ssl_stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
        std::move(*socket), get_ssl_context());
    uint16_t port = ssl_stream->next_layer().local_endpoint().port();
    const ListenerConfig* lc = get_listener_config(port);
    pass_stream(std::move(ssl_stream), lc ? *lc : ListenerConfig{});
}

// ---------------------------------------------------------------------------
// Per-acceptor SSL accept
// ---------------------------------------------------------------------------
void ServerBase::do_ssl_accept(
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor,
    const ListenerConfig& lc)
{
    if (get_state() != ServerState::Running) return;

    auto sock = std::make_unique<boost::asio::ip::tcp::socket>(
        std::static_pointer_cast<IOThreadPool>(m_ioThreadPool)->get_io_context());
    auto ssl_sock = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
        std::move(*sock), get_ssl_context());
    auto& lowest = ssl_sock->next_layer();

    acceptor->async_accept(lowest, [this, acceptor, lc,
        ssl_sock = std::move(ssl_sock)](const boost::system::error_code& ec) mutable {
        if (!ec) {
            auto ip = ssl_sock->next_layer().remote_endpoint().address().to_string();
            if (is_ip_banned(ip)) {
                increment_connections_rejected();
                boost::system::error_code ign;
                ssl_sock->next_layer().close(ign);
            } else {
                std::string reason;
                if (should_reject_connection(reason, ip)) {
                    increment_connections_rejected();
                    boost::system::error_code ign;
                    ssl_sock->next_layer().close(ign);
                } else {
                    increment_connections_total();
                    handle_accept(std::move(ssl_sock), ec, lc);
                }
            }
        }
        do_ssl_accept(acceptor, lc);
    });
}

void ServerBase::start_all_ssl_acceptors() {
    for (auto& acc : m_ssl_acceptors) {
        uint16_t port = acc->local_endpoint().port();
        do_ssl_accept(acc, m_listener_configs[port]);
    }
}

// ---------------------------------------------------------------------------
// Per-acceptor TCP accept
// ---------------------------------------------------------------------------
void ServerBase::do_tcp_accept(
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor,
    const ListenerConfig& lc)
{
    if (get_state() != ServerState::Running) return;

    auto sock = std::make_unique<boost::asio::ip::tcp::socket>(
        std::static_pointer_cast<IOThreadPool>(m_ioThreadPool)->get_io_context());
    auto* psock = sock.get();

    acceptor->async_accept(*psock, [this, acceptor, lc,
        sock = std::move(sock)](const boost::system::error_code& ec) mutable {
        if (!ec) {
            auto ip = sock->remote_endpoint().address().to_string();
            if (is_ip_banned(ip)) {
                LOG_NETWORK_WARN("Banned IP {} rejected at accept", ip);
                increment_connections_rejected();
                boost::system::error_code ign;
                sock->close(ign);
            } else {
                std::string reason;
                if (should_reject_connection(reason, ip)) {
                    LOG_NETWORK_WARN("Rejecting TCP connection from {}: {}", ip, reason);
                    increment_connections_rejected();
                    boost::system::error_code ign;
                    sock->close(ign);
                } else {
                    LOG_NETWORK_INFO("New TCP connection accepted from {}", ip);
                    increment_connections_total();
                    handle_tcp_accept(std::move(sock), ec, lc);
                }
            }
        }
        do_tcp_accept(acceptor, lc);
    });
}

void ServerBase::start_all_tcp_acceptors() {
    for (auto& acc : m_tcp_acceptors) {
        uint16_t port = acc->local_endpoint().port();
        do_tcp_accept(acc, m_listener_configs[port]);
    }
}

// ====================================================================
// start — 非阻塞启动所有子系统
//  1. 恢复入侵检测器状态
//  2. 状态切换为 Running
//  3. 启动 metrics HTTP server
//  4. 创建 listener 线程: 注册 async_accept → io_context::run()
// ====================================================================
void ServerBase::start() {
    if (m_state.load() == ServerState::Running) return;

    {
        auto cfg = std::atomic_load(&m_config);
        m_intrusionDetector.set_enabled(cfg->intrusion_detection_enabled);
        m_intrusionDetector.set_max_records(static_cast<size_t>(cfg->intrusion_max_records));
        m_intrusionDetector.set_ban_threshold(cfg->intrusion_ban_threshold);
        m_intrusionDetector.set_persist_interval(cfg->intrusion_persist_interval_sec);
        m_intrusionDetector.set_persist_dirty_threshold(cfg->intrusion_persist_dirty_threshold);
    }
    m_intrusionDetector.restore();

    if (m_state.load() != ServerState::Stopped) {
        m_state.store(ServerState::Running);
        start_metrics_server();

        try {
            if (!has_listener_thread) {
                m_listenerThread = std::thread([this]() {
                    start_all_ssl_acceptors();
                    start_all_tcp_acceptors();
                    m_ioContext->run();
                });
                has_listener_thread = true;
                // m_listenerThread.detach();
            } else {
                boost::asio::post(*m_ioContext, [this]() {
                    start_all_ssl_acceptors();
                    start_all_tcp_acceptors();
                });
            }
            LOG_SERVER_INFO("Server started");
        } catch (const std::exception& e) {
            LOG_SERVER_ERROR("Error starting server: {}", e.what());
            stop();
        }
    }
}

// ====================================================================
// run — 阻塞当前线程直到收到停止信号
//  信号处理器调用 request_stop() 将状态置为 Pausing 后，此函数返回。
//  返回后外部 shared_ptr 析构 → stop() → RAII 清理所有资源。
// ====================================================================
void ServerBase::run() {
    while (m_state.load() == ServerState::Running || m_state.load() == ServerState::Paused)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ====================================================================
// stop — 停止服务器并释放资源 (protected, 仅从 ~ServerBase 调用)
//
//  停止顺序（保证资源逆序释放）:
//   1. work_guard.reset() — 移除 io_context 保活
//   2. acceptor->close()  — 取消所有挂起的 async_accept
//   3. io_context->stop() — 强制 listener 线程的 run() 返回
//   4. listenerThread.join() — 等待线程结束
//   5. stop_metrics_server() / outbound / persistent_queue — 停业务子系统
//   6. io_thread_pool / worker_thread_pool — 最后停线程池
//   7. intrusion_detector.persist() — 持久化 IP 封禁记录
//
//  外部应使用 request_stop() + 析构，不直接调用此函数。
// ====================================================================
void ServerBase::stop(ServerState next_state) {
    if (next_state == ServerState::Running) {
        LOG_SERVER_WARN("Can't switch to running in stop()");
        return;
    }
    auto cur = m_state.load();
    if (cur != ServerState::Running && cur != ServerState::Pausing) return;
    try {
        m_state.store(next_state);

        // 1. 释放 work guard → io_context::run() 可退出
        m_workGuard.reset();

        // 2. 停 io_context → run() 返回 → join 线程
        if (m_ioContext && !m_ioContext->stopped()) m_ioContext->stop();
        if (m_listenerThread.joinable()) m_listenerThread.join();
        LOG_SERVER_INFO("Listener thread stopped");

        // 3. 线程已退出，安全关闭 acceptor
        boost::system::error_code ec;
        for (auto& a : m_ssl_acceptors) a->close(ec);
        for (auto& a : m_tcp_acceptors) a->close(ec);

        stop_metrics_server();

        if (m_ioThreadPool) m_ioThreadPool->stop();
        if (m_workerThreadPool) m_workerThreadPool->stop();
        LOG_SERVER_INFO("ThreadPools stopped");

        m_intrusionDetector.persist();
        LOG_SERVER_INFO("Server stopped");
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error stopping server: {}", e.what());
    }
}

ServerState ServerBase::get_state() const { return m_state.load(); }
std::shared_ptr<boost::asio::io_context> ServerBase::get_io_context() { return m_ioContext; }
boost::asio::ssl::context& ServerBase::get_ssl_context() { return m_sslContext; }

void ServerBase::load_certificates(const std::string& cert_file, const std::string& key_file, const std::string& dh_file) {
    try {
        if (!std::ifstream(cert_file.c_str()).good())
            throw std::runtime_error("Certificate file not found: " + cert_file);
        if (!std::ifstream(key_file.c_str()).good())
            throw std::runtime_error("Private key file not found: " + key_file);
        m_sslContext.use_certificate_chain_file(cert_file);
        m_sslContext.use_private_key_file(key_file, boost::asio::ssl::context::pem);
        if (!dh_file.empty() && std::ifstream(dh_file.c_str()).good()) {
            m_sslContext.use_tmp_dh_file(dh_file);
        }
        LOG_SERVER_INFO("SSL certificates loaded successfully");
    } catch (const std::exception& e) {
        LOG_SERVER_ERROR("Error loading SSL certificates: {}", e.what());
        throw;
    }
}

void ServerBase::start_metrics_server() {
    auto cfg = std::atomic_load(&m_config);
    if (!cfg->metrics_enabled || m_metricsServer) return;
    try {
        m_metricsServer = std::make_shared<MetricsServer>(
            *m_ioContext, cfg->metrics_port, cfg->metrics_bind_address,
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

    // 监听器
    for (auto& [port, lc] : m_listener_configs) {
        m_metricsServer->set_gauge("protorelay_listener_info",
            {{"port", std::to_string(port)}, {"type", listener_type_to_string(lc.type)}}, 1);
    }

    // Shard Router
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

} // namespace mail_system
