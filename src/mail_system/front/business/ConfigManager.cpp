#include "mail_system/front/business/ConfigManager.h"
#include <QDir>
#include <QStandardPaths>
#include <QDebug>


namespace mail_system {
namespace front {
namespace business {

// 初始化静态成员变量
std::unique_ptr<ConfigManager> ConfigManager::s_instance = nullptr;

ConfigManager& ConfigManager::getInstance() {
    if (!s_instance) {
        s_instance.reset(new ConfigManager());
    }
    return *s_instance;
}

ConfigManager::ConfigManager(QObject* parent) : QObject(parent), 
    m_settings(nullptr), m_initialized(false) {
    // 初始化
}

ConfigManager::~ConfigManager() {
    // 清理资源
    if (m_settings) {
        delete m_settings;
        m_settings = nullptr;
    }
}

bool ConfigManager::initialize(const QString& configFilePath) {
    // 如果已经初始化，先清理旧的设置
    if (m_settings) {
        delete m_settings;
        m_settings = nullptr;
    }
    
    // 设置配置文件路径
    m_configFilePath = configFilePath.isEmpty() ? getDefaultConfigFilePath() : configFilePath;
    
    // 确保配置文件所在目录存在
    QFileInfo fileInfo(m_configFilePath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qCritical() << "Failed to create config directory:" << dir.path();
            return false;
        }
    }
    
    // 创建设置对象
    m_settings = new QSettings(m_configFilePath, QSettings::IniFormat);
    
    // 检查设置对象是否有效
    if (m_settings->status() != QSettings::NoError) {
        qCritical() << "Failed to initialize settings:" << m_settings->status();
        delete m_settings;
        m_settings = nullptr;
        return false;
    }
    
    // 加载默认配置
    loadDefaults();
    
    m_initialized = true;
    return true;
}

bool ConfigManager::saveConfig() {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return false;
    }
    
    m_settings->sync();
    return m_settings->status() == QSettings::NoError;
}

bool ConfigManager::resetToDefaults() {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return false;
    }
    
    // 清除所有设置
    m_settings->clear();
    
    // 加载默认配置
    loadDefaults();
    
    // 保存设置
    return saveConfig();
}

QString ConfigManager::getString(const QString& key, const QString& defaultValue) const {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return defaultValue;
    }
    
    return m_settings->value(key, defaultValue).toString();
}

void ConfigManager::setString(const QString& key, const QString& value) {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return;
    }
    
    m_settings->setValue(key, value);
    emit configChanged(key, value);
}

int ConfigManager::getInt(const QString& key, int defaultValue) const {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return defaultValue;
    }
    
    return m_settings->value(key, defaultValue).toInt();
}

void ConfigManager::setInt(const QString& key, int value) {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return;
    }
    
    m_settings->setValue(key, value);
    emit configChanged(key, value);
}

bool ConfigManager::getBool(const QString& key, bool defaultValue) const {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return defaultValue;
    }
    
    return m_settings->value(key, defaultValue).toBool();
}

void ConfigManager::setBool(const QString& key, bool value) {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return;
    }
    
    m_settings->setValue(key, value);
    emit configChanged(key, value);
}

double ConfigManager::getDouble(const QString& key, double defaultValue) const {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return defaultValue;
    }
    
    return m_settings->value(key, defaultValue).toDouble();
}

void ConfigManager::setDouble(const QString& key, double value) {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return;
    }
    
    m_settings->setValue(key, value);
    emit configChanged(key, value);
}

bool ConfigManager::containsKey(const QString& key) const {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return false;
    }
    
    return m_settings->contains(key);
}

void ConfigManager::removeKey(const QString& key) {
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return;
    }
    
    m_settings->remove(key);
    emit configChanged(key, QVariant());
}

std::map<QString, QVariant> ConfigManager::getAllSettings() const {
    std::map<QString, QVariant> result;
    
    if (!m_initialized || !m_settings) {
        qWarning() << "ConfigManager not initialized";
        return result;
    }
    
    // 获取所有键
    QStringList keys = m_settings->allKeys();
    
    // 遍历所有键，获取对应的值
    for (const QString& key : keys) {
        result[key] = m_settings->value(key);
    }
    
    return result;
}

QString ConfigManager::getConfigFilePath() const {
    return m_configFilePath;
}

void ConfigManager::loadDefaults() {
    if (!m_settings) {
        return;
    }
    
    // 只在键不存在时设置默认值
    if (!m_settings->contains("General/Language")) {
        m_settings->setValue("General/Language", "en");
    }
    
    if (!m_settings->contains("General/Theme")) {
        m_settings->setValue("General/Theme", "light");
    }
    
    if (!m_settings->contains("Network/ServerUrl")) {
        m_settings->setValue("Network/ServerUrl", "https://localhost:8080");
    }
    
    if (!m_settings->contains("Network/Timeout")) {
        m_settings->setValue("Network/Timeout", 30000);
    }
    
    if (!m_settings->contains("Network/UseSSL")) {
        m_settings->setValue("Network/UseSSL", true);
    }
    
    if (!m_settings->contains("Mail/CheckInterval")) {
        m_settings->setValue("Mail/CheckInterval", 300); // 5分钟
    }
    
    if (!m_settings->contains("Mail/MaxAttachmentSize")) {
        m_settings->setValue("Mail/MaxAttachmentSize", 10 * 1024 * 1024); // 10MB
    }
    
    if (!m_settings->contains("UI/ShowNotifications")) {
        m_settings->setValue("UI/ShowNotifications", true);
    }
    
    if (!m_settings->contains("UI/NotificationSound")) {
        m_settings->setValue("UI/NotificationSound", true);
    }
    
    if (!m_settings->contains("UI/FontSize")) {
        m_settings->setValue("UI/FontSize", 12);
    }
    
    if (!m_settings->contains("Attachments/StoragePath")) {
        QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/attachments";
        m_settings->setValue("Attachments/StoragePath", defaultPath);
    }
}

QString ConfigManager::getDefaultConfigFilePath() const {
    // 获取应用数据目录
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    
    // 创建配置文件路径
    return appDataPath + "/config.ini";
}

} // namespace business
} // namespace front
} // namespace mail_system