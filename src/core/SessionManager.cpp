// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "SessionManager.h"

#include <QDir>
#include <QStandardPaths>
#include <QDebug>

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
    // Ensure profiles directory exists
    QDir().mkpath(profilesDir());

    // Start with a new session
    newSession();

    // Load existing profiles
    refreshProfiles();
}

SessionManager::~SessionManager() = default;

QString SessionManager::profilesDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) 
           + QStringLiteral("/profiles");
}

QString SessionManager::profilePath(const QString &name) const
{
    return profilesDir() + QStringLiteral("/") + name + QStringLiteral(".conf");
}

void SessionManager::newSession()
{
    m_currentProfile = SessionProfile();
    m_currentProfile.name = QString();
    m_currentProfile.layout = QStringLiteral("horizontal");

    // Default: 2 instances
    m_currentProfile.instances.clear();
    for (int i = 0; i < 2; ++i) {
        InstanceConfig config;
        config.monitor = 0;
        m_currentProfile.instances.append(config);
    }

    Q_EMIT currentProfileChanged();
    Q_EMIT currentLayoutChanged();
    Q_EMIT instanceCountChanged();
    Q_EMIT instancesChanged();
}

void SessionManager::refreshProfiles()
{
    m_savedProfiles.clear();

    QDir dir(profilesDir());
    QStringList filters;
    filters << QStringLiteral("*.conf");

    for (const QString &fileName : dir.entryList(filters, QDir::Files)) {
        QString name = fileName;
        name.chop(5); // Remove ".conf"

        SessionProfile profile;
        profile.name = name;
        profile.filePath = dir.absoluteFilePath(fileName);

        // Read basic info
        KConfig config(profile.filePath);
        KConfigGroup general = config.group(QStringLiteral("General"));
        profile.layout = general.readEntry("layout", QStringLiteral("horizontal"));

        m_savedProfiles.append(profile);
    }

    Q_EMIT savedProfilesChanged();
}

bool SessionManager::saveProfile(const QString &name)
{
    if (name.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Profile name cannot be empty"));
        return false;
    }

    QString path = profilePath(name);
    KConfig config(path);

    // General section
    KConfigGroup general = config.group(QStringLiteral("General"));
    general.writeEntry("name", name);
    general.writeEntry("layout", m_currentProfile.layout);
    general.writeEntry("instanceCount", m_currentProfile.instances.size());

    // Instance sections
    for (int i = 0; i < m_currentProfile.instances.size(); ++i) {
        const InstanceConfig &inst = m_currentProfile.instances[i];
        KConfigGroup instGroup = config.group(QStringLiteral("Instance%1").arg(i));

        instGroup.writeEntry("username", inst.username);
        instGroup.writeEntry("monitor", inst.monitor);
        instGroup.writeEntry("internalWidth", inst.internalWidth);
        instGroup.writeEntry("internalHeight", inst.internalHeight);
        instGroup.writeEntry("outputWidth", inst.outputWidth);
        instGroup.writeEntry("outputHeight", inst.outputHeight);
        instGroup.writeEntry("refreshRate", inst.refreshRate);
        instGroup.writeEntry("scalingMode", inst.scalingMode);
        instGroup.writeEntry("filterMode", inst.filterMode);
        instGroup.writeEntry("gameCommand", inst.gameCommand);
        instGroup.writeEntry("steamAppId", inst.steamAppId);
        instGroup.writeEntry("launchMode", inst.launchMode);

        // Convert devices to string list
        QStringList deviceStrings;
        for (int dev : inst.devices) {
            deviceStrings << QString::number(dev);
        }
        instGroup.writeEntry("devices", deviceStrings);
    }

    config.sync();

    m_currentProfile.name = name;
    m_currentProfile.filePath = path;

    Q_EMIT currentProfileChanged();
    refreshProfiles();

    return true;
}

bool SessionManager::loadProfile(const QString &name)
{
    QString path = profilePath(name);

    if (!QFile::exists(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Profile not found: %1").arg(name));
        return false;
    }

    KConfig config(path);

    // Read general section
    KConfigGroup general = config.group(QStringLiteral("General"));
    m_currentProfile.name = name;
    m_currentProfile.filePath = path;
    m_currentProfile.layout = general.readEntry("layout", QStringLiteral("horizontal"));
    int instanceCount = general.readEntry("instanceCount", 2);

    // Read instances
    m_currentProfile.instances.clear();
    for (int i = 0; i < instanceCount; ++i) {
        KConfigGroup instGroup = config.group(QStringLiteral("Instance%1").arg(i));

        InstanceConfig inst;
        inst.username = instGroup.readEntry("username", QString());
        inst.monitor = instGroup.readEntry("monitor", 0);
        inst.internalWidth = instGroup.readEntry("internalWidth", 1920);
        inst.internalHeight = instGroup.readEntry("internalHeight", 1080);
        inst.outputWidth = instGroup.readEntry("outputWidth", 960);
        inst.outputHeight = instGroup.readEntry("outputHeight", 1080);
        inst.refreshRate = instGroup.readEntry("refreshRate", 60);
        inst.scalingMode = instGroup.readEntry("scalingMode", QStringLiteral("fit"));
        inst.filterMode = instGroup.readEntry("filterMode", QStringLiteral("linear"));
        inst.gameCommand = instGroup.readEntry("gameCommand", QString());
        inst.steamAppId = instGroup.readEntry("steamAppId", QString());
        inst.launchMode = instGroup.readEntry("launchMode", QStringLiteral("direct"));

        // Read devices
        QStringList deviceStrings = instGroup.readEntry("devices", QStringList());
        for (const QString &devStr : deviceStrings) {
            inst.devices << devStr.toInt();
        }

        m_currentProfile.instances.append(inst);
    }

    Q_EMIT currentProfileChanged();
    Q_EMIT currentLayoutChanged();
    Q_EMIT instanceCountChanged();
    Q_EMIT instancesChanged();

    return true;
}

bool SessionManager::deleteProfile(const QString &name)
{
    QString path = profilePath(name);

    if (!QFile::exists(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Profile not found: %1").arg(name));
        return false;
    }

    if (!QFile::remove(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to delete profile: %1").arg(name));
        return false;
    }

    // Clear current profile name if we just deleted the current profile
    if (m_currentProfile.name == name) {
        m_currentProfile.name = QString();
        m_currentProfile.filePath = QString();
        Q_EMIT currentProfileChanged();
    }

    refreshProfiles();
    return true;
}

void SessionManager::setCurrentLayout(const QString &layout)
{
    if (m_currentProfile.layout != layout) {
        m_currentProfile.layout = layout;
        Q_EMIT currentLayoutChanged();
    }
}

void SessionManager::setInstanceCount(int count)
{
    if (count < 2) count = 2; // Minimum 2 for split-screen
    if (count > 4) count = 4; // Max 4 for now

    if (m_currentProfile.instances.size() == count) {
        return; // No change
    }

    while (m_currentProfile.instances.size() < count) {
        InstanceConfig config;
        config.monitor = 0;
        m_currentProfile.instances.append(config);
    }

    while (m_currentProfile.instances.size() > count) {
        m_currentProfile.instances.removeLast();
    }

    Q_EMIT instanceCountChanged();
    Q_EMIT instancesChanged();
}

QVariantMap SessionManager::getInstanceConfig(int index) const
{
    QVariantMap map;

    if (index < 0 || index >= m_currentProfile.instances.size()) {
        return map;
    }

    const InstanceConfig &inst = m_currentProfile.instances[index];
    map[QStringLiteral("username")] = inst.username;
    map[QStringLiteral("monitor")] = inst.monitor;
    map[QStringLiteral("internalWidth")] = inst.internalWidth;
    map[QStringLiteral("internalHeight")] = inst.internalHeight;
    map[QStringLiteral("outputWidth")] = inst.outputWidth;
    map[QStringLiteral("outputHeight")] = inst.outputHeight;
    map[QStringLiteral("refreshRate")] = inst.refreshRate;
    map[QStringLiteral("scalingMode")] = inst.scalingMode;
    map[QStringLiteral("filterMode")] = inst.filterMode;
    map[QStringLiteral("gameCommand")] = inst.gameCommand;
    map[QStringLiteral("steamAppId")] = inst.steamAppId;
    map[QStringLiteral("launchMode")] = inst.launchMode;

    QVariantList deviceList;
    for (int dev : inst.devices) {
        deviceList << dev;
    }
    map[QStringLiteral("devices")] = deviceList;

    return map;
}

void SessionManager::setInstanceConfig(int index, const QVariantMap &config)
{
    if (index < 0 || index >= m_currentProfile.instances.size()) {
        return;
    }

    InstanceConfig &inst = m_currentProfile.instances[index];

    if (config.contains(QStringLiteral("username")))
        inst.username = config[QStringLiteral("username")].toString();
    if (config.contains(QStringLiteral("monitor")))
        inst.monitor = config[QStringLiteral("monitor")].toInt();
    if (config.contains(QStringLiteral("internalWidth")))
        inst.internalWidth = config[QStringLiteral("internalWidth")].toInt();
    if (config.contains(QStringLiteral("internalHeight")))
        inst.internalHeight = config[QStringLiteral("internalHeight")].toInt();
    if (config.contains(QStringLiteral("outputWidth")))
        inst.outputWidth = config[QStringLiteral("outputWidth")].toInt();
    if (config.contains(QStringLiteral("outputHeight")))
        inst.outputHeight = config[QStringLiteral("outputHeight")].toInt();
    if (config.contains(QStringLiteral("refreshRate")))
        inst.refreshRate = config[QStringLiteral("refreshRate")].toInt();
    if (config.contains(QStringLiteral("scalingMode")))
        inst.scalingMode = config[QStringLiteral("scalingMode")].toString();
    if (config.contains(QStringLiteral("filterMode")))
        inst.filterMode = config[QStringLiteral("filterMode")].toString();
    if (config.contains(QStringLiteral("gameCommand")))
        inst.gameCommand = config[QStringLiteral("gameCommand")].toString();
    if (config.contains(QStringLiteral("steamAppId")))
        inst.steamAppId = config[QStringLiteral("steamAppId")].toString();
    if (config.contains(QStringLiteral("launchMode")))
        inst.launchMode = config[QStringLiteral("launchMode")].toString();

    Q_EMIT instancesChanged();
}

void SessionManager::setInstanceUser(int index, const QString &username)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].username = username;
        Q_EMIT instancesChanged();
    }
}

void SessionManager::setInstanceMonitor(int index, int monitor)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].monitor = monitor;
        Q_EMIT instancesChanged();
    }
}

void SessionManager::setInstanceResolution(int index, int internalW, int internalH, int outputW, int outputH)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        InstanceConfig &inst = m_currentProfile.instances[index];
        inst.internalWidth = internalW;
        inst.internalHeight = internalH;
        inst.outputWidth = outputW;
        inst.outputHeight = outputH;
        Q_EMIT instancesChanged();
    }
}

void SessionManager::setInstanceDevices(int index, const QList<int> &devices)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].devices = devices;
        Q_EMIT instancesChanged();
    }
}

void SessionManager::setInstanceGame(int index, const QString &gameCommand)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].gameCommand = gameCommand;
        Q_EMIT instancesChanged();
    }
}

QVariantList SessionManager::savedProfilesAsVariant() const
{
    QVariantList list;
    for (const auto &profile : m_savedProfiles) {
        QVariantMap map;
        map[QStringLiteral("name")] = profile.name;
        map[QStringLiteral("layout")] = profile.layout;
        map[QStringLiteral("filePath")] = profile.filePath;
        list.append(map);
    }
    return list;
}

QVariantList SessionManager::instancesAsVariant() const
{
    QVariantList list;
    for (int i = 0; i < m_currentProfile.instances.size(); ++i) {
        list.append(getInstanceConfig(i));
    }
    return list;
}

void SessionManager::recalculateOutputResolutions(int screenWidth, int screenHeight)
{
    int count = m_currentProfile.instances.size();
    if (count < 1) {
        return;
    }

    QString layout = m_currentProfile.layout;

    for (int i = 0; i < count; ++i) {
        InstanceConfig &inst = m_currentProfile.instances[i];

        if (layout == QStringLiteral("horizontal")) {
            inst.outputWidth = screenWidth / count;
            inst.outputHeight = screenHeight;
        } else if (layout == QStringLiteral("vertical")) {
            inst.outputWidth = screenWidth;
            inst.outputHeight = screenHeight / count;
        } else if (layout == QStringLiteral("grid")) {
            int cols = (count <= 2) ? count : 2;
            int rows = (count + cols - 1) / cols;
            inst.outputWidth = screenWidth / cols;
            inst.outputHeight = screenHeight / rows;
        } else {
            // multi-monitor or unknown: use full resolution
            inst.outputWidth = screenWidth;
            inst.outputHeight = screenHeight;
        }

        // Also set internal resolution to match output by default
        // This avoids unnecessary scaling and gives best performance
        inst.internalWidth = inst.outputWidth;
        inst.internalHeight = inst.outputHeight;
    }

    Q_EMIT instancesChanged();
}
