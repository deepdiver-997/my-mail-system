#pragma once

#include <QObject>
#include <QString>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <memory>
#include <vector>
#include <string>
#include "mail_system/back/entities/mail.h"
#include "mail_system/front/business/DatabaseManager.h"

namespace mail_system {
namespace front {
namespace business {

/**
 * @brief 附件管理器类，负责处理附件相关的业务逻辑
 * 
 * 该类处理附件的上传、下载、删除等操作
 */
class AttachmentManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 获取AttachmentManager的单例实例
     * @return AttachmentManager的单例实例引用
     */
    static AttachmentManager& getInstance();

    /**
     * @brief 设置附件存储目录
     * @param path 附件存储目录路径
     */
    void setAttachmentStoragePath(const QString& path);

    /**
     * @brief 获取附件存储目录
     * @return 附件存储目录路径
     */
    QString getAttachmentStoragePath() const;

    /**
     * @brief 上传附件
     * @param mailId 邮件ID
     * @param filePath 文件路径
     * @return 上传是否成功
     */
    bool uploadAttachment(size_t mailId, const QString& filePath);

    /**
     * @brief 下载附件
     * @param attachmentId 附件ID
     * @param targetPath 目标路径
     * @return 下载是否成功
     */
    bool downloadAttachment(size_t attachmentId, const QString& targetPath);

    /**
     * @brief 删除附件
     * @param attachmentId 附件ID
     * @return 删除是否成功
     */
    bool deleteAttachment(size_t attachmentId);

    /**
     * @brief 获取邮件的所有附件
     * @param mailId 邮件ID
     * @return 附件列表
     */
    std::vector<attachment> getAttachmentsByMail(size_t mailId);

    /**
     * @brief 获取附件信息
     * @param attachmentId 附件ID
     * @return 附件信息
     */
    attachment getAttachment(size_t attachmentId);

    /**
     * @brief 获取附件文件路径
     * @param att 附件信息
     * @return 附件文件的完整路径
     */
    QString getAttachmentFilePath(const attachment& att) const;

    /**
     * @brief 检查附件是否存在
     * @param attachmentId 附件ID
     * @return 附件是否存在
     */
    bool attachmentExists(size_t attachmentId);

    /**
     * @brief 获取附件的MIME类型
     * @param filePath 文件路径
     * @return MIME类型
     */
    QString getMimeType(const QString& filePath) const;

    /**
     * @brief 获取文件大小
     * @param filePath 文件路径
     * @return 文件大小（字节）
     */
    qint64 getFileSize(const QString& filePath) const;

signals:
    /**
     * @brief 附件上传成功信号
     * @param att 上传的附件信息
     */
    void attachmentUploaded(const attachment& att);

    /**
     * @brief 附件下载成功信号
     * @param att 下载的附件信息
     * @param targetPath 下载的目标路径
     */
    void attachmentDownloaded(const attachment& att, const QString& targetPath);

    /**
     * @brief 附件删除成功信号
     * @param attachmentId 被删除的附件ID
     */
    void attachmentDeleted(size_t attachmentId);

private:
    // 私有构造函数和析构函数，防止外部创建实例
    AttachmentManager(QObject* parent = nullptr);
    ~AttachmentManager() noexcept;
    
    // 禁用拷贝构造函数和赋值操作符
    AttachmentManager(const AttachmentManager&) = delete;
    AttachmentManager& operator=(const AttachmentManager&) = delete;

    /**
     * @brief 生成唯一的文件名
     * @param originalFilename 原始文件名
     * @return 唯一的文件名
     */
    QString generateUniqueFilename(const QString& originalFilename) const;

    /**
     * @brief 确保附件存储目录存在
     * @return 目录是否存在或创建成功
     */
    bool ensureAttachmentStoragePathExists() const;
    
    // 附件存储目录
    QString m_attachmentStoragePath;
    
    // 单例实例
    static std::unique_ptr<AttachmentManager> s_instance;
};

} // namespace business
} // namespace front
} // namespace mail_system