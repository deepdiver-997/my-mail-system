#include "mail_system/front/business/NetworkManager.h"
#include <QDebug>
#include <QHttpMultiPart>
#include <QFile>
#include <QFileInfo>
#include <QUrlQuery>
#include <QSslSocket>

namespace mail_system {
namespace front {
namespace business {

// 初始化静态成员变量
std::unique_ptr<NetworkManager> NetworkManager::s_instance = nullptr;

NetworkManager& NetworkManager::getInstance() {
    if (!s_instance) {
        s_instance.reset(new NetworkManager());
    }
    return *s_instance;
}

NetworkManager::NetworkManager(QObject* parent) : QObject(parent), 
    m_serverUrl("https://localhost:8080"), 
    m_timeout(30000), // 默认30秒超时
    m_useSSL(true) {
    
    // 初始化网络访问管理器
    m_networkManager = new QNetworkAccessManager(this);
    
    // 连接信号槽
    connect(m_networkManager, &QNetworkAccessManager::sslErrors, 
            [this](QNetworkReply* reply, const QList<QSslError>& errors) {
        qWarning() << "SSL Errors:" << errors;
        // 在生产环境中，应该根据实际情况处理SSL错误
        // 这里为了测试方便，忽略SSL错误
        reply->ignoreSslErrors();
    });
}

NetworkManager::~NetworkManager() {
    // 清理资源
    for (auto timer : m_timeoutTimers.values()) {
        timer->stop();
        timer->deleteLater();
    }
    m_timeoutTimers.clear();
}

void NetworkManager::setServerUrl(const QString& serverUrl) {
    m_serverUrl = serverUrl;
}

QString NetworkManager::getServerUrl() const {
    return m_serverUrl;
}

void NetworkManager::setTimeout(int timeout) {
    m_timeout = timeout;
}

int NetworkManager::getTimeout() const {
    return m_timeout;
}

void NetworkManager::setUseSSL(bool useSSL) {
    m_useSSL = useSSL;
}

bool NetworkManager::getUseSSL() const {
    return m_useSSL;
}

void NetworkManager::setCredentials(const QString& username, const QString& password) {
    m_username = username;
    m_password = password;
}

void NetworkManager::clearCredentials() {
    m_username.clear();
    m_password.clear();
}

void NetworkManager::testConnection(std::function<void(bool)> callback) {
    QNetworkRequest request = createRequest("/api/ping");
    
    QNetworkReply* reply = m_networkManager->get(request);
    
    handleReply(reply, [callback](const QByteArray& data, const QString& error) {
        if (!error.isEmpty()) {
            qWarning() << "Connection test failed:" << error;
            callback(false);
            return;
        }
        
        // 检查响应是否为"pong"
        QString response = QString::fromUtf8(data).trimmed();
        callback(response == "pong");
    });
}

void NetworkManager::sendMail(const mail& mail, const std::vector<QString>& attachmentPaths, 
                            std::function<void(bool, const QString&)> callback) {
    QNetworkRequest request = createRequest("/api/mail/send");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "multipart/form-data");
    
    // 创建多部分HTTP请求
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    
    // 添加邮件信息
    QHttpPart fromPart;
    fromPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"from\""));
    fromPart.setBody(QString::fromStdString(mail.from).toUtf8());
    multiPart->append(fromPart);
    
    QHttpPart toPart;
    toPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"to\""));
    toPart.setBody(QString::fromStdString(mail.to).toUtf8());
    multiPart->append(toPart);
    
    QHttpPart headerPart;
    headerPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"header\""));
    headerPart.setBody(QString::fromStdString(mail.header).toUtf8());
    multiPart->append(headerPart);
    
    QHttpPart bodyPart;
    bodyPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"body\""));
    bodyPart.setBody(QString::fromStdString(mail.body).toUtf8());
    multiPart->append(bodyPart);
    
    // 添加附件
    for (const auto& attachmentPath : attachmentPaths) {
        QFileInfo fileInfo(attachmentPath);
        if (!fileInfo.exists() || !fileInfo.isFile()) {
            qWarning() << "Attachment file does not exist or is not a regular file:" << attachmentPath;
            continue;
        }
        
        QFile* file = new QFile(attachmentPath);
        if (!file->open(QIODevice::ReadOnly)) {
            qWarning() << "Failed to open attachment file:" << attachmentPath;
            delete file;
            continue;
        }
        
        QHttpPart attachmentPart;
        QString contentDisposition = QString("form-data; name=\"attachments\"; filename=\"%1\"").arg(fileInfo.fileName());
        attachmentPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(contentDisposition));
        attachmentPart.setBodyDevice(file);
        file->setParent(multiPart); // 设置父对象，以便自动删除
        multiPart->append(attachmentPart);
    }
    
    // 发送请求
    QNetworkReply* reply = m_networkManager->post(request, multiPart);
    multiPart->setParent(reply); // 设置父对象，以便自动删除
    
    handleReply(reply, [callback](const QByteArray& data, const QString& error) {
        if (!error.isEmpty()) {
            callback(false, error);
            return;
        }
        
        // 解析响应
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (jsonDoc.isNull() || !jsonDoc.isObject()) {
            callback(false, "Invalid response format");
            return;
        }
        
        QJsonObject jsonObj = jsonDoc.object();
        bool success = jsonObj.value("success").toBool();
        QString message = jsonObj.value("message").toString();
        
        callback(success, message);
    });
}

void NetworkManager::receiveMail(const std::string& userEmail, 
                               std::function<void(const std::vector<mail>&, const QString&)> callback) {
    QNetworkRequest request = createRequest("/api/mail/receive");
    
    // 添加查询参数
    QUrlQuery query;
    query.addQueryItem("email", QString::fromStdString(userEmail));
    
    QUrl url = request.url();
    url.setQuery(query);
    request.setUrl(url);
    
    QNetworkReply* reply = m_networkManager->get(request);
    
    handleReply(reply, [callback](const QByteArray& data, const QString& error) {
        if (!error.isEmpty()) {
            callback({}, error);
            return;
        }
        
        // 解析响应
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (jsonDoc.isNull() || !jsonDoc.isArray()) {
            callback({}, "Invalid response format");
            return;
        }
        
        QJsonArray jsonArray = jsonDoc.array();
        std::vector<mail> mails;
        
        for (const QJsonValue& value : jsonArray) {
            if (!value.isObject()) {
                continue;
            }
            
            QJsonObject obj = value.toObject();
            mail m;
            m.id = obj.value("id").toInt();
            m.from = obj.value("from").toString().toStdString();
            m.to = obj.value("to").toString().toStdString();
            m.header = obj.value("header").toString().toStdString();
            m.body = obj.value("body").toString().toStdString();
            m.send_time = obj.value("send_time").toInt();
            m.is_draft = obj.value("is_draft").toBool();
            m.is_read = obj.value("is_read").toBool();
            
            mails.push_back(m);
        }
        
        callback(mails, "");
    });
}

void NetworkManager::syncMail(const std::string& userEmail, time_t lastSyncTime, 
                            std::function<void(bool, const QString&)> callback) {
    QNetworkRequest request = createRequest("/api/mail/sync");
    
    // 添加查询参数
    QUrlQuery query;
    query.addQueryItem("email", QString::fromStdString(userEmail));
    query.addQueryItem("last_sync_time", QString::number(lastSyncTime));
    
    QUrl url = request.url();
    url.setQuery(query);
    request.setUrl(url);
    
    QNetworkReply* reply = m_networkManager->get(request);
    
    handleReply(reply, [this, callback](const QByteArray& data, const QString& error) {
        if (!error.isEmpty()) {
            callback(false, error);
            return;
        }
        
        // 解析响应
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (jsonDoc.isNull() || !jsonDoc.isObject()) {
            callback(false, "Invalid response format");
            return;
        }
        
        QJsonObject jsonObj = jsonDoc.object();
        bool success = jsonObj.value("success").toBool();
        QString message = jsonObj.value("message").toString();
        
        if (success && jsonObj.contains("mails")) {
            QJsonArray mailsArray = jsonObj.value("mails").toArray();
            std::vector<mail> newMails;
            
            for (const QJsonValue& value : mailsArray) {
                if (!value.isObject()) {
                    continue;
                }
                
                QJsonObject obj = value.toObject();
                mail m;
                m.id = obj.value("id").toInt();
                m.from = obj.value("from").toString().toStdString();
                m.to = obj.value("to").toString().toStdString();
                m.header = obj.value("header").toString().toStdString();
                m.body = obj.value("body").toString().toStdString();
                m.send_time = obj.value("send_time").toInt();
                m.is_draft = obj.value("is_draft").toBool();
                m.is_read = obj.value("is_read").toBool();
                
                newMails.push_back(m);
            }
            
            // 发送新邮件接收信号
            if (!newMails.empty()) {
                emit newMailsReceived(newMails);
            }
        }
        
        callback(success, message);
    });
}

void NetworkManager::downloadAttachment(size_t attachmentId, const QString& targetPath, 
                                      std::function<void(bool, const QString&)> callback) {
    QNetworkRequest request = createRequest(QString("/api/attachment/%1/download").arg(attachmentId));
    
    QNetworkReply* reply = m_networkManager->get(request);
    
    // 创建目标文件
    QFile* file = new QFile(targetPath);
    if (!file->open(QIODevice::WriteOnly)) {
        qCritical() << "Failed to open target file for writing:" << targetPath;
        reply->abort();
        reply->deleteLater();
        delete file;
        callback(false, "Failed to open target file for writing");
        return;
    }
    
    // 连接数据接收信号
    connect(reply, &QNetworkReply::readyRead, [reply, file]() {
        file->write(reply->readAll());
    });
    
    // 连接完成信号
    connect(reply, &QNetworkReply::finished, [reply, file, callback]() {
        file->close();
        
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Download attachment failed:" << reply->errorString();
            file->remove(); // 删除不完整的文件
            delete file;
            callback(false, reply->errorString());
            reply->deleteLater();
            return;
        }
        
        delete file;
        callback(true, "");
        reply->deleteLater();
    });
    
    // 设置超时
    handleTimeout(reply);
}

void NetworkManager::uploadAttachment(size_t mailId, const QString& filePath, 
                                    std::function<void(bool, const QString&)> callback) {
    QNetworkRequest request = createRequest(QString("/api/mail/%1/attachment").arg(mailId));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "multipart/form-data");
    
    // 创建多部分HTTP请求
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    
    // 添加文件
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qWarning() << "File does not exist or is not a regular file:" << filePath;
        callback(false, "File does not exist or is not a regular file");
        delete multiPart;
        return;
    }
    
    QFile* file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open file:" << filePath;
        callback(false, "Failed to open file");
        delete file;
        delete multiPart;
        return;
    }
    
    QHttpPart filePart;
    QString contentDisposition = QString("form-data; name=\"file\"; filename=\"%1\"").arg(fileInfo.fileName());
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(contentDisposition));
    filePart.setBodyDevice(file);
    file->setParent(multiPart); // 设置父对象，以便自动删除
    multiPart->append(filePart);
    
    // 发送请求
    QNetworkReply* reply = m_networkManager->post(request, multiPart);
    multiPart->setParent(reply); // 设置父对象，以便自动删除
    
    handleReply(reply, [callback](const QByteArray& data, const QString& error) {
        if (!error.isEmpty()) {
            callback(false, error);
            return;
        }
        