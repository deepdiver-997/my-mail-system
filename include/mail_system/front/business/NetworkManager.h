#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include "mail_system/back/entities/mail.h"

namespace mail_system {
namespace front {
namespace business {

/**
 * @brief 网络管理器类，负责处理网络通信相关的业务逻辑
 * 
 * 该类处理与服务器的通信、邮件的发送和接收等操作
 */
class NetworkManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 获取NetworkManager的单例实例
     * @return NetworkManager的单例实例引用
     */
    static NetworkManager& getInstance();

    /**
     * @brief 设置服务器地址
     * @param serverUrl 服务器地址
     */
    void setServerUrl(const QString& serverUrl);

    /**
     * @brief 获取服务器地址
     * @return 服务器地址
     */
    QString getServerUrl() const;

    /**
     * @brief 设置连接超时时间
     * @param timeout 超时时间（毫秒）
     */
    void setTimeout(int timeout);

    /**
     * @brief 获取连接超时时间
     * @return 超时时间（毫秒）
     */
    int getTimeout() const;

    /**
     * @brief 设置是否使用SSL
     * @param useSSL 是否使用SSL
     */
    void setUseSSL(bool useSSL);

    /**
     * @brief 获取是否使用SSL
     * @return 是否使用SSL
     */
    bool getUseSSL() const;

    /**
     * @brief 设置用户凭据
     * @param username 用户名
     * @param password 密码
     */
    void setCredentials(const QString& username, const QString& password);

    /**
     * @brief 清除用户凭据
     */
    void clearCredentials();

    /**
     * @brief 测试服务器连接
     * @param callback 回调函数，参数为连接是否成功
     */
    void testConnection(std::function<void(bool)> callback);

    /**
     * @brief 发送邮件
     * @param mail 邮件信息
     * @param attachmentPaths 附件路径列表
     * @param callback 回调函数，参数为发送是否成功和错误信息
     */
    void sendMail(const mail& mail, const std::vector<QString>& attachmentPaths, 
                 std::function<void(bool, const QString&)> callback);

    /**
     * @brief 接收邮件
     * @param userEmail 用户邮箱地址
     * @param callback 回调函数，参数为接收到的邮件列表和错误信息
     */
    void receiveMail(const std::string& userEmail, 
                    std::function<void(const std::vector<mail>&, const QString&)> callback);

    /**
     * @brief 同步邮件
     * @param userEmail 用户邮箱地址
     * @param lastSyncTime 上次同步时间
     * @param callback 回调函数，参数为同步是否成功和错误信息
     */
    void syncMail(const std::string& userEmail, time_t lastSyncTime, 
                 std::function<void(bool, const QString&)> callback);

    /**
     * @brief 下载附件
     * @param attachmentId 附件ID
     * @param targetPath 目标路径
     * @param callback 回调函数，参数为下载是否成功和错误信息
     */
    void downloadAttachment(size_t attachmentId, const QString& targetPath, 
                           std::function<void(bool, const QString&)> callback);

    /**
     * @brief 上传附件
     * @param mailId 邮件ID
     * @param filePath 文件路径
     * @param callback 回调函数，参数为上传是否成功和错误信息
     */
    void uploadAttachment(size_t mailId, const QString& filePath, 
                         std::function<void(bool, const QString&)> callback);

signals:
    /**
     * @brief 网络错误信号
     * @param errorMessage 错误信息
     */
    void networkError(const QString& errorMessage);

    /**
     * @brief 连接状态变化信号
     * @param isConnected 是否已连接
     */
    void connectionStatusChanged(bool isConnected);

    /**
     * @brief 收到新邮件信号
     * @param newMails 新邮件列表
     */
    void newMailsReceived(const std::vector<mail>& newMails);

private:
    // 私有构造函数和析构函数，防止外部创建实例
    NetworkManager(QObject* parent = nullptr);
    ~NetworkManager() noexcept;

    // 禁用拷贝构造函数和赋值操作符
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    /**
     * @brief 创建网络请求
     * @param endpoint API端点
     * @return 网络请求对象
     */
    QNetworkRequest createRequest(const QString& endpoint);

    /**
     * @brief 处理网络回复
     * @param reply 网络回复对象
     * @param callback 回调函数，参数为回复数据和错误信息
     */
    void handleReply(QNetworkReply* reply, 
                    std::function<void(const QByteArray&, const QString&)> callback);

    /**
     * @brief 处理超时
     * @param reply 网络回复对象
     */
    void handleTimeout(QNetworkReply* reply);

    // 服务器地址
    QString m_serverUrl;
    
    // 连接超时时间（毫秒）
    int m_timeout;
    
    // 是否使用SSL
    bool m_useSSL;
    
    // 用户凭据
    QString m_username;
    QString m_password;
    
    // 网络访问管理器
    QNetworkAccessManager* m_networkManager;
    
    // 超时定时器映射
    QMap<QNetworkReply*, QTimer*> m_timeoutTimers;
    
    // 单例实例
    static std::unique_ptr<NetworkManager> s_instance;
};

} // namespace business
} // namespace front
} // namespace mail_system