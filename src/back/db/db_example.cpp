#include "mail_system/back/db/mysql_service.h"
#include "mail_system/back/db/mysql_pool.h"
#include <iostream>
#include <memory>

using namespace mail_system;

// 示例：如何使用数据库服务和连接池
void db_example() {
    try {
        // 1. 获取MySQL服务实例
        auto& dbService = MySQLService::get_instance();
        
        // 2. 创建数据库连接池配置
        DBPoolConfig config;
        config.host = "localhost";
        config.user = "username";
        config.password = "password";
        config.database = "mail_system";
        config.port = 3306;
        config.initial_pool_size = 5;
        config.max_pool_size = 10;
        config.connection_timeout = 5;
        config.idle_timeout = 60;
        
        // 3. 创建连接池
        auto& poolFactory = MySQLPoolFactory::get_instance();
        auto dbPool = poolFactory.create_pool(config, std::shared_ptr<DBService>(&dbService, [](DBService*){}));
        
        // 4. 获取数据库连接
        auto connection = dbPool->get_connection();
        if (!connection) {
            std::cerr << "Failed to get database connection" << std::endl;
            return;
        }
        
        // 5. 执行查询
        std::string sql = "SELECT * FROM users LIMIT 10";
        auto result = connection->query(sql);
        
        if (result) {
            // 6. 处理查询结果
            std::cout << "Total rows: " << result->get_row_count() << std::endl;
            
            // 获取所有列名
            auto columnNames = result->get_column_names();
            for (const auto& name : columnNames) {
                std::cout << name << "\t";
            }
            std::cout << std::endl;
            
            // 获取所有行数据
            auto rows = result->get_all_rows();
            for (const auto& row : rows) {
                for (const auto& name : columnNames) {
                    std::cout << row.at(name) << "\t";
                }
                std::cout << std::endl;
            }
        } else {
            std::cerr << "Query failed: " << connection->get_last_error() << std::endl;
        }
        
        // 7. 执行更新操作
        std::string updateSql = "UPDATE users SET last_login = NOW() WHERE id = 1";
        if (connection->execute(updateSql)) {
            std::cout << "Update successful" << std::endl;
        } else {
            std::cerr << "Update failed: " << connection->get_last_error() << std::endl;
        }
        
        // 8. 事务示例
        connection->begin_transaction();
        try {
            connection->execute("INSERT INTO logs (user_id, action) VALUES (1, 'login')");
            connection->execute("UPDATE user_stats SET login_count = login_count + 1 WHERE user_id = 1");
            connection->commit();
            std::cout << "Transaction committed" << std::endl;
        } catch (const std::exception& e) {
            connection->rollback();
            std::cerr << "Transaction rolled back: " << e.what() << std::endl;
        }
        
        // 9. 释放连接回连接池
        dbPool->release_connection(connection);
        
        // 10. 关闭连接池
        dbPool->close();
        
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
}

// 如果需要单独测试，可以取消下面的注释
/*
int main() {
    db_example();
    return 0;
}
*/