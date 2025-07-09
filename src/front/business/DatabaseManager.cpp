#include "mail_system/front/business/DatabaseManager.h"
#include <QDateTime>
#include <QCryptographicHash>

namespace mail_system {
namespace front {
namespace business {

// 初始化静态成员变量
std::unique_ptr<DatabaseManager> DatabaseManager::s_instance = nullptr;

DatabaseManager& DatabaseManager::getInstance() {
    if (!s_instance) {
        s_instance.reset(new DatabaseManager());
    }
    return *s_instance;
}

DatabaseManager::DatabaseManager(QObject* parent) : QObject(parent) {
    // 注册数据库驱动
    if (!QSqlDatabase::isDriverAvailable("QSQLITE")) {
        qCritical() << "SQLite driver is not available";
    }
}

DatabaseManager::~DatabaseManager() {
    close();
}

bool DatabaseManager::initialize(const QString& dbPath) {
    QMutexLocker locker(&m_mutex);
    
    // 创建数据库连接
    m_db = QSqlDatabase::addDatabase("QSQLITE");
    m_db.setDatabaseName(dbPath);
    
    if (!m_db.open()) {
        qCritical() << "Failed to open database:" << m_db.lastError().text();
        return false;
    }
    
    qDebug() << "Database opened successfully";
    
    // 创建表结构
    if (!createTables()) {
        qCritical() << "Failed to create tables";
        return false;
    }
    
    return true;
}

void DatabaseManager::close() {
    QMutexLocker locker(&m_mutex);
    
    if (m_db.isOpen()) {
        m_db.close();
        qDebug() << "Database connection closed";
    }
}

bool DatabaseManager::createTables() {
    QSqlQuery query;
    
    // 创建用户表
    if (!query.exec("CREATE TABLE IF NOT EXISTS users ("
                   "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "mail_address TEXT UNIQUE NOT NULL, "
                   "password TEXT NOT NULL, "
                   "name TEXT NOT NULL, "
                   "telephone TEXT, "
                   "register_time INTEGER NOT NULL)")) {
        qCritical() << "Failed to create users table:" << query.lastError().text();
        return false;
    }
    
    // 创建邮箱表
    if (!query.exec("CREATE TABLE IF NOT EXISTS mailboxes ("
                   "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "user_id INTEGER NOT NULL, "
                   "name TEXT NOT NULL, "
                   "is_system BOOLEAN NOT NULL, "
                   "box_type INTEGER, "
                   "create_time INTEGER NOT NULL, "
                   "FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE)")) {
        qCritical() << "Failed to create mailboxes table:" << query.lastError().text();
        return false;
    }
    
    // 创建邮件表
    if (!query.exec("CREATE TABLE IF NOT EXISTS mails ("
                   "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "from_address TEXT NOT NULL, "
                   "to_address TEXT NOT NULL, "
                   "header TEXT NOT NULL, "
                   "body TEXT, "
                   "send_time INTEGER NOT NULL, "
                   "is_draft BOOLEAN NOT NULL, "
                   "is_read BOOLEAN NOT NULL)")) {
        qCritical() << "Failed to create mails table:" << query.lastError().text();
        return false;
    }
    
    // 创建邮件-邮箱关联表
    if (!query.exec("CREATE TABLE IF NOT EXISTS mail_mailbox ("
                   "mail_id INTEGER NOT NULL, "
                   "mailbox_id INTEGER NOT NULL, "
                   "PRIMARY KEY (mail_id, mailbox_id), "
                   "FOREIGN KEY (mail_id) REFERENCES mails (id) ON DELETE CASCADE, "
                   "FOREIGN KEY (mailbox_id) REFERENCES mailboxes (id) ON DELETE CASCADE)")) {
        qCritical() << "Failed to create mail_mailbox table:" << query.lastError().text();
        return false;
    }
    
    // 创建附件表
    if (!query.exec("CREATE TABLE IF NOT EXISTS attachments ("
                   "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "mail_id INTEGER NOT NULL, "
                   "filename TEXT NOT NULL, "
                   "filepath TEXT NOT NULL, "
                   "file_size INTEGER NOT NULL, "
                   "mime_type TEXT NOT NULL, "
                   "upload_time INTEGER NOT NULL, "
                   "FOREIGN KEY (mail_id) REFERENCES mails (id) ON DELETE CASCADE)")) {
        qCritical() << "Failed to create attachments table:" << query.lastError().text();
        return false;
    }
    
    return true;
}

// 用户相关操作实现
bool DatabaseManager::addUser(const usr& user) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("INSERT INTO users (mail_address, password, name, telephone, register_time) "
                 "VALUES (:mail_address, :password, :name, :telephone, :register_time)");
    query.bindValue(":mail_address", QString::fromStdString(user.mailAddress));
    query.bindValue(":password", QString::fromStdString(user.password));
    query.bindValue(":name", QString::fromStdString(user.name));
    query.bindValue(":telephone", QString::fromStdString(user.telephone));
    query.bindValue(":register_time", static_cast<qint64>(user.registerTime));
    
    if (!query.exec()) {
        qCritical() << "Failed to add user:" << query.lastError().text();
        return false;
    }
    
    // 为新用户创建默认邮箱
    size_t userId = query.lastInsertId().toULongLong();
    
    // 创建收件箱
    mailbox inbox;
    inbox.user_id = userId;
    inbox.name = "收件箱";
    inbox.is_system = true;
    inbox.box_type = 1;
    inbox.create_time = time(nullptr);
    addMailbox(inbox);
    
    // 创建发件箱
    mailbox outbox;
    outbox.user_id = userId;
    outbox.name = "发件箱";
    outbox.is_system = true;
    outbox.box_type = 2;
    outbox.create_time = time(nullptr);
    addMailbox(outbox);
    
    // 创建垃圾箱
    mailbox trashbox;
    trashbox.user_id = userId;
    trashbox.name = "垃圾箱";
    trashbox.is_system = true;
    trashbox.box_type = 3;
    trashbox.create_time = time(nullptr);
    addMailbox(trashbox);
    
    // 创建已删除
    mailbox deletedbox;
    deletedbox.user_id = userId;
    deletedbox.name = "已删除";
    deletedbox.is_system = true;
    deletedbox.box_type = 4;
    deletedbox.create_time = time(nullptr);
    addMailbox(deletedbox);
    
    return true;
}

bool DatabaseManager::updateUser(const usr& user) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("UPDATE users SET mail_address = :mail_address, password = :password, "
                 "name = :name, telephone = :telephone WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(user.id));
    query.bindValue(":mail_address", QString::fromStdString(user.mailAddress));
    query.bindValue(":password", QString::fromStdString(user.password));
    query.bindValue(":name", QString::fromStdString(user.name));
    query.bindValue(":telephone", QString::fromStdString(user.telephone));
    
    if (!query.exec()) {
        qCritical() << "Failed to update user:" << query.lastError().text();
        return false;
    }
    
    return query.numRowsAffected() > 0;
}

bool DatabaseManager::deleteUser(size_t userId) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("DELETE FROM users WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(userId));
    
    if (!query.exec()) {
        qCritical() << "Failed to delete user:" << query.lastError().text();
        return false;
    }
    
    return query.numRowsAffected() > 0;
}

usr DatabaseManager::getUserById(size_t userId) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("SELECT * FROM users WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(userId));
    
    usr user;
    if (query.exec() && query.next()) {
        user.id = query.value("id").toULongLong();
        user.mailAddress = query.value("mail_address").toString().toStdString();
        user.password = query.value("password").toString().toStdString();
        user.name = query.value("name").toString().toStdString();
        user.telephone = query.value("telephone").toString().toStdString();
        user.registerTime = query.value("register_time").toLongLong();
    } else {
        qWarning() << "User not found with id:" << userId;
    }
    
    return user;
}

usr DatabaseManager::getUserByEmail(const std::string& email) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("SELECT * FROM users WHERE mail_address = :mail_address");
    query.bindValue(":mail_address", QString::fromStdString(email));
    
    usr user;
    if (query.exec() && query.next()) {
        user.id = query.value("id").toULongLong();
        user.mailAddress = query.value("mail_address").toString().toStdString();
        user.password = query.value("password").toString().toStdString();
        user.name = query.value("name").toString().toStdString();
        user.telephone = query.value("telephone").toString().toStdString();
        user.registerTime = query.value("register_time").toLongLong();
    } else {
        qWarning() << "User not found with email:" << QString::fromStdString(email);
    }
    
    return user;
}

bool DatabaseManager::validateUserCredentials(const std::string& email, const std::string& password) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("SELECT password FROM users WHERE mail_address = :mail_address");
    query.bindValue(":mail_address", QString::fromStdString(email));
    
    if (query.exec() && query.next()) {
        std::string storedPassword = query.value("password").toString().toStdString();
        return storedPassword == password; // 实际应用中应该使用安全的密码比较方法
    }
    
    return false;
}

// 邮件相关操作实现
bool DatabaseManager::addMail(const mail& mail) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("INSERT INTO mails (from_address, to_address, header, body, send_time, is_draft, is_read) "
                 "VALUES (:from_address, :to_address, :header, :body, :send_time, :is_draft, :is_read)");
    query.bindValue(":from_address", QString::fromStdString(mail.from));
    query.bindValue(":to_address", QString::fromStdString(mail.to));
    query.bindValue(":header", QString::fromStdString(mail.header));
    query.bindValue(":body", QString::fromStdString(mail.body));
    query.bindValue(":send_time", static_cast<qint64>(mail.send_time));
    query.bindValue(":is_draft", mail.is_draft);
    query.bindValue(":is_read", mail.is_read);
    
    if (!query.exec()) {
        qCritical() << "Failed to add mail:" << query.lastError().text();
        return false;
    }
    
    return true;
}

bool DatabaseManager::updateMail(const mail& mail) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("UPDATE mails SET from_address = :from_address, to_address = :to_address, "
                 "header = :header, body = :body, is_draft = :is_draft, is_read = :is_read "
                 "WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(mail.id));
    query.bindValue(":from_address", QString::fromStdString(mail.from));
    query.bindValue(":to_address", QString::fromStdString(mail.to));
    query.bindValue(":header", QString::fromStdString(mail.header));
    query.bindValue(":body", QString::fromStdString(mail.body));
    query.bindValue(":is_draft", mail.is_draft);
    query.bindValue(":is_read", mail.is_read);
    
    if (!query.exec()) {
        qCritical() << "Failed to update mail:" << query.lastError().text();
        return false;
    }
    
    return query.numRowsAffected() > 0;
}

bool DatabaseManager::deleteMail(size_t mailId) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("DELETE FROM mails WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(mailId));
    
    if (!query.exec()) {
        qCritical() << "Failed to delete mail:" << query.lastError().text();
        return false;
    }
    
    return query.numRowsAffected() > 0;
}

mail DatabaseManager::getMailById(size_t mailId) {
    QMutexLocker locker(&m_mutex);
    
    QSqlQuery query;
    query.prepare("SELECT * FROM mails WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(mailId));
    
    mail m;
    if (query.exec() && query.next()) {
        m.id = query.value("id").toULongLong();
        m.from =