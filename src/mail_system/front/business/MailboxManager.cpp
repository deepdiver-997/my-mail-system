#include "mail_system/front/business/MailboxManager.h"
#include <QDateTime>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>

namespace mail_system {
namespace front {
namespace business {

// 初始化静态成员变量
std::unique_ptr<MailboxManager> MailboxManager::s_instance = nullptr;

MailboxManager& MailboxManager::getInstance() {
    if (!s_instance) {
        s_instance.reset(new MailboxManager());
    }
    return *s_instance;
}

MailboxManager::MailboxManager(QObject* parent) : QObject(parent) {
    // 初始化
}

MailboxManager::~MailboxManager() {
    // 清理资源
}

bool MailboxManager::createMailbox(size_t userId, const std::string& name) {
    // 创建新邮箱
    mailbox newMailbox;
    newMailbox.user_id = userId;
    newMailbox.name = name;
    newMailbox.is_system = false;
    newMailbox.box_type = 0; // 非系统邮箱
    newMailbox.create_time = QDateTime::currentSecsSinceEpoch();
    
    // 添加邮箱到数据库
    bool result = DatabaseManager::getInstance().addMailbox(newMailbox);
    
    if (result) {
        // 获取新创建邮箱的ID
        QSqlQuery query;
        query.prepare("SELECT last_insert_rowid()");
        if (query.exec() && query.next()) {
            newMailbox.id = query.value(0).toULongLong();
        }
        
        // 发送邮箱创建信号
        emit mailboxCreated(newMailbox);
    }
    
    return result;
}

bool MailboxManager::deleteMailbox(size_t mailboxId) {
    // 检查是否为系统邮箱
    mailbox mb = getMailbox(mailboxId);
    if (mb.id == 0) {
        qWarning() << "Mailbox not found with id:" << mailboxId;
        return false;
    }
    
    if (mb.is_system) {
        qWarning() << "Cannot delete system mailbox:" << mailboxId;
        return false;
    }
    
    // 删除邮箱
    bool result = DatabaseManager::getInstance().deleteMailbox(mailboxId);
    
    if (result) {
        // 发送邮箱删除信号
        emit mailboxDeleted(mailboxId);
    }
    
    return result;
}

bool MailboxManager::renameMailbox(size_t mailboxId, const std::string& newName) {
    // 检查是否为系统邮箱
    mailbox mb = getMailbox(mailboxId);
    if (mb.id == 0) {
        qWarning() << "Mailbox not found with id:" << mailboxId;
        return false;
    }
    
    if (mb.is_system) {
        qWarning() << "Cannot rename system mailbox:" << mailboxId;
        return false;
    }
    
    // 更新邮箱名称
    mb.name = newName;
    bool result = DatabaseManager::getInstance().updateMailbox(mb);
    
    if (result) {
        // 发送邮箱更新信号
        emit mailboxUpdated(mb);
    }
    
    return result;
}

std::vector<mailbox> MailboxManager::getUserMailboxes(size_t userId) {
    return DatabaseManager::getInstance().getMailboxesByUser(userId);
}

std::vector<mailbox> MailboxManager::getSystemMailboxes(size_t userId) {
    std::vector<mailbox> allMailboxes = getUserMailboxes(userId);
    std::vector<mailbox> systemMailboxes;
    
    for (const auto& mb : allMailboxes) {
        if (mb.is_system) {
            systemMailboxes.push_back(mb);
        }
    }
    
    return systemMailboxes;
}

std::vector<mailbox> MailboxManager::getCustomMailboxes(size_t userId) {
    std::vector<mailbox> allMailboxes = getUserMailboxes(userId);
    std::vector<mailbox> customMailboxes;
    
    for (const auto& mb : allMailboxes) {
        if (!mb.is_system) {
            customMailboxes.push_back(mb);
        }
    }
    
    return customMailboxes;
}

mailbox MailboxManager::getMailbox(size_t mailboxId) {
    return DatabaseManager::getInstance().getMailboxById(mailboxId);
}

size_t MailboxManager::getMailCount(size_t mailboxId) {
    QSqlQuery query;
    query.prepare("SELECT COUNT(*) FROM mail_mailbox WHERE mailbox_id = :mailbox_id");
    query.bindValue(":mailbox_id", static_cast<qint64>(mailboxId));
    
    if (query.exec() && query.next()) {
        return query.value(0).toULongLong();
    }
    
    return 0;
}

size_t MailboxManager::getUnreadMailCount(size_t mailboxId) {
    QSqlQuery query;
    query.prepare("SELECT COUNT(*) FROM mails m "
                 "JOIN mail_mailbox mm ON m.id = mm.mail_id "
                 "WHERE mm.mailbox_id = :mailbox_id AND m.is_read = 0");
    query.bindValue(":mailbox_id", static_cast<qint64>(mailboxId));
    
    if (query.exec() && query.next()) {
        return query.value(0).toULongLong();
    }
    
    return 0;
}

bool MailboxManager::moveMail(size_t mailId, size_t sourceMailboxId, size_t targetMailboxId) {
    // 开始事务
    QSqlQuery query;
    if (!query.exec("BEGIN TRANSACTION")) {
        qCritical() << "Failed to begin transaction:" << query.lastError().text();
        return false;
    }
    
    // 从源邮箱中移除邮件
    query.prepare("DELETE FROM mail_mailbox WHERE mail_id = :mail_id AND mailbox_id = :mailbox_id");
    query.bindValue(":mail_id", static_cast<qint64>(mailId));
    query.bindValue(":mailbox_id", static_cast<qint64>(sourceMailboxId));
    
    if (!query.exec()) {
        qCritical() << "Failed to remove mail from source mailbox:" << query.lastError().text();
        query.exec("ROLLBACK");
        return false;
    }
    
    // 将邮件添加到目标邮箱
    query.prepare("INSERT INTO mail_mailbox (mail_id, mailbox_id) VALUES (:mail_id, :mailbox_id)");
    query.bindValue(":mail_id", static_cast<qint64>(mailId));
    query.bindValue(":mailbox_id", static_cast<qint64>(targetMailboxId));
    
    if (!query.exec()) {
        qCritical() << "Failed to add mail to target mailbox:" << query.lastError().text();
        query.exec("ROLLBACK");
        return false;
    }
    
    // 提交事务
    if (!query.exec("COMMIT")) {
        qCritical() << "Failed to commit transaction:" << query.lastError().text();
        query.exec("ROLLBACK");
        return false;
    }
    
    return true;
}

bool MailboxManager::emptyMailbox(size_t mailboxId) {
    // 检查是否为系统邮箱
    mailbox mb = getMailbox(mailboxId);
    if (mb.id == 0) {
        qWarning() << "Mailbox not found with id:" << mailboxId;
        return false;
    }
    
    // 删除邮箱中的所有邮件关联
    QSqlQuery query;
    query.prepare("DELETE FROM mail_mailbox WHERE mailbox_id = :mailbox_id");
    query.bindValue(":mailbox_id", static_cast<qint64>(mailboxId));
    
    if (!query.exec()) {
        qCritical() << "Failed to empty mailbox:" << query.lastError().text();
        return false;
    }
    
    return true;
}

} // namespace business
} // namespace front
} // namespace mail_system