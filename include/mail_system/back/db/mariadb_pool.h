#ifndef MAIL_SYSTEM_MARIADB_POOL_H
#define MAIL_SYSTEM_MARIADB_POOL_H

#include "mail_system/back/db/db_pool.h"
#include <memory>

namespace mail_system {

// MariaDB 连接池工厂：复用引擎无关的 MySQLPool（它只依赖 IDBConnection/DBService），
// 只换 DBService 为 MariaDBService。`achieve=mariadb` 时由 server_base 调用。
class MariaDBPoolFactory : public DBPoolFactory {
public:
    ~MariaDBPoolFactory() override = default;

    std::shared_ptr<DBPool> create_pool(
        const DBPoolConfig& config,
        std::shared_ptr<DBService> db_service
    ) override;

    static MariaDBPoolFactory& get_instance();

private:
    MariaDBPoolFactory() = default;
    static std::unique_ptr<MariaDBPoolFactory> s_instance;
    static std::mutex s_mutex;

    MariaDBPoolFactory(const MariaDBPoolFactory&) = delete;
    MariaDBPoolFactory& operator=(const MariaDBPoolFactory&) = delete;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_MARIADB_POOL_H
