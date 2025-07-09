#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include "mail_system/back/entities/usr.h"
#include "mail_system/front/business/DatabaseManager.h"

namespace mail_system {
namespace front {
namespace business {

/**
 * @brief 用户管理器类，负责处理用户相关的业务逻辑
 * 
 * 该类处理用户的注册、登录、信息修改等操作
 */
class UserManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 获取UserManager的单例实例
     * @return UserManager的单例实例引用
     */
    static UserManager& getInstance();

    /**
     * @brief 用户注册
     * @param email 邮箱地址
     * @param password 密码
     * @param name 用户名
     * @param telephone 电话号码
     * @return 注册是否成功
     */
    bool registerUser(const std::string& email, const std::string& password, 
                     const std::string& name, const std::string& telephone);

    /**
     * @brief 用户登录
     * @param email 邮箱地址
     * @param password 密码
     * @return 登录是否成功
     */
    bool login(const std::string& email, const std::string& password);

    /**
     * @brief 用户登出
     */
    void logout();

    /**
     * @brief 修改用户密码
     * @param userId 用户ID
     * @param oldPassword 旧密码
     * @param newPassword 新密码
     * @return 修改是否成功
     */
    bool changePassword(size_t userId, const std::string& oldPassword, const std::string& newPassword);

    /**
     * @brief 修改用户信息
     * @param user 用户信息
     * @return 修改是否成功
     */
    bool updateUserInfo(const usr& user);

    /**
     * @brief 获取当前登录用户
     * @return 当前登录用户信息
     */
    usr getCurrentUser() const;

    /**
     * @brief 检查用户是否已登录
     * @return 是否已登录
     */
    bool isLoggedIn() const;

signals:
    /**
     * @brief 用户登录成功信号
     * @param user 登录成功的用户信息
     */
    void loginSuccessful(const usr& user);

    /**
     * @brief 用户登出信号
     */
    void logoutSuccessful();

    /**
     * @brief 用户信息更新信号
     * @param user 更新后的用户信息
     */
    void userInfoUpdated(const usr& user);

private:
    // 私有构造函数和析构函数，防止外部创建实例
    UserManager(QObject* parent = nullptr);
    ~UserManager();
    
    // 禁用拷贝构造函数和赋值操作符
    UserManager(const UserManager&) = delete;
    UserManager& operator=(const UserManager&) = delete;

    /**
     * @brief 加密密码
     * @param password 原始密码
     * @return 加密后的密码
     */
    std::string encryptPassword(const std::string& password);

    // 当前登录用户
    usr m_currentUser;
    bool m_isLoggedIn;
    
    // 单例实例
    static std::unique_ptr<UserManager> s_instance;
};

} // namespace business
} // namespace front
} // namespace mail_system