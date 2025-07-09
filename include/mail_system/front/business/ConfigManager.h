#pragma once

#include <QObject>
#include <QString>
#include <QSettings>
#include <QVariant>
#include <memory>
#include <string>
#include <map>

namespace mail_system {
namespace front {
namespace business {

/**
 * @brief 配置管理器类，负责处理配置相关的业务逻辑
 * 
 * 该类处理应用程序配置的读取、保存和更新
 */
class ConfigManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 获取ConfigManager的单例实例
     * @return ConfigManager的单例实例引用
     */
    static ConfigManager& getInstance();

    /**
     * @brief 初始化配置管理器
     * @param configFilePath 配置文件路径，如果为空则使用默认路径
     * @return 初始化是否成功
     */
    bool initialize(const QString& configFilePath = "");

    /**
     * @brief 保存配置
     * @return 保存是否成功
     */
    bool saveConfig();

    /**
     * @brief 重置配置为默认值
     * @return 重置是否成功
     */
    bool resetToDefaults();

    /**
     * @brief 获取字符串配置项
     * @param key 配置项键
     * @param defaultValue 默认值
     * @return 配置项值
     */
    QString getString(const QString& key, const QString& defaultValue = "") const;

    /**
     * @brief 设置字符串配置项
     * @param key 配置项键
     * @param value 配置项值
     */
    void setString(const QString& key, const QString& value);

    /**
     * @brief 获取整数配置项
     * @param key 配置项键
     * @param defaultValue 默认值
     * @return 配置项值
     */
    int getInt(const QString& key, int defaultValue = 0) const;

    /**
     * @brief 设置整数配置项
     * @param key 配置项键
     * @param value 配置项值
     */
    void setInt(const QString& key, int value);

    /**
     * @brief 获取布尔配置项
     * @param key 配置项键
     * @param defaultValue 默认值
     * @return 配置项值
     */
    bool getBool(const QString& key, bool defaultValue = false) const;

    /**
     * @brief 设置布尔配置项
     * @param key 配置项键
     * @param value 配置项值
     */
    void setBool(const QString& key, bool value);

    /**
     * @brief 获取浮点数配置项
     * @param key 配置项键
     * @param defaultValue 默认值
     * @return 配置项值
     */
    double getDouble(const QString& key, double defaultValue = 0.0) const;

    /**
     * @brief 设置浮点数配置项
     * @param key 配置项键
     * @param value 配置项值
     */
    void setDouble(const QString& key, double value);

    /**
     * @brief 检查配置项是否存在
     * @param key 配置项键
     * @return 配置项是否存在
     */
    bool containsKey(const QString& key) const;

    /**
     * @brief 移除配置项
     * @param key 配置项键
     */
    void removeKey(const QString& key);

    /**
     * @brief 获取所有配置项
     * @return 所有配置项的键值对
     */
    std::map<QString, QVariant> getAllSettings() const;

    /**
     * @brief 获取配置文件路径
     * @return 配置文件路径
     */
    QString getConfigFilePath() const;

signals:
    /**
     * @brief 配置项变更信号
     * @param key 变更的配置项键
     * @param value 新的配置项值
     */
    void configChanged(const QString& key, const QVariant& value);

private:
    // 私有构造函数和析构函数，防止外部创建实例
    ConfigManager(QObject* parent = nullptr);
    ~ConfigManager() noexcept;
    
    // 禁用拷贝构造函数和赋值操作符
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /**
     * @brief 加载默认配置
     */
    void loadDefaults();

    /**
     * @brief 获取默认配置文件路径
     * @return 默认配置文件路径
     */
    QString getDefaultConfigFilePath() const;
    
    // 配置对象
    QSettings* m_settings;
    
    // 配置文件路径
    QString m_configFilePath;
    
    // 是否已初始化
    bool m_initialized;
    
    // 单例实例
    static std::unique_ptr<ConfigManager> s_instance;
};

} // namespace business
} // namespace front
} // namespace mail_system