#include "mail_system/front/business/MailManager.h"
#include <QDateTime>
#include <QDebug>
#include "mail_system/front/business/UserManager.h"

namespace mail_system {
namespace front {
namespace business {

// 初始化静态成员变量
std::unique_ptr<MailManager> MailManager::s_instance = nullptr;

MailManager& MailManager::getInstance() {
    if (!s_instance) {
        s_instance.reset(new MailManager());
    }
    return *s_instance;
}

MailManager::MailManager(QObject* parent) : QObject(parent) {
    // 初始化
}

MailManager::~MailManager() {
    // 清理资源
}

bool MailManager::sendMail(const std::string& from, const std::string& to, 
                          const std::string& header, const std::string& body,
                          const std::vector<attachment>& attachments) {
    // 创建新邮件
    mail newMail;
    newMail.from = from;
    newMail.to = to;
    newMail.header = header;
    newMail.body = body;
    newMail.send_time = QDateTime::currentSecsSinceEpoch();
    newMail.is_draft = false;
    newMail.is_read = false;
    
    // 添加邮件到数据库
    if (!DatabaseManager::getInstance().addMail(newMail)) {
        qCritical() << "Failed to add mail to database";
        return false;
    }
    
    // 获取新添加邮件的ID
    size_t mailId = newMail.id;
    
    // 添加附件
    for (const auto& att : attachments) {
        attachment newAtt = att;
        newAtt.mail_id = mailId;
        newAtt.upload_time = QDateTime::currentSecsSinceEpoch();
        
        if (!DatabaseManager::getInstance().addAttachment(newAtt)) {
            qWarning() << "Failed to add attachment:" << QString::fromStdString(att.filename);
        }
    }
    
    // 将邮件添加到发件人的发件箱
    size_t senderOutboxId = getUserMailboxByType(from, 2); // 2 = 发件箱
    if (senderOutboxId == 0 || !addMailToMailbox(mailId, senderOutboxId)) {
        qWarning() << "Failed to add mail to sender's outbox";
    }
    
    // 将邮件添加到收件人的收件箱
    size_t recipientInboxId = getUserMailboxByType(to, 1); // 1 = 收件箱
    if (recipientInboxId == 0 || !addMailToMailbox(mailId, recipientInboxId)) {
        qWarning() << "Failed to add mail to recipient's inbox";
    }
    
    // 发送邮件发送成功信号
    emit mailSent(newMail);
    
    return true;
}

bool MailManager::saveDraft(const std::string& from, const std::string& to, 
                           const std::string& header, const std::string& body,
                           const std::vector<attachment>& attachments) {
    // 创建新草稿
    mail draftMail;
    draftMail.from = from;
    draftMail.to = to;
    draftMail.header = header;
    draftMail.body = body;
    draftMail.send_time = QDateTime::currentSecsSinceEpoch();
    draftMail.is_draft = true;
    draftMail.is_read = true; // 草稿默认为已读
    
    // 添加草稿到数据库
    if (!DatabaseManager::getInstance().addMail(draftMail)) {
        qCritical() << "Failed to add draft to database";
        return false;
    }
    
    // 获取新添加草稿的ID
    size_t mailId = draftMail.id;
    
    // 添加附件
    for (const auto& att : attachments) {
        attachment newAtt = att;
        newAtt.mail_id = mailId;
        newAtt.upload_time = QDateTime::currentSecsSinceEpoch();
        
        if (!DatabaseManager::getInstance().addAttachment(newAtt)) {
            qWarning() << "Failed to add attachment:" << QString::fromStdString(att.filename);
        }
    }
    
    // 将草稿添加到用户的草稿箱
    // 注意：这里假设草稿箱是一个自定义邮箱，需要先创建
    // 在实际应用中，可能需要在用户注册时创建草稿箱
    
    // 发送邮件状态更新信号
    emit mailStatusUpdated(draftMail);
    
    return true;
}

std::vector<mail> MailManager::getInboxMails(const std::string& userEmail) {
    size_t inboxId = getUserMailboxByType(userEmail, 1); // 1 = 收件箱
    if (inboxId == 0) {
        qWarning() << "Inbox not found for user:" << QString::fromStdString(userEmail);
        return {};
    }
    
    return DatabaseManager::getInstance().getMailsByMailbox(inboxId);
}

std::vector<mail> MailManager::getSentMails(const std::string& userEmail) {
    size_t outboxId = getUserMailboxByType(userEmail, 2); // 2 = 发件箱
    if (outboxId == 0) {
        qWarning() << "Outbox not found for user:" << QString::fromStdString(userEmail);
        return {};
    }
    
    return DatabaseManager::getInstance().getMailsByMailbox(outboxId);
}

std::vector<mail> MailManager::getDraftMails(const std::string& userEmail) {
    // 获取用户的所有邮件，筛选出草稿
    return DatabaseManager::getInstance().getMailsByUser(userEmail, true);
}

std::vector<mail> MailManager::getTrashMails(const std::string& userEmail) {
    size_t trashId = getUserMailboxByType(userEmail, 3); // 3 = 垃圾箱
    if (trashId == 0) {
        qWarning() << "Trash not found for user:" << QString::fromStdString(userEmail);
        return {};
    }
    
    return DatabaseManager::getInstance().getMailsByMailbox(trashId);
}

std::vector<mail> MailManager::getMailsByMailbox(size_t mailboxId) {
    return DatabaseManager::getInstance().getMailsByMailbox(mailboxId);
}

bool MailManager::markAsRead(size_t mailId) {
    mail m = DatabaseManager::getInstance().getMailById(mailId);
    if (m.id == 0) {
        qWarning() << "Mail not found with id:" << mailId;
        return false;
    }
    
    m.is_read = true;
    bool result = DatabaseManager::getInstance().updateMail(m);
    
    if (result) {
        emit mailStatusUpdated(m);
    }
    
    return result;
}

bool MailManager::markAsUnread(size_t mailId) {
    mail m = DatabaseManager::getInstance().getMailById(mailId);
    if (m.id == 0) {
        qWarning() << "Mail not found with id:" << mailId;
        return false;
    }
    
    m.is_read = false;
    bool result = DatabaseManager::getInstance().updateMail(m);
    
    if (result) {
        emit mailStatusUpdated(m);
    }
    
    return result;
}

bool MailManager::deleteMail(size_t mailId, const std::string& userEmail) {
    // 获取用户的垃圾箱
    size_t trashId = getUserMailboxByType(userEmail, 3); // 3 = 垃圾箱
    if (trashId == 0) {
        qWarning() << "Trash not found for user:" << QString::fromStdString(userEmail);
        return false;
    }
    
    // 将邮件添加到垃圾箱
    return addMailToMailbox(mailId, trashId);
}

bool MailManager::permanentlyDeleteMail(size_t mailId) {
    return DatabaseManager::getInstance().deleteMail(mailId);
}

bool MailManager::restoreDeletedMail(size_t mailId, const std::string& userEmail) {
    mail m = DatabaseManager::getInstance().getMailById(mailId);
    if (m.id == 0) {
        qWarning() << "Mail not found with id:" << mailId;
        return false;
    }
    
    // 获取用户的垃圾箱
    size_t trashId = getUserMailboxByType(userEmail, 3); // 3 = 垃圾箱
    if (trashId == 0) {
        qWarning() << "Trash not found for user:" << QString::fromStdString(userEmail);
        return false;
    }
    
    // 从垃圾箱中移除邮件
    if (!removeMailFromMailbox(mailId, trashId)) {
        qWarning() << "Failed to remove mail from trash";
        return false;
    }
    
    // 根据邮件类型，将邮件添加到适当的邮箱
    size_t targetMailboxId;
    if (m.from == userEmail && m.to == userEmail) {
        // 自己发给自己的邮件，添加到收件箱
        targetMailboxId = getUserMailboxByType(userEmail, 1); // 1 = 收件箱
    } else if (m.from == userEmail) {
        // 自己发出的邮件，添加到发件箱
        targetMailboxId = getUserMailboxByType(userEmail, 2); // 2 = 发件箱
    } else {
        // 收到的邮件，添加到收件箱
        targetMailboxId = getUserMailboxByType(userEmail, 1); // 1 = 收件箱
    }
    
    if (targetMailboxId == 0) {
        qWarning() << "Target mailbox not found";
        return false;
    }
    
    return addMailToMailbox(mailId, targetMailboxId);
}

std::vector<attachment> MailManager::getAttachments(size_t mailId) {
    return DatabaseManager::getInstance().getAttachmentsByMail(mailId);
}

bool MailManager::addAttachment(size_t mailId, const std::string& filename, 
                               const std::string& filepath, size_t fileSize, 
                               const std::string& mimeType) {
    attachment att;
    att.mail_id = mailId;
    att.filename = filename;
    att.filepath = filepath;
    att.file_size = fileSize;
    att.mime_type = mimeType;
    att.upload_time = QDateTime::currentSecsSinceEpoch();
    
    return DatabaseManager::getInstance().addAttachment(att);
}

bool MailManager::deleteAttachment(size_t attachmentId) {
    return DatabaseManager::getInstance().deleteAttachment(attachmentId);
}

size_t MailManager::getUserMailboxByType(const std::string& userEmail, int boxType) {
    // 获取用户ID
    usr user = DatabaseManager::getInstance().getUserByEmail(userEmail);
    if (user.id == 0) {
        qWarning() << "User not found with email:" << QString::fromStdString(userEmail);
        return 0;
    }
    
    // 获取用户的所有邮箱
    std::vector<mailbox> mailboxes = DatabaseManager::getInstance().getMailboxesByUser(user.id);
    
    // 查找指定类型的邮箱
    for (const auto& mb : mailboxes) {
        if (mb.is_system && mb.box_type == boxType) {
            return mb.id;
        }
    }
    
    qWarning() << "Mailbox not found with type:" << boxType << "for user:" << QString::fromStdString(userEmail);
    return 0;
}

bool MailManager::addMailToMailbox(size_t mailId, size_t mailboxId) {
    QSqlQuery query;
    query.prepare("INSERT INTO mail_mailbox (mail_id, mailbox_id) VALUES (:mail_id, :mailbox_id)");
    query.bindValue(":mail_id", static_cast<qint64>(mailId));
    query.bindValue(":mailbox_id", static_cast<qint64>(mailboxId));
    
    if (!query.exec()) {
        qCritical() << "Failed to add mail to mailbox:" << query.lastError().text();
        return false;
    }
    
    return true;
}

bool MailManager::removeMailFromMailbox(size_t mailId, size_t mailboxId) {
    QSqlQuery query;
    query.prepare("DELETE FROM mail_mailbox WHERE mail_id = :mail_id AND mailbox_id = :mailbox_id");
    query.bindValue(":mail_id", static_cast<qint64>(mailId));
    query.bindValue(":mailbox_id", static_cast<qint64>(mailboxId));
    
    if (!query.exec()) {
        qCritical() << "Failed to remove mail from mailbox:" << query.lastError().text();
        return false;
    }
    
    return query.numRowsAffected() > 0;
}

} // namespace business
} // namespace front
} // namespace mail_system