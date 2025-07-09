#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include <vector>
#include <string>
#include "mail_system/back/entities/mail.h"
#include "mail_system/front/business/DatabaseManager.h"

namespace mail_system {
namespace front {
namespace business {

/**
 * @brief 邮箱管理器类，负责处理邮箱相关的业务逻辑
 * 
 * 该类处理邮箱的创建、删除、重命名等操作
 */
class MailboxManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 获取MailboxManager的单例实例
     * @return MailboxManager的单例实例引用
     */
    static MailboxManager& getInstance();

    /**
     * @brief 创建新邮箱
     * @param userId 用户ID
     * @param name 邮箱名称
     * @return 创建是否成功
     */
    bool createMailbox(size_t userId, const std::string& name);

    /**
     * @brief 删除邮箱
     * @param mailboxId 邮箱ID
     * @return 删除是否成功
     */
    bool deleteMailbox(size_t mailboxId);

    /**
     * @brief 重命名邮箱
     * @param mailboxId 邮箱ID
     * @param newName 新名称
     * @return 重命名是否成功
     */
    bool renameMailbox(size_t mailboxId, const std::string& newName);

    /**
     * @brief 获取用户的所有邮箱
     * @param userId 用户ID
     * @return 邮箱列表
     */
    std::vector<mailbox> getUserMailboxes(size_t userId);

    /**
     * @brief 获取用户的系统邮箱
     * @param userId 用户ID
     * @return 系统邮箱列表
     */
    std::vector<mailbox> getSystemMailboxes(size_t userId);

    /**
     * @brief 获取用户的自定义邮箱
     * @param userId 用户ID
     * @return 自定义邮箱列表
     */
    std::vector<mailbox> getCustomMailboxes(size_t userId);

    /**
     * @brief 获取邮箱信息
     * @param mailboxId 邮箱ID
     * @return 邮箱信息
     */
    mailbox getMailbox(size_t mailboxId);

    /**
     * @brief 获取邮箱中的邮件数量
     * @param mailboxId 邮箱ID
     * @return 邮件数量
     */
    size_t getMailCount(size_t mailboxId);

    /**
     * @brief 获取邮箱中的未读邮件数量
     * @param mailboxId 邮箱ID
     * @return 未读邮件数量
     */
    size_t getUnreadMailCount(size_t mailboxId);

    /**
     * @brief 将邮件移动到指定邮箱
     * @param mailId 邮件ID
     * @param sourceMailboxId 源邮箱ID
     * @param targetMailboxId 目标邮箱ID
     * @return 移动是否成功
     */
    bool moveMail(size_t mailId, size_t sourceMailboxId, size_t targetMailboxId);

    /**
     * @brief 清空邮箱
     * @param mailboxId 邮箱ID
     * @return 清空是否成功
     */
    bool emptyMailbox(size_t mailboxId);

signals:
    /**
     * @brief 邮箱创建信号
     * @param newMailbox 新创建的邮箱
     */
    void mailboxCreated(const mailbox& newMailbox);

    /**
     * @brief 邮箱删除信号
     * @param mailboxId 被删除的邮箱ID
     */
    void mailboxDeleted(size_t mailboxId);

    /**
     * @brief 邮箱更新信号
     * @param updatedMailbox 更新后的邮箱
     */
    void mailboxUpdated(const mailbox& updatedMailbox);

private:
    // 私有构造函数和析构函数，防止外部创建实例
    MailboxManager(QObject* parent = nullptr);
    ~MailboxManager();
    
    // 禁用拷贝构造函数和赋值操作符
    MailboxManager(const MailboxManager&) = delete;
    MailboxManager& operator=(const MailboxManager&) = delete;
    
    // 单例实例
    static std::unique_ptr<MailboxManager> s_instance;
};

} // namespace business
} // namespace front
} // namespace mail_system