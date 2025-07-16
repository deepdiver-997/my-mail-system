#include "mail_system/front/business/AttachmentManager.h"
#include <QDir>
#include <QUuid>
#include <QDateTime>
#include <QDebug>

namespace mail_system {
namespace front {
namespace business {

// 初始化静态成员变量
std::unique_ptr<AttachmentManager> AttachmentManager::s_instance = nullptr;

AttachmentManager& AttachmentManager::getInstance() {
    if (!s_instance) {
        s_instance.reset(new AttachmentManager());
    }
    return *s_instance;
}

AttachmentManager::AttachmentManager(QObject* parent) : QObject(parent) {
    // 默认附件存储路径
    m_attachmentStoragePath = QDir::homePath() + "/MailSystemAttachments";
    ensureAttachmentStoragePathExists();
}

AttachmentManager::~AttachmentManager() {
    // 清理资源
}

void AttachmentManager::setAttachmentStoragePath(const QString& path) {
    m_attachmentStoragePath = path;
    ensureAttachmentStoragePathExists();
}

QString AttachmentManager::getAttachmentStoragePath() const {
    return m_attachmentStoragePath;
}

bool AttachmentManager::uploadAttachment(size_t mailId, const QString& filePath) {
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qWarning() << "File does not exist or is not a regular file:" << filePath;
        return false;
    }
    
    // 获取文件信息
    QString filename = fileInfo.fileName();
    qint64 fileSize = fileInfo.size();
    QString mimeType = getMimeType(filePath);
    
    // 生成唯一的存储文件名
    QString uniqueFilename = generateUniqueFilename(filename);
    QString targetPath = m_attachmentStoragePath + "/" + uniqueFilename;
    
    // 复制文件到附件存储目录
    if (!QFile::copy(filePath, targetPath)) {
        qCritical() << "Failed to copy file to attachment storage:" << filePath << "->" << targetPath;
        return false;
    }
    
    // 创建附件记录
    attachment att;
    att.mail_id = mailId;
    att.filename = filename.toStdString();
    att.filepath = uniqueFilename.toStdString(); // 只存储相对路径
    att.file_size = fileSize;
    att.mime_type = mimeType.toStdString();
    att.upload_time = QDateTime::currentSecsSinceEpoch();
    
    // 添加附件到数据库
    bool result = DatabaseManager::getInstance().addAttachment(att);
    
    if (result) {
        // 获取新添加附件的ID
        QSqlQuery query;
        query.prepare("SELECT last_insert_rowid()");
        if (query.exec() && query.next()) {
            att.id = query.value(0).toULongLong();
        }
        
        // 发送附件上传成功信号
        emit attachmentUploaded(att);
    } else {
        // 如果数据库添加失败，删除已复制的文件
        QFile::remove(targetPath);
    }
    
    return result;
}

bool AttachmentManager::downloadAttachment(size_t attachmentId, const QString& targetPath) {
    // 获取附件信息
    attachment att = getAttachment(attachmentId);
    if (att.id == 0) {
        qWarning() << "Attachment not found with id:" << attachmentId;
        return false;
    }
    
    // 获取附件文件路径
    QString sourcePath = getAttachmentFilePath(att);
    
    // 检查源文件是否存在
    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        qWarning() << "Attachment file does not exist or is not a regular file:" << sourcePath;
        return false;
    }
    
    // 确保目标目录存在
    QFileInfo targetInfo(targetPath);
    QDir targetDir = targetInfo.dir();
    if (!targetDir.exists()) {
        if (!targetDir.mkpath(".")) {
            qCritical() << "Failed to create target directory:" << targetDir.path();
            return false;
        }
    }
    
    // 复制文件到目标路径
    if (QFile::exists(targetPath)) {
        if (!QFile::remove(targetPath)) {
            qCritical() << "Failed to overwrite existing file:" << targetPath;
            return false;
        }
    }
    
    if (!QFile::copy(sourcePath, targetPath)) {
        qCritical() << "Failed to copy attachment file:" << sourcePath << "->" << targetPath;
        return false;
    }
    
    // 发送附件下载成功信号
    emit attachmentDownloaded(att, targetPath);
    
    return true;
}

bool AttachmentManager::deleteAttachment(size_t attachmentId) {
    // 获取附件信息
    attachment att = getAttachment(attachmentId);
    if (att.id == 0) {
        qWarning() << "Attachment not found with id:" << attachmentId;
        return false;
    }
    
    // 获取附件文件路径
    QString filePath = getAttachmentFilePath(att);
    
    // 删除文件
    if (QFile::exists(filePath)) {
        if (!QFile::remove(filePath)) {
            qWarning() << "Failed to delete attachment file:" << filePath;
            // 继续删除数据库记录，即使文件删除失败
        }
    }
    
    // 删除数据库记录
    bool result = DatabaseManager::getInstance().deleteAttachment(attachmentId);
    
    if (result) {
        // 发送附件删除成功信号
        emit attachmentDeleted(attachmentId);
    }
    
    return result;
}

std::vector<attachment> AttachmentManager::getAttachmentsByMail(size_t mailId) {
    return DatabaseManager::getInstance().getAttachmentsByMail(mailId);
}

attachment AttachmentManager::getAttachment(size_t attachmentId) {
    return DatabaseManager::getInstance().getAttachmentById(attachmentId);
}

QString AttachmentManager::getAttachmentFilePath(const attachment& att) const {
    return m_attachmentStoragePath + "/" + QString::fromStdString(att.filepath);
}

bool AttachmentManager::attachmentExists(size_t attachmentId) {
    attachment att = getAttachment(attachmentId);
    if (att.id == 0) {
        return false;
    }
    
    QString filePath = getAttachmentFilePath(att);
    return QFile::exists(filePath);
}

QString AttachmentManager::getMimeType(const QString& filePath) const {
    QMimeDatabase db;
    QMimeType mime = db.mimeTypeForFile(filePath);
    return mime.name();
}

qint64 AttachmentManager::getFileSize(const QString& filePath) const {
    QFileInfo fileInfo(filePath);
    return fileInfo.size();
}

QString AttachmentManager::generateUniqueFilename(const QString& originalFilename) const {
    QFileInfo fileInfo(originalFilename);
    QString extension = fileInfo.suffix();
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    
    if (extension.isEmpty()) {
        return uuid;
    } else {
        return uuid + "." + extension;
    }
}

bool AttachmentManager::ensureAttachmentStoragePathExists() const {
    QDir dir(m_attachmentStoragePath);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qCritical() << "Failed to create attachment storage directory:" << m_attachmentStoragePath;
            return false;
        }
    }
    return true;
}

} // namespace business
} // namespace front
} // namespace mail_system