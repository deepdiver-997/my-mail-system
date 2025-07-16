#include "mail_system/front/business/UserManager.h"
#include <QCryptographicHash>
#include <QDateTime>

namespace mail_system {
namespace front {
namespace business {

// 初始化静态成员变量
std::unique_ptr<UserManager> UserManager::s_instance = nullptr;

UserManager& UserManager::getInstance() {
    if (!s_instance) {
        s_instance.reset(new UserManager());
    }
    return *s_instance;
}

UserManager::UserManager(QObject* parent) : QObject(parent), m_isLoggedIn(false) {
    // 初始化
}

UserManager::~UserManager() {
    // 清理资源
}

bool UserManager::registerUser(const std::string& email, const std::string& password, 
                              const std::string& name, const std::string& telephone) {
    // 检查邮箱是否已存在
    usr existingUser = DatabaseManager::getInstance().getUserByEmail(email);
    if (!existingUser.mailAddress.empty()) {
        qWarning() << "Email already exists:" << QString::fromStdString(email);
        return false;
    }
    
    // 创建新用户
    usr newUser;
    newUser.mailAddress = email;
    newUser.password = encryptPassword(password);
    newUser.name = name;
    newUser.telephone = telephone;
    newUser.registerTime = QDateTime::currentSecsSinceEpoch();
    
    // 添加用户到数据库
    return DatabaseManager::getInstance().addUser(newUser);
}

bool UserManager::login(const std::string& email, const std::string& password) {
    // 验证用户凭据
    if (DatabaseManager::getInstance().validateUserCredentials(email, encryptPassword(password))) {
        // 获取用户信息
        m_currentUser = DatabaseManager::getInstance().getUserByEmail(email);
        m_isLoggedIn = true;
        
        // 发送登录成功信号
        emit loginSuccessful(m_currentUser);
        return true;
    }
    
    return false;
}

void UserManager::logout() {
    if (m_isLoggedIn) {
        m_isLoggedIn = false;
        m_currentUser = usr(); // 清空当前用户信息
        
        // 发送登出成功信号
        emit logoutSuccessful();
    }
}

bool UserManager::changePassword(size_t userId, const std::string& oldPassword, const std::string& newPassword) {
    // 获取用户信息
    usr user = DatabaseManager::getInstance().getUserById(userId);
    if (user.id == 0) {
        qWarning() << "User not found with id:" << userId;
        return false;
    }
    
    // 验证旧密码
    if (user.password != encryptPassword(oldPassword)) {
        qWarning() << "Old password is incorrect";
        return false;
    }
    
    // 更新密码
    user.password = encryptPassword(newPassword);
    return DatabaseManager::getInstance().updateUser(user);
}

bool UserManager::updateUserInfo(const usr& user) {
    // 更新用户信息
    bool result = DatabaseManager::getInstance().updateUser(user);
    
    // 如果更新成功且是当前登录用户，更新当前用户信息
    if (result && m_isLoggedIn && m_currentUser.id == user.id) {
        m_currentUser = user;
        emit userInfoUpdated(m_currentUser);
    }
    
    return result;
}

usr UserManager::getCurrentUser() const {
    return m_currentUser;
}

bool UserManager::isLoggedIn() const {
    return m_isLoggedIn;
}

std::string UserManager::encryptPassword(const std::string& password) {
    // 使用SHA-256加密密码
    QByteArray passwordData = QByteArray::fromStdString(password);
    QByteArray hashedPassword = QCryptographicHash::hash(passwordData, QCryptographicHash::Sha256);
    return hashedPassword.toHex().toStdString();
}

} // namespace business
} // namespace front
} // namespace mail_system