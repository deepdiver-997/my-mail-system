#ifndef SMTPS_FSM_H
#define SMTPS_FSM_H

#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/db/db_service.h"
#include <functional>
#include <map>
#include <string>
#include <regex>

namespace mail_system {

// 状态处理函数类型定义
using StateHandler = std::function<void(SmtpsSession*, const std::string&)>;

// SMTPS状态机接口
class SmtpsFsm {
    std::shared_ptr<DBPool> db_pool_;
public:
    SmtpsFsm(std::shared_ptr<DBPool> db_pool) : db_pool_(db_pool) {}
    virtual ~SmtpsFsm() = default;

    // 处理事件
    virtual void process_event(SmtpsSession* session, SmtpsEvent event, const std::string& args) = 0;

    // 获取状态名称
    static std::string get_state_name(SmtpsState state);

    // 获取事件名称
    static std::string get_event_name(SmtpsEvent event);

    // 数据库操作
    void save_mail_data(SmtpsSession* session, const std::string& mail_data) {
        auto connection = db_pool_->get_connection();
        if (connection && connection->is_connected()) {
            std::string sql = "INSERT INTO mails (session_id, mail_data) VALUES ('" +
                              connection->escape_string(session->get_session_id()) + "', '" +
                              connection->escape_string(mail_data) + "')";
            connection->execute(sql);
        }
    }
};

} // namespace mail_system

#endif // SMTPS_FSM_H