#ifndef MAIL_SYSTEM_IMAPS_SERVER_FACTORY_H
#define MAIL_SYSTEM_IMAPS_SERVER_FACTORY_H

#include <memory>
#include <boost/asio.hpp>
#include "mail_system/back/mailServer/factory/mail_server_factory.h"
#include "mail_system/back/mailServer/imaps/imaps_server.h"

namespace mail_system {

class ImapsServerFactory : public MailServerFactory {
public:
    ImapsServerFactory(boost::asio::io_context& io_context,
                     std::shared_ptr<DBPool> db_pool);
    virtual ~ImapsServerFactory();

    // 创建服务器实例
    std::unique_ptr<ServerBase> create_server(const ServerConfig& config);

private:
    // IO上下文
    boost::asio::io_context& m_io_context;
    // 数据库连接池
    std::shared_ptr<DBPool> m_dbPool;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_IMAPS_SERVER_FACTORY_H