#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <memory>
#include <vector>
#include <string>
#include "mail_system/back/entities/mail.h"
#include "mail_system/front/business/DatabaseManager.h"

namespace mail_system {
namespace front {
namespace business {

/**
 * @brief 邮件管理器类，负责处理邮件相关的业务逻辑
 * 
 * 该类处理邮件的收发、存储、查询等操作
 */
class MailManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 获取MailManager的单例实例
     * @return MailManager的单例实例引用
     */
    static MailManager& getInstance();

    /**
     * @brief 发送邮件
     * @param from 发件人邮箱地址
     * @param to 收件人邮箱地址
     * @param header 邮件标题
     * @param body 邮件正文
     * @param attachments 附件列表
     * @return 发送是否成功
     */
    bool sendMail(const std::string& from, const std::string& to, 
                 const std::string& header, const std::string& body,
                 const std::vector<attachment>& attachments = {});

    /**
     * @brief 保存草稿
     * @param from 发件人邮箱地址
     * @param to 收件人邮箱地址
     * @param header 邮件标题
     * @param body 邮件正文
     * @param attachments 附件列表
     * @return 保存是否成功
     */
    bool saveDraft(const std::string& from, const std::string& to, 
                  const std::string& header, const std::string& body,
                  const std::vector<attachment>& attachments = {});

    /**
     * @brief 获取收件箱邮件
     * @param userEmail 用户邮箱地址
     * @return 收件箱邮件列表
     */
    std::vector<mail> getInboxMails(const std::string& userEmail);

    /**
     * @brief 获取发件箱邮件
     * @param userEmail 用户邮箱地址
     * @return 发件箱邮件列表
     */
    std::vector<mail> getSentMails(const std::string& userEmail);

    /**
     * @brief 获取草稿箱邮件
     * @param userEmail 用户邮箱地址
     * @return 草稿箱邮件列表
     */
    std::vector<mail> getDraftMails(const std::string& userEmail);

    /**
     * @brief 获取垃圾箱邮件
     * @param userEmail 用户邮箱地址
     * @return 垃圾箱邮件列表
     */
    std::vector<mail> getTrashMails(const std::string& userEmail);

    /**
     * @brief 获取指定邮箱的邮件
     * @param mailboxId 邮箱ID
     * @return 邮件列表
     */
    std::vector<mail> getMailsByMailbox(size_t mailboxId);

    /**
     * @brief 将邮件标记为已读
     * @param mailId 邮件ID
     * @return 操作是否成功
     */
    bool markAsRead(size_t mailId);

    /**
     * @brief 将邮件标记为未读
     * @param mailId 邮件ID
     * @return 操作是否成功
     */
    bool markAsUnread(size_t mailId);

    /**
     * @brief 删除邮件（移至垃圾箱）
     * @param mailId 邮件ID
     * @param userEmail 用户邮箱地址
     * @return 操作是否成功
     */
    bool deleteMail(size_t mailId, const std::string& userEmail);

    /**
     * @brief 永久删除邮件
     * @param mailId 邮件ID
     * @return 操作是否成功
     */
    bool permanentlyDeleteMail(size_t mailId);

    /**
     * @brief 恢复已删除邮件
     * @param mailId 邮件ID
     * @param userEmail 用户邮箱地址
     * @return 操作是否成功
     */
    bool restoreDeletedMail(size_t mailId, const std::string& userEmail);

    /**
     * @brief 获取邮件的附件
     * @param mailId 邮件ID
     * @return 附件列表
     */
    std::vector<attachment> getAttachments(size_t mailId);

    /**
     * @brief 添加附件
     * @param mailId 邮件ID
     * @param filename 文件名
     * @param filepath 文件路径
     * @param fileSize 文件大小
     * @param mimeType 文件MIME类型
     * @return 操作是否成功
     */
    bool addAttachment(size_t mailId, const std::string& filename, 
                      const std::string& filepath, size_t fileSize, 
                      const std::string& mimeType);

    /**
     * @brief 删除附件
     * @param attachmentId 附件ID
     * @return 操作是否成功
     */
    bool deleteAttachment(size_t attachmentId);

signals:
    /**
     * @brief 收到新邮件信号
     * @param newMail 新邮件
     */
    void newMailReceived(const mail& newMail);

    /**
     * @brief 邮件发送成功信号
     * @param sentMail 发送的邮件
     */
    void mailSent(const mail& sentMail);

    /**
     * @brief 邮件状态更新信号
     * @param updatedMail 更新后的邮件
     */
    void mailStatusUpdated(const mail& updatedMail);

private:
    // 私有构造函数和析构函数，防止外部创建实例
    MailManager(QObject* parent = nullptr);
    ~MailManager();
    
    // 禁用拷贝构造函数和赋值操作符
    MailManager(const MailManager&) = delete;
    MailManager& operator=(const MailManager&) = delete;

    /**
     * @brief 获取用户的指定类型邮箱
     * @param userEmail 用户邮箱地址
     * @param boxType 邮箱类型
     * @return 邮箱ID
     */
    size_t getUserMailboxByType(const std::string& userEmail, int boxType);

    /**
     * @brief 将邮件添加到指定邮箱
     * @param mailId 邮件ID
     * @param mailboxId 邮箱ID
     * @return 操作是否成功
     */
    bool addMailToMailbox(size_t mailId, size_t mailboxId);

    /**
     * @brief 从指定邮箱中移除邮件
     * @param mailId 邮件ID
     * @param mailboxId 邮箱ID
     * @return 操作是否成功
     */
    bool removeMailFromMailbox(size_t mailId, size_t mailboxId);
    
    // 单例实例
    static std::unique_ptr<MailManager> s_instance;
};

} // namespace business
} // namespace front
} // namespace mail_system