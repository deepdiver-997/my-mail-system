#include "mail_system/back/db/mariadb_pool.h"
#include "mail_system/back/db/mariadb_service.h"
#include "mail_system/back/db/mysql_pool.h"

namespace mail_system {

std::unique_ptr<MariaDBPoolFactory> MariaDBPoolFactory::s_instance = nullptr;
std::mutex MariaDBPoolFactory::s_mutex;

std::shared_ptr<DBPool> MariaDBPoolFactory::create_pool(
    const DBPoolConfig& config, std::shared_ptr<DBService> db_service) {
    // 池是引擎无关的：MySQLPool 只依赖 IDBConnection/DBService，直接复用。
    // 若调用方没传 DBService（用 MariaDBService 兜底），保证 achieve=mariadb 时
    // 一定创建 MariaDB 连接。
    if (!db_service) {
        db_service = std::make_shared<MariaDBService>();
    }
    return std::make_shared<MySQLPool>(config, std::move(db_service));
}

MariaDBPoolFactory& MariaDBPoolFactory::get_instance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_instance) {
        s_instance = std::unique_ptr<MariaDBPoolFactory>(new MariaDBPoolFactory());
    }
    return *s_instance;
}

} // namespace mail_system
