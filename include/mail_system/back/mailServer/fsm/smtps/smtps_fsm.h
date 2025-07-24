#ifndef SMTPS_FSM_H
#define SMTPS_FSM_H

#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include <functional>
#include <map>
#include <string>
#include <regex>

namespace mail_system {

// 状态处理函数类型定义
using StateHandler = std::function<void(std::weak_ptr<SmtpsSession>, const std::string&)>;

// SMTPS状态机接口
class SmtpsFsm {
protected:
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;
public:
    SmtpsFsm(std::shared_ptr<ThreadPoolBase> io_thread_pool,
             std::shared_ptr<ThreadPoolBase> worker_thread_pool,
             std::shared_ptr<DBPool> db_pool)
        : m_ioThreadPool(io_thread_pool),
          m_workerThreadPool(worker_thread_pool),
          m_dbPool(db_pool) {}
    virtual ~SmtpsFsm() = default;

    // 处理事件
    virtual void process_event(std::weak_ptr<SmtpsSession> session, SmtpsEvent event, const std::string& args) = 0;

    // 获取状态名称
    static std::string get_state_name(SmtpsState state);

    // 获取事件名称
    static std::string get_event_name(SmtpsEvent event);

    // 数据库操作

    bool auth_user(std::weak_ptr<SmtpsSession> session, const std::string& username, const std::string& password) {
        auto s = session.lock();
        if (!s) {
            std::cerr << "Session is expired in auth_user" << std::endl;
            return false;
        }
        auto connection = m_dbPool->get_connection();
        if (connection && connection->is_connected()) {
            std::string sql = "SELECT * FROM users WHERE username = '" +
                              connection->escape_string(username) + "' AND password = '" +
                              connection->escape_string(password) + "'";
            auto result = connection->query(sql);
            return result->get_row_count();
        }
        return false;
    }

    void get_mail_data(std::weak_ptr<SmtpsSession> session, std::string& mail_data) {
        auto s = session.lock();
        if (!s) {
            std::cerr << "Session is expired in get_mail_data" << std::endl;
            return;
        }
        auto connection = m_dbPool->get_connection();
        if (connection && connection->is_connected()) {
            std::string sql = "SELECT mail_data FROM mails WHERE sender_address = '" +
                              connection->escape_string(s->context_.sender_address) + "'";
            auto result = connection->query(sql);
            if (!result->get_row_count()) {
                mail_data = result->get_value(0, "mail_data");
            }
        }
    }

    void save_mail_data(mail* d) {
        std::unique_ptr<mail> data;
        data.reset(d);
        auto connection = m_dbPool->get_connection();
        if (connection && connection->is_connected()) {
            std::string sql = "INSERT INTO mails (sender, recipient, subjiect, body) VALUES ('" +
                              connection->escape_string(data->from) + "', '" +
                              connection->escape_string(data->to) + "', '" +
                              connection->escape_string(data->header) + "', '" +
                              connection->escape_string(data->body) + "')";
            connection->execute(sql);
        }
    }
};

} // namespace mail_system

#endif // SMTPS_FSM_H