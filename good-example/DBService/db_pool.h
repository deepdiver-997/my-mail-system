#ifndef MAIL_SYSTEM_DB_POOL_H
#define MAIL_SYSTEM_DB_POOL_H

#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <stdexcept>

namespace mail_system {

// 数据库连接接口
class DBConnection {
public:
    virtual ~DBConnection() = default;
    
    // 执行SQL查询
    virtual bool executeQuery(const std::string& query) = 0;
    
    // 执行SQL查询并返回结果
    virtual bool executeQuery(const std::string& query, std::function<void(void*)> resultHandler) = 0;
    
    // 开始事务
    virtual bool beginTransaction() = 0;
    
    // 提交事务
    virtual bool commitTransaction() = 0;
    
    // 回滚事务
    virtual bool rollbackTransaction() = 0;
    
    // 检查连接是否有效
    virtual bool isValid() = 0;
    
    // 重新连接
    virtual bool reconnect() = 0;
};

// 数据库连接工厂接口
class DBConnectionFactory {
public:
    virtual ~DBConnectionFactory() = default;
    
    // 创建数据库连接
    virtual std::unique_ptr<DBConnection> createConnection(const std::string& connection_string) = 0;
};

// 数据库连接池
class DBPool {
public:
    explicit DBPool(const std::string& connection_string, size_t pool_size = 10)
        : connection_string_(connection_string)
        , pool_size_(pool_size)
        , initialized_(false)
    {}
    
    ~DBPool() {
        shutdown();
    }
    
    // 初始化连接池
    void initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (initialized_) {
            return;
        }
        
        for (size_t i = 0; i < pool_size_; ++i) {
            auto connection = createConnection();
            if (connection) {
                available_connections_.push_back(std::move(connection));
            }
        }
        
        initialized_ = true;
    }
    
    // 获取连接
    std::unique_ptr<DBConnection> getConnection() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (!initialized_) {
            throw std::runtime_error("Database pool not initialized");
        }
        
        // 等待可用连接
        condition_.wait(lock, [this]() {
            return !available_connections_.empty() || !initialized_;
        });
        
        if (!initialized_) {
            throw std::runtime_error("Database pool is shutting down");
        }
        
        // 获取连接
        auto connection = std::move(available_connections_.back());
        available_connections_.pop_back();
        
        // 检查连接是否有效，如果无效则重新连接
        if (!connection->isValid()) {
            if (!connection->reconnect()) {
                // 重新连接失败，创建新连接
                connection = createConnection();
                if (!connection) {
                    throw std::runtime_error("Failed to create database connection");
                }
            }
        }
        
        return connection;
    }
    
    // 释放连接
    void releaseConnection(std::unique_ptr<DBConnection> connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!initialized_) {
            return;
        }
        
        if (connection && connection->isValid()) {
            available_connections_.push_back(std::move(connection));
            condition_.notify_one();
        }
    }
    
    // 关闭连接池
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!initialized_) {
            return;
        }
        
        available_connections_.clear();
        initialized_ = false;
        condition_.notify_all();
    }
    
    // 设置连接工厂
    void setConnectionFactory(std::unique_ptr<DBConnectionFactory> factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_factory_ = std::move(factory);
    }
    
    // 获取连接池大小
    size_t getPoolSize() const {
        return pool_size_;
    }
    
    // 获取可用连接数
    size_t getAvailableConnectionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_connections_.size();
    }
    
private:
    // 创建新连接
    std::unique_ptr<DBConnection> createConnection() {
        if (connection_factory_) {
            return connection_factory_->createConnection(connection_string_);
        }
        
        // 默认实现（可以根据连接字符串选择不同的数据库驱动）
        // TODO: 实现默认的数据库连接创建逻辑
        return nullptr;
    }
    
private:
    std::string connection_string_;
    size_t pool_size_;
    bool initialized_;
    std::vector<std::unique_ptr<DBConnection>> available_connections_;
    std::unique_ptr<DBConnectionFactory> connection_factory_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_DB_POOL_H