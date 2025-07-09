#ifndef MAIL_SYSTEM_POP3S_FSM_H
#define MAIL_SYSTEM_POP3S_FSM_H

#include <string>
#include <memory>
#include <vector>
#include <map>
#include "mail_system/back/db/db_pool.h"

namespace mail_system {

// POP3状态枚举
enum class Pop3sState {
    AUTHORIZATION,   // 授权状态，需要用户名和密码
    TRANSACTION,     // 事务状态，可以执行命令
    UPDATE           // 更新状态，准备退出
};

// POP3事件枚举
enum class Pop3sEvent {
    CONNECT,        // 客户端连接
    USER,           // USER命令
    PASS,           // PASS命令
    STAT,          // STAT命令
    LIST,           // LIST命令
    RETR,           // RETR命令
    DELE,           // DELE命令
    NOOP,           // NOOP命令
    RSET,           // RSET命令
    QUIT,           // QUIT命令
    TOP,            // TOP命令
    UIDL,           // UIDL命令
    UNKNOWN         // 未知命令
};

// POP3S上下文结构
struct Pop3sContext {
    std::string username;       // 当前用户名
    int userId;                 // 用户ID
    std::vector<Pop3MailInfo> mails; // 用户邮件列表
};

// 邮件信息结构
struct Pop3MailInfo {
    int id;                     // 邮件ID
    std::string from;           // 发件人
    std::string to;             // 收件人
    std::string subject;        // 主题
    std::string content;        // 内容
    std::string date;           // 日期
    int size;                   // 大小
    bool deleted;               // 是否标记为删除
};

// POP3S状态机类
class Pop3sFsm {
public:
    explicit Pop3sFsm(std::shared_ptr<DBPool> db_pool);
    virtual ~Pop3sFsm();

    // 处理POP3命令
    std::string process_command(const std::string& command, const std::string& args);
    // 获取当前状态
    Pop3sState get_state() const;
    // 重置状态机
    void reset();

protected:
    // 验证用户
    bool authenticate_user(const std::string& username, const std::string& password);
    // 加载用户邮件
    bool load_user_mails();
    // 更新邮件状态
    bool update_mail_status();

private:
    // 处理USER命令
    std::string handle_user(const std::string& args);
    // 处理PASS命令
    std::string handle_pass(const std::string& args);
    // 处理STAT命令
    std::string handle_stat();
    // 处理LIST命令
    std::string handle_list(const std::string& args);
    // 处理RETR命令
    std::string handle_retr(const std::string& args);
    // 处理DELE命令
    std::string handle_dele(const std::string& args);
    // 处理NOOP命令
    std::string handle_noop();
    // 处理RSET命令
    std::string handle_rset();
    // 处理QUIT命令
    std::string handle_quit();
    // 处理TOP命令
    std::string handle_top(const std::string& args);
    // 处理UIDL命令
    std::string handle_uidl(const std::string& args);
    // 处理未知命令
    std::string handle_unknown(const std::string& command);

    // 邮件映射表（邮件序号 -> 邮件索引）
    std::map<int, size_t> m_mailMap;
    // 数据库连接池
    std::shared_ptr<DBPool> m_dbPool;
};

// POP3S状态机工厂类
class Pop3sFsmFactory {
public:
    explicit Pop3sFsmFactory(std::shared_ptr<DBPool> db_pool);
    virtual ~Pop3sFsmFactory();

    // 创建新的状态机实例
    std::unique_ptr<Pop3sFsm> create_fsm();

private:
    // 数据库连接池
    std::shared_ptr<DBPool> m_dbPool;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_POP3S_FSM_H