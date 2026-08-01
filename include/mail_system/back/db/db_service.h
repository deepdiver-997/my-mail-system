#ifndef MAIL_SYSTEM_DB_SERVICE_H
#define MAIL_SYSTEM_DB_SERVICE_H

#include "framework/db/db_pool.h"
#include <string>
#include <memory>

namespace mail_system {

// 向后兼容
using pr::IDBResult;
using pr::IDBConnection;
using pr::DBService;
using pr::QueryCallback;
using pr::ExecuteCallback;

} // namespace mail_system
#endif
