#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <memory>
#include <string>
#include <vector>
#include "mail_system/back/entities/usr.h"
#include "mail_system/back/entities/mail.h"

namespace mail_system {
namespace front {
namespace business {

/**
 * @brief 数据库管理器类，负责处理与SQLite数据库的所有交互
 * 
 * 该类使用单例模式，确保整个应用程序中只有一个数据库连接实例
 */
class DatabaseManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 获取DatabaseManager的单例实例
     * @return DatabaseManager的单例实例引用
     */
    static DatabaseManager& getInstance();

    /**
     * @brief 初始化数据库连接和表结构
     * @param dbPath 数据库文件路径
     * @return 初始化是否成功
     */
    bool initialize(const QString& dbPath);

    /**
     * @brief 关闭数据库连接
     */
    void close();

    // 用户相关操作
    bool addUser(const usr& user);
    bool updateUser(const usr& user);
    bool deleteUser(size_t userId);
    usr getUserById(size_t userId);
    usr getUserByEmail(const std::string& email);
    bool validateUserCredentials(const std::string& email, const std::string& password);

    // 邮件相关操作
    bool addMail(const mail& mail);
    bool updateMail(const mail& mail);
    bool deleteMail(size_t mailId);
    mail getMailById(size_t mailId);
    std::vector<mail> getMailsByUser(const std::string& userEmail, bool isDraft = false);
    std::vector<mail> getMailsByMailbox(size_t mailboxId);

    // 邮箱相关操作
    bool addMailbox(const mailbox& mb);
    bool updateMailbox(const mailbox& mb);
    bool deleteMailbox(size_t mailboxId);
    mailbox getMailboxById(size_t mailboxId);
    std::vector<mailbox> getMailboxesByUser(size_t userId);
    
    // 附件相关操作
    bool addAttachment(const attachment& att);
    bool updateAttachment(const attachment& att);
    bool deleteAttachment(size_t attachmentId);
    attachment getAttachmentById(size_t attachmentId);
    std::vector<attachment> getAttachmentsByMail(size_t mailId);

private:
    // 私有构造函数和析构函数，防止外部创建实例
    DatabaseManager(QObject* parent = nullptr);
    ~DatabaseManager();
    
    // 禁用拷贝构造函数和赋值操作符
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // 创建数据库表
    bool createTables();

    // 数据库连接
    QSqlDatabase m_db;
    
    // 互斥锁，用于线程安全
    QMutex m_mutex;
    
    // 单例实例
    static std::unique_ptr<DatabaseManager> s_instance;
};

} // namespace business
} // namespace front
} // namespace mail_system