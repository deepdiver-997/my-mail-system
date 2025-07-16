#include "mail_system/back/db/mysql_pool.h"
#include <iostream>

namespace mail_system {

// 静态成员初始化
std::unique_ptr<MySQLPoolFactory> MySQLPoolFactory::s_instance = nullptr;
std::mutex MySQLPoolFactory::s_mutex;

// MySQLPool实现

MySQLPool::MySQLPool(const DBPoolConfig& config, std::shared_ptr<DBService> db_service)
    : m_config(config), m_dbService(db_service), m_running(true) {
    initialize_pool();
    m_maintenanceThread = std::thread(&MySQLPool::maintenance_thread, this);
}

MySQLPool::~MySQLPool() {
    close();
}

void MySQLPool::initialize_pool() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 创建初始连接
    for (size_t i = 0; i < m_config.initial_pool_size; ++i) {
        auto connection = create_connection();
        if (connection) {
            auto wrapper = std::make_shared<ConnectionWrapper>(connection);
            m_connections.push_back(wrapper);
            m_availableConnections.push(wrapper);
        }
    }
}

std::shared_ptr<IDBConnection> MySQLPool::create_connection() {
    return m_dbService->create_connection(
        m_config.host,
        m_config.user,
        m_config.password,
        m_config.database,
        m_config.port
    );
}

std::shared_ptr<IDBConnection> MySQLPool::get_connection() {
    std::unique_lock<std::mutex> lock(m_mutex);

    // 等待可用连接，最多等待连接超时时间
    auto timeout = std::chrono::seconds(m_config.connection_timeout);
    bool hasConnection = m_cv.wait_for(lock, timeout, [this] {
        return !m_availableConnections.empty() || !m_running;
    });

    if (!m_running) {
        return nullptr;
    }

    if (!hasConnection) {
        // 超时，检查是否可以创建新连接
        if (m_connections.size() < m_config.max_pool_size) {
            auto connection = create_connection();
            if (connection) {
                auto wrapper = std::make_shared<ConnectionWrapper>(connection);
                wrapper->in_use = true;
                m_connections.push_back(wrapper);
                return wrapper->connection;
            }
        }
        return nullptr;
    }

    // 获取可用连接
    auto wrapper = m_availableConnections.front();
    m_availableConnections.pop();
    wrapper->in_use = true;
    wrapper->last_used = std::chrono::steady_clock::now();

    // 验证连接是否有效
    if (!validate_connection(wrapper->connection)) {
        // 连接无效，创建新连接
        wrapper->connection = create_connection();
        if (!wrapper->connection) {
            // 无法创建新连接
            wrapper->in_use = false;
            return nullptr;
        }
    }

    return wrapper->connection;
}

void MySQLPool::release_connection(std::shared_ptr<IDBConnection> connection) {
    if (!connection) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // 查找连接包装器
    for (auto& wrapper : m_connections) {
        if (wrapper->connection == connection) {
            wrapper->in_use = false;
            wrapper->last_used = std::chrono::steady_clock::now();
            m_availableConnections.push(wrapper);
            m_cv.notify_one();
            break;
        }
    }
}

size_t MySQLPool::get_pool_size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connections.size();
}

size_t MySQLPool::get_available_connections() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_availableConnections.size();
}

void MySQLPool::close() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_running = false;
    }

    m_cv.notify_all();

    // 等待维护线程结束
    if (m_maintenanceThread.joinable()) {
        m_maintenanceThread.join();
    }

    // 关闭所有连接
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& wrapper : m_connections) {
        if (wrapper->connection) {
            wrapper->connection->disconnect();
        }
    }
    m_connections.clear();
    
    // 清空可用连接队列
    std::queue<std::shared_ptr<ConnectionWrapper>> empty;
    std::swap(m_availableConnections, empty);
}

void MySQLPool::maintenance_thread() {
    while (m_running) {
        // 每10秒检查一次空闲连接
        std::this_thread::sleep_for(std::chrono::seconds(10));
        cleanup_idle_connections();
    }
}

void MySQLPool::cleanup_idle_connections() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::steady_clock::now();
    auto idleTimeout = std::chrono::seconds(m_config.idle_timeout);

    // 保留至少初始连接数量的连接
    if (m_connections.size() <= m_config.initial_pool_size) {
        return;
    }

    // 检查并关闭空闲连接
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        auto& wrapper = *it;
        if (!wrapper->in_use && 
            (now - wrapper->last_used) > idleTimeout && 
            m_connections.size() > m_config.initial_pool_size) {
            
            // 从可用连接队列中移除
            std::queue<std::shared_ptr<ConnectionWrapper>> tempQueue;
            while (!m_availableConnections.empty()) {
                auto conn = m_availableConnections.front();
                m_availableConnections.pop();
                if (conn != wrapper) {
                    tempQueue.push(conn);
                }
            }
            m_availableConnections = std::move(tempQueue);

            // 断开连接并从连接池中移除
            wrapper->connection->disconnect();
            it = m_connections.erase(it);
        } else {
            ++it;
        }
    }
}

bool MySQLPool::validate_connection(std::shared_ptr<IDBConnection> connection) {
    if (!connection->is_connected()) {
        return false;
    }

    // 执行简单查询来验证连接
    try {
        auto result = connection->query("SELECT 1");
        return result != nullptr;
    } catch (...) {
        return false;
    }
}

// MySQLPoolFactory实现

std::shared_ptr<DBPool> MySQLPoolFactory::create_pool(
    const DBPoolConfig& config,
    std::shared_ptr<DBService> db_service
) {
    return std::make_shared<MySQLPool>(config, db_service);
}

MySQLPoolFactory& MySQLPoolFactory::get_instance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_instance) {
        s_instance = std::unique_ptr<MySQLPoolFactory>(new MySQLPoolFactory());
    }
    return *s_instance;
}

} // namespace mail_system