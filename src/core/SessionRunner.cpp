// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SessionRunner.h"
#include "GamescopeInstance.h"
#include "Logging.h"
#include "SessionManager.h"
#include "SettingsManager.h"
#include "DeviceManager.h"
#include "PresetManager.h"
#include "SteamConfigManager.h"
#include "WindowManager.h"
#include "../dbus/CouchPlayHelperClient.h"

#include <QAction>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>

#include <KGlobalAccel>
#include <KLocalizedString>

#include <pwd.h>
#include <grp.h>
#include <unistd.h>

static const QString COUCHPLAY_GROUP = QStringLiteral("couchplay");

static bool isUserInCouchPlayGroup(const QString &username)
{
    struct group *grp = getgrnam(COUCHPLAY_GROUP.toLocal8Bit().constData());
    if (!grp) {
        return false;
    }

    for (char **member = grp->gr_mem; *member != nullptr; ++member) {
        if (username == QString::fromLocal8Bit(*member)) {
            return true;
        }
    }

    // Primary group check: getgrnam(3) only returns supplementary members in gr_mem
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    if (pw && pw->pw_gid == grp->gr_gid) {
        return true;
    }

    return false;
}

SessionRunner::SessionRunner(QObject *parent)
    : QObject(parent)
    , m_windowManager(new WindowManager(this))
{
    setStatus(QStringLiteral("Ready"));
    setupGlobalShortcut();
    
    m_virtualDeviceWatcher = new VirtualDeviceWatcher(this);
    connect(m_virtualDeviceWatcher, &VirtualDeviceWatcher::virtualDeviceAppeared,
            this, &SessionRunner::onVirtualDeviceAppeared);
    
    connect(m_windowManager, &WindowManager::gamescopeWindowPositioned,
            this, &SessionRunner::onWindowPositioned);
    connect(m_windowManager, &WindowManager::positioningTimedOut,
            this, &SessionRunner::onWindowPositioningTimeout);
}

SessionRunner::~SessionRunner()
{
    stop();
}

void SessionRunner::setStatus(const QString &status)
{
    if (m_status != status) {
        m_status = status;
        Q_EMIT statusChanged();
    }
}

void SessionRunner::setSessionManager(SessionManager *manager)
{
    if (m_sessionManager != manager) {
        m_sessionManager = manager;
        Q_EMIT sessionManagerChanged();
    }
}

void SessionRunner::setDeviceManager(DeviceManager *manager)
{
    if (m_deviceManager != manager) {
        if (m_deviceManager) {
            disconnect(m_deviceManager, &DeviceManager::deviceReconnected,
                      this, &SessionRunner::onDeviceReconnected);
        }
        
        m_deviceManager = manager;
        
        if (m_deviceManager) {
            connect(m_deviceManager, &DeviceManager::deviceReconnected,
                   this, &SessionRunner::onDeviceReconnected);
        }
        
        Q_EMIT deviceManagerChanged();
    }
}

void SessionRunner::setHelperClient(CouchPlayHelperClient *client)
{
    if (m_helperClient != client) {
        m_helperClient = client;
        Q_EMIT helperClientChanged();
    }
}

void SessionRunner::setPresetManager(PresetManager *manager)
{
    if (m_presetManager != manager) {
        m_presetManager = manager;
        Q_EMIT presetManagerChanged();
    }
}

void SessionRunner::setSteamConfigManager(SteamConfigManager *manager)
{
    if (m_steamConfigManager != manager) {
        m_steamConfigManager = manager;
        Q_EMIT steamConfigManagerChanged();
    }
}

void SessionRunner::setHeroicConfigManager(HeroicConfigManager *manager)
{
    if (m_heroicConfigManager != manager) {
        m_heroicConfigManager = manager;
        Q_EMIT heroicConfigManagerChanged();
    }
}

void SessionRunner::setSettingsManager(SettingsManager *manager)
{
    if (m_settingsManager != manager) {
        m_settingsManager = manager;
        Q_EMIT settingsManagerChanged();
    }
}

bool SessionRunner::start()
{
    if (!m_sessionManager) {
        Q_EMIT errorOccurred(QStringLiteral("No session manager configured"));
        return false;
    }

    if (isRunning()) {
        Q_EMIT errorOccurred(QStringLiteral("Session already running"));
        return false;
    }

    setStatus(QStringLiteral("Starting session..."));

    cleanupInstances();

    const SessionProfile &profile = m_sessionManager->currentProfile();
    int instanceCount = profile.instances.size();

    if (instanceCount < 1) {
        Q_EMIT errorOccurred(QStringLiteral("No instances configured"));
        setStatus(QStringLiteral("Error"));
        return false;
    }

    // Steam can't run multiple instances under the same user
    QSet<QString> usedUsers;
    for (int i = 0; i < instanceCount; ++i) {
        const QString &username = profile.instances[i].username;
        if (!username.isEmpty()) {
            if (usedUsers.contains(username)) {
                Q_EMIT errorOccurred(QStringLiteral("User '%1' is assigned to multiple instances. Each instance needs a unique user.").arg(username));
                setStatus(QStringLiteral("Error"));
                return false;
            }
            usedUsers.insert(username);
        }
    }

    struct passwd *compositorPw = getpwuid(getuid());
    QString compositorUser = compositorPw ? QString::fromLocal8Bit(compositorPw->pw_name) : QString();
    
    for (int i = 0; i < instanceCount; ++i) {
        const QString &username = profile.instances[i].username;
        if (username.isEmpty()) {
            continue;
        }
        
        if (username == compositorUser) {
            continue;
        }
        if (!isUserInCouchPlayGroup(username)) {
            Q_EMIT errorOccurred(QStringLiteral("User '%1' is not a CouchPlay managed user. Please create the user via CouchPlay or add them to the 'couchplay' group.").arg(username));
            setStatus(QStringLiteral("Error"));
            return false;
        }
    }

    inhibitScreenSaver();

    QRect screenGeometry = getScreenGeometry();
    m_layouts = calculateLayout(profile.layout, instanceCount, screenGeometry, profile.gridSubLayout);

    if (!setupDeviceOwnership()) {
        qWarning() << "Failed to set up device ownership - continuing anyway";
    }

    if (!setupSharedDirectories()) {
        qWarning() << "Failed to set up shared directories - continuing anyway";
    }

    buildBindPaths();

    if (!setupLauncherAccess()) {
        qWarning() << "Failed to set up launcher access - continuing anyway";
    }

    // Sequential launch: fixes race condition with window positioning
    m_pendingInstanceConfigs.clear();
    for (int i = 0; i < instanceCount; ++i) {
        const InstanceConfig &instConfig = profile.instances[i];

        QVariantMap config;
        config[QStringLiteral("username")] = instConfig.username;
        config[QStringLiteral("monitor")] = instConfig.monitor;
        
        config[QStringLiteral("internalWidth")] = m_layouts[i].width();
        config[QStringLiteral("internalHeight")] = m_layouts[i].height();
        config[QStringLiteral("outputWidth")] = m_layouts[i].width();
        config[QStringLiteral("outputHeight")] = m_layouts[i].height();
        config[QStringLiteral("positionX")] = m_layouts[i].x();
        config[QStringLiteral("positionY")] = m_layouts[i].y();
        config[QStringLiteral("refreshRate")] = instConfig.refreshRate;
        config[QStringLiteral("scalingMode")] = instConfig.scalingMode;
        config[QStringLiteral("filterMode")] = instConfig.filterMode;
        config[QStringLiteral("gameCommand")] = instConfig.gameCommand;
        config[QStringLiteral("steamAppId")] = instConfig.steamAppId;
        config[QStringLiteral("borderless")] = m_settingsManager ? m_settingsManager->borderlessWindows() : false;

        if (m_presetManager) {
            QString presetId = instConfig.presetId;
            if (presetId.isEmpty()) {
                presetId = QStringLiteral("steam");  // Default
            }
            config[QStringLiteral("presetId")] = presetId;
            config[QStringLiteral("presetCommand")] = m_presetManager->getCommand(presetId);
            config[QStringLiteral("presetWorkingDirectory")] = m_presetManager->getWorkingDirectory(presetId);
            config[QStringLiteral("steamIntegration")] = m_presetManager->getSteamIntegration(presetId);
        } else {
            config[QStringLiteral("presetId")] = QStringLiteral("steam");
            config[QStringLiteral("presetCommand")] = QStringLiteral("steam -tenfoot -steamdeck");
            config[QStringLiteral("steamIntegration")] = true;
        }

        if (m_deviceManager) {
            QStringList devicePaths = m_deviceManager->getDevicePathsForInstance(i);
            QVariantList pathList;
            for (const QString &path : devicePaths) {
                pathList.append(path);
            }
            config[QStringLiteral("devicePaths")] = pathList;
        }

        if (m_instanceBindPaths.contains(i)) {
            config[QStringLiteral("bindPaths")] = m_instanceBindPaths.value(i);
        }

        m_pendingInstanceConfigs.append(config);
    }

    m_nextInstanceToStart = 0;
    startNextInstance();

    QSet<int> knownEventNumbers;
    if (m_deviceManager) {
        for (const auto &device : m_deviceManager->devices()) {
            knownEventNumbers.insert(device.eventNumber);
        }
    }
    m_virtualDeviceWatcher->startWatching(knownEventNumbers);

    return true;
}

void SessionRunner::stop()
{
    if (!isRunning()) {
        return;
    }

    setStatus(QStringLiteral("Stopping session..."));

    uninhibitScreenSaver();

    m_virtualDeviceWatcher->stopWatching();

    QStringList overridePaths;
    if (m_sessionManager) {
        const auto &profile = m_sessionManager->currentProfile();
        for (int i = 0; i < profile.instances.size(); ++i) {
            const auto &instConfig = profile.instances[i];
            if (instConfig.overridePatterns.isEmpty() || instConfig.overrideGamePath.isEmpty()) {
                continue;
            }
            QString gameId = instConfig.steamAppId;
            if (gameId.isEmpty()) {
                gameId = QString::fromLatin1(QCryptographicHash::hash(
                    instConfig.overrideGamePath.toUtf8(), QCryptographicHash::Md5).toHex().left(16));
            }
            QString presetId = instConfig.presetId;
            if (presetId.isEmpty()) {
                presetId = QStringLiteral("steam");
            }
            overridePaths.append(getOverridesRootPath(presetId, gameId));
        }
    }

    for (auto *instance : m_instances) {
        if (instance->isRunning()) {
            instance->stop();
        }
    }

    restoreDeviceOwnership();
    teardownSharedDirectories();

    cleanupInstances();
    cleanupOverrideDirs(overridePaths);

    setStatus(QStringLiteral("Stopped"));
    Q_EMIT runningChanged();
    Q_EMIT instancesChanged();
    Q_EMIT sessionStopped();
}

void SessionRunner::stopInstance(int index)
{
    if (index >= 0 && index < m_instances.size()) {
        m_instances[index]->stop();
    }
}

bool SessionRunner::isRunning() const
{
    for (const auto *instance : m_instances) {
        if (instance->isRunning()) {
            return true;
        }
    }
    return false;
}

int SessionRunner::runningInstanceCount() const
{
    int count = 0;
    for (const auto *instance : m_instances) {
        if (instance->isRunning()) {
            ++count;
        }
    }
    return count;
}

QVariantList SessionRunner::instancesAsVariant() const
{
    QVariantList list;
    for (const auto *instance : m_instances) {
        QVariantMap map;
        map[QStringLiteral("index")] = instance->index();
        map[QStringLiteral("running")] = instance->isRunning();
        map[QStringLiteral("status")] = instance->status();
        map[QStringLiteral("pid")] = instance->pid();
        map[QStringLiteral("username")] = instance->username();
        
        QRect geom = instance->windowGeometry();
        map[QStringLiteral("x")] = geom.x();
        map[QStringLiteral("y")] = geom.y();
        map[QStringLiteral("width")] = geom.width();
        map[QStringLiteral("height")] = geom.height();
        
        list.append(map);
    }
    return list;
}

void SessionRunner::startNextInstance()
{
    if (m_nextInstanceToStart >= m_pendingInstanceConfigs.size()) {
        setStatus(QStringLiteral("Session running"));
        Q_EMIT runningChanged();
        Q_EMIT instancesChanged();
        Q_EMIT sessionStarted();
        return;
    }

    int index = m_nextInstanceToStart;
    const QVariantMap &config = m_pendingInstanceConfigs[index];

    auto *instance = new GamescopeInstance(this);
    connect(instance, &GamescopeInstance::started, this, &SessionRunner::onInstanceStarted);
    connect(instance, &GamescopeInstance::stopped, this, &SessionRunner::onInstanceStopped);
    connect(instance, &GamescopeInstance::errorOccurred, this, &SessionRunner::onInstanceError);

    m_instances.append(instance);

    if (!instance->start(config, index)) {
        qWarning() << "Failed to start instance" << index;
    }

    // If KWin is not available, start next instance immediately (no window positioning to wait for)
    if (!m_windowManager || !m_windowManager->isAvailable()) {
        ++m_nextInstanceToStart;
        startNextInstance();
    }
    // Otherwise wait for onWindowPositioned to trigger the next start
}

void SessionRunner::cleanupInstances()
{
    if (m_windowManager) {
        m_windowManager->cancelAllRequests();
    }
    
    for (auto *instance : m_instances) {
        instance->deleteLater();
    }
    m_instances.clear();
    m_positionedWindowIds.clear();
    
    m_pendingInstanceConfigs.clear();
    m_layouts.clear();
    m_nextInstanceToStart = 0;
}

void SessionRunner::cleanupOverrideDirs(const QStringList &overridePaths)
{
    for (const QString &path : overridePaths) {
        QDir dir(path);
        if (dir.exists()) {
            if (dir.removeRecursively()) {
                qCDebug(couchplayCore) << "Cleaned up override staging directory:" << path;
            } else {
                qCWarning(couchplayCore) << "Failed to clean up override staging directory:" << path;
            }
        }
    }
}

bool SessionRunner::setupDeviceOwnership()
{
    if (!m_deviceManager || !m_helperClient) {
        return true;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, skipping device ownership setup";
        return true;
    }

    m_ownedDevicePaths.clear();

    if (!m_sessionManager) {
        return true;
    }

    const auto &profile = m_sessionManager->currentProfile();
    
    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        
        if (username.isEmpty()) {
            continue;
        }
        
        struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
        if (!pw) {
            qWarning() << "SessionRunner: User" << username << "not found, skipping device ownership for instance" << i;
            continue;
        }
        int uid = static_cast<int>(pw->pw_uid);
        
        QStringList devicePaths = m_deviceManager->getDevicePathsForInstance(i);
        
        for (const QString &path : devicePaths) {
            if (m_helperClient->setDeviceOwner(path, uid)) {
                if (!m_ownedDevicePaths.contains(path)) {
                    m_ownedDevicePaths.append(path);
                }
            } else {
                qWarning() << "SessionRunner: Failed to set ownership of" << path;
                Q_EMIT errorOccurred(QStringLiteral("Failed to set device ownership for %1").arg(path));
            }
        }
        
        QStringList hidrawPaths = m_deviceManager->getHidrawPathsForInstance(i);
        for (const QString &hidrawPath : hidrawPaths) {
            if (m_helperClient->setDeviceOwner(hidrawPath, uid)) {
                if (!m_ownedDevicePaths.contains(hidrawPath)) {
                    m_ownedDevicePaths.append(hidrawPath);
                }
                qDebug() << "SessionRunner: Set hidraw ownership" << hidrawPath << "for user" << username;
            } else {
                qWarning() << "SessionRunner: Failed to set hidraw ownership" << hidrawPath;
            }
        }
    }

    return true;
}

void SessionRunner::restoreDeviceOwnership()
{
    if (!m_helperClient || m_ownedDevicePaths.isEmpty()) {
        return;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot restore device ownership";
        m_ownedDevicePaths.clear();
        return;
    }

    m_helperClient->restoreAllDevices();
    
    m_ownedDevicePaths.clear();
}

bool SessionRunner::setupSharedDirectories()
{
    if (!m_helperClient || !m_sessionManager) {
        return true;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, skipping shared directory setup";
        return true;
    }

    uint compositorUid = static_cast<uint>(getuid());

    const auto &profile = m_sessionManager->currentProfile();
    bool allSucceeded = true;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        const QStringList &sharedDirs = profile.instances[i].sharedDirectories;
        
        if (username.isEmpty()) {
            continue;
        }

        if (sharedDirs.isEmpty()) {
            qDebug() << "SessionRunner: No shared directories for instance" << i << "user" << username;
            continue;
        }

        qDebug() << "SessionRunner: Mounting" << sharedDirs.size() << "shared directories for user" << username;
        
        QStringList formattedDirs;
        for (const QString &dir : sharedDirs) {
            formattedDirs << dir + QLatin1Char('|');
        }
        
        int mountResult = m_helperClient->mountSharedDirectories(username, compositorUid, formattedDirs);
        if (mountResult < 0) {
            qWarning() << "SessionRunner: Failed to mount shared directories for user" << username;
            allSucceeded = false;
        } else {
            qDebug() << "SessionRunner: Mounted" << mountResult << "directories for user" << username;
        }
    }

    return allSucceeded;
}

void SessionRunner::teardownSharedDirectories()
{
    if (!m_helperClient) {
        return;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot unmount shared directories";
        return;
    }

    m_helperClient->unmountAllSharedDirectories();
}

bool SessionRunner::buildBindPaths()
{
    m_instanceBindPaths.clear();

    if (!m_sessionManager) {
        return true;
    }

    const auto &profile = m_sessionManager->currentProfile();

    for (int i = 0; i < profile.instances.size(); ++i) {
        const auto &instConfig = profile.instances[i];

        if (instConfig.overridePatterns.isEmpty()) {
            qCDebug(couchplaySharing) << "No override patterns for instance" << i << "- skipping bind paths";
            continue;
        }
        qCDebug(couchplaySharing) << "Building bind paths for instance" << i << "with" << instConfig.overridePatterns.size() << "patterns";

        const QString &username = instConfig.username;
        if (username.isEmpty()) {
            qCDebug(couchplaySharing) << "Instance" << i << "has no username, skipping bind paths";
            continue;
        }

        QString gamePath = instConfig.overrideGamePath;
        if (gamePath.isEmpty()) {
            qCWarning(couchplaySharing) << "Instance" << i << "has patterns but no game path, skipping";
            continue;
        }

        QString gameId = instConfig.steamAppId;
        if (gameId.isEmpty()) {
            gameId = QString::fromLatin1(QCryptographicHash::hash(
                gamePath.toUtf8(), QCryptographicHash::Md5).toHex().left(16));
        }

        QStringList matchedFiles = expandPatternsToFiles(gamePath, instConfig.overridePatterns);
        qCDebug(couchplaySharing) << "Instance" << i << "matched" << matchedFiles.size() << "files from" << instConfig.overridePatterns.size() << "patterns";

        QString presetId = instConfig.presetId;
        if (presetId.isEmpty()) {
            presetId = QStringLiteral("steam");  // Default preset
        }
        QString overridesRoot = getOverridesRootPath(presetId, gameId);

        // Bind format: "<stagingDir>/<relativeFile>:<gamePath>/<relativeFile>"
        QStringList bindPaths;
        for (const QString &relativePath : matchedFiles) {
            QString bindEntry = overridesRoot + relativePath
                                + QLatin1Char(':')
                                + gamePath + QLatin1Char('/') + relativePath;
            bindPaths.append(bindEntry);
        }

        if (!bindPaths.isEmpty()) {
            m_instanceBindPaths[i] = bindPaths;
            qCDebug(couchplaySharing) << "Instance" << i << "has" << bindPaths.size() << "bind paths";
        }

        loadOverrideFiles(overridesRoot, matchedFiles, username, gameId);
    }

    return true;
}

bool SessionRunner::setupLauncherAccess()
{
    if (!m_sessionManager || !m_presetManager || !m_helperClient) {
        return true;
    }

    const auto &profile = m_sessionManager->currentProfile();
    bool allSucceeded = true;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        const QString &presetId = profile.instances[i].presetId;

        if (username.isEmpty()) {
            qCDebug(couchplaySteam) << "Skipping instance" << i << "- no username";
            continue;
        }

        LaunchPreset preset = m_presetManager->getPreset(presetId);

        if (preset.launcherInfo.requiresAcls) {
            for (const QString &dir : preset.launcherInfo.gameDirectories) {
                if (dir.isEmpty()) {
                    continue;
                }
                qCDebug(couchplaySteam) << "Setting ACL with parents on" << dir << "for" << username;
                if (!m_helperClient->setPathAclWithParents(dir, username)) {
                    qCWarning(couchplaySteam) << "Failed to set ACL on" << dir;
                }
            }
        }

        if (preset.launcherId == QStringLiteral("heroic")) {
             if (m_heroicConfigManager && m_heroicConfigManager->isHeroicDetected()) {
                  qCDebug(couchplaySteam) << "Syncing Heroic config for user" << username;
                  if (!m_heroicConfigManager->syncConfigToUser(username)) {
                     qCWarning(couchplaySteam) << "Failed to sync Heroic config to" << username;
                     allSucceeded = false;
                 }
                 
                  if (m_heroicConfigManager->syncShortcutsEnabled()) {
                      qCDebug(couchplaySteam) << "Syncing Heroic shortcuts for user" << username;
                     if (!m_heroicConfigManager->syncShortcutsToUser(username)) {
                         qCWarning(couchplaySteam) << "Failed to sync Heroic shortcuts to" << username;
                         allSucceeded = false;
                     }
                 } else {
                     qCDebug(couchplaySteam) << "Heroic shortcut sync disabled, skipping";
                 }
             }
        }

        if (!m_steamConfigManager) {
            continue;
        }

        if (!m_steamConfigManager->syncShortcutsEnabled()) {
            qCDebug(couchplaySteam) << "Shortcut sync disabled, skipping";
            continue;
        }

        if (!m_steamConfigManager->isSteamDetected()) {
            m_steamConfigManager->detectSteamPaths();
        }

        if (!m_steamConfigManager->isSteamDetected()) {
            qCDebug(couchplaySteam) << "Steam not detected, skipping config sync";
            continue;
        }

        if (!(preset.steamIntegration || preset.launcherId == QStringLiteral("steam"))) {
            qCDebug(couchplaySteam) << "Skipping instance" << i << "- preset" << presetId
                                    << "does not use Steam integration";
            continue;
        }

        m_steamConfigManager->loadShortcuts();
        QStringList shortcutDirs = m_steamConfigManager->extractShortcutDirectories();
        qCDebug(couchplaySteam) << "Found" << shortcutDirs.size() << "directories in shortcuts";

        qCDebug(couchplaySteam) << "Setting up Steam shortcuts for user" << username;

        for (const QString &dir : shortcutDirs) {
            if (QDir(dir).exists()) {
                qCDebug(couchplaySteam) << "Setting ACL with parents on" << dir << "for" << username;
                if (!m_helperClient->setPathAclWithParents(dir, username)) {
                    qCWarning(couchplaySteam) << "Failed to set ACL on" << dir;
                }
            }
        }

        qCDebug(couchplaySteam) << "Calling syncShortcutsToUser for" << username;
        if (!m_steamConfigManager->syncShortcutsToUser(username)) {
            qCWarning(couchplaySteam) << "Failed to sync shortcuts to user" << username;
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

QRect SessionRunner::getScreenGeometry() const
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        return screen->geometry();
    }

    return QRect(0, 0, 1920, 1080);
}

QString SessionRunner::getOverridesRootPath(const QString &presetId, const QString &gameKeyHash)
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path = basePath + QStringLiteral("/overrides/") + presetId + QStringLiteral("/") + gameKeyHash + QStringLiteral("/");
    return path;
}

QString SessionRunner::getAndEnsureOverridesPath(const QString &presetId)
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString overridesPath = basePath + QStringLiteral("/overrides/") + presetId + QStringLiteral("/");
    
    QDir dir;
    if (!dir.exists(overridesPath)) {
        dir.mkpath(overridesPath);
    }
    
    return overridesPath;
}

QStringList SessionRunner::expandPatternsToFiles(const QString &gamePath, const QStringList &patterns)
{
    QStringList matchedFiles;
    
    if (gamePath.isEmpty() || patterns.isEmpty()) {
        return matchedFiles;
    }
    
    QDir baseDir(gamePath);
    if (!baseDir.exists()) {
        qCWarning(couchplaySharing) << "expandPatternsToFiles: gamePath does not exist:" << gamePath;
        return matchedFiles;
    }
    
    QDirIterator it(gamePath, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    
    while (it.hasNext()) {
        QString filePath = it.next();
        QString relativePath = baseDir.relativeFilePath(filePath);
        
        for (const QString &pattern : patterns) {
            if (QDir::match(pattern, relativePath)) {
                matchedFiles.append(relativePath);
                qCDebug(couchplaySharing) << "Pattern" << pattern << "matched file:" << relativePath;
                break;
            }
        }
    }
    
    qCDebug(couchplaySharing) << "expandPatternsToFiles:" << matchedFiles.size()
                              << "files matched from" << patterns.size() << "patterns in" << gamePath;
    
    return matchedFiles;
}

void SessionRunner::loadOverrideFiles(const QString &overridesRoot, const QStringList &matchedFiles, const QString &username, const QString &gameId)
{
    Q_UNUSED(username)
    Q_UNUSED(gameId)

    if (overridesRoot.isEmpty() || matchedFiles.isEmpty()) {
        return;
    }

    QDir dir(overridesRoot);
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            qCWarning(couchplaySharing) << "Failed to create overrides directory:" << overridesRoot;
            return;
        }
    }

    qCDebug(couchplaySharing) << "Override staging ready:" << overridesRoot
                               << "with" << matchedFiles.size() << "files for bind paths";
}

QList<QRect> SessionRunner::calculateLayout(const QString &layout,
                                             int instanceCount,
                                             const QRect &screenGeometry,
                                             const QString &gridSubLayout)
{
    QList<QRect> result;

    if (instanceCount < 1) {
        return result;
    }

    int x = screenGeometry.x();
    int y = screenGeometry.y();
    int w = screenGeometry.width();
    int h = screenGeometry.height();

    if (layout == QStringLiteral("horizontal")) {
        // Side by side (equal width)
        int instanceWidth = w / instanceCount;
        for (int i = 0; i < instanceCount; ++i) {
            result.append(QRect(x + i * instanceWidth, y, instanceWidth, h));
        }
    } else if (layout == QStringLiteral("vertical")) {
        // Stacked top to bottom (equal height)
        int instanceHeight = h / instanceCount;
        for (int i = 0; i < instanceCount; ++i) {
            result.append(QRect(x, y + i * instanceHeight, w, instanceHeight));
        }
    } else if (layout == QStringLiteral("grid")) {
        // 3-player grid sub-layouts:
        //   "horizontal" (default): 3x1
        //   "grid-2x2": 2x2 with empty cell
        //   "left-right": player 1 left 40%, players 2+3 stacked right 60%
        if (instanceCount == 3 && gridSubLayout == QStringLiteral("left-right")) {
            int leftWidth = w * 2 / 5;
            int rightWidth = w - leftWidth;
            int halfHeight = h / 2;
            result.append(QRect(x, y, leftWidth, h));
            result.append(QRect(x + leftWidth, y, rightWidth, halfHeight));
            result.append(QRect(x + leftWidth, y + halfHeight, rightWidth, h - halfHeight));
        } else {
            int cols, rows;
            if (instanceCount == 3) {
                if (gridSubLayout == QStringLiteral("grid-2x2")) {
                    cols = 2;
                    rows = 2;
                } else {
                    // Default for 3 players: horizontal (3×1)
                    cols = 3;
                    rows = 1;
                }
            } else if (instanceCount <= 2) {
                cols = 2;
                rows = 1;
            } else {
                cols = 2;
                rows = 2;
            }

            int cellWidth = w / cols;
            int cellHeight = h / rows;

            for (int i = 0; i < instanceCount; ++i) {
                int col = i % cols;
                int row = i / cols;
                result.append(QRect(x + col * cellWidth, y + row * cellHeight, cellWidth, cellHeight));
            }
        }
    } else if (layout == QStringLiteral("multi-monitor")) {
        for (int i = 0; i < instanceCount; ++i) {
            result.append(screenGeometry);
        }
    } else {
        int instanceWidth = w / instanceCount;
        for (int i = 0; i < instanceCount; ++i) {
            result.append(QRect(x + i * instanceWidth, y, instanceWidth, h));
        }
    }

    return result;
}

void SessionRunner::onInstanceStarted()
{
    auto *instance = qobject_cast<GamescopeInstance*>(sender());
    if (instance) {
        positionInstanceWindow(instance);
        
        Q_EMIT instanceStarted(instance->index());
        Q_EMIT instancesChanged();
        Q_EMIT runningInstanceCountChanged();
    }
}

void SessionRunner::onInstanceStopped()
{
    auto *instance = qobject_cast<GamescopeInstance*>(sender());
    if (instance) {
        Q_EMIT instanceStopped(instance->index());
        Q_EMIT instancesChanged();
        Q_EMIT runningInstanceCountChanged();

        if (!isRunning()) {
            uninhibitScreenSaver();
            setStatus(QStringLiteral("Session ended"));
            restoreDeviceOwnership();
            Q_EMIT runningChanged();
            Q_EMIT sessionStopped();
        }
    }
}

void SessionRunner::onInstanceError(const QString &message)
{
    auto *instance = qobject_cast<GamescopeInstance*>(sender());
    QString fullMessage;
    if (instance) {
        fullMessage = QStringLiteral("Instance %1: %2").arg(instance->index()).arg(message);
    } else {
        fullMessage = message;
    }
    Q_EMIT errorOccurred(fullMessage);
}

void SessionRunner::positionInstanceWindow(GamescopeInstance *instance)
{
    if (!instance || !m_windowManager || !m_windowManager->isAvailable()) {
        return;
    }

    QRect targetGeometry = instance->windowGeometry();
    int instanceIndex = instance->index();
    bool borderless = m_settingsManager ? m_settingsManager->borderlessWindows() : false;

    m_windowManager->queuePositionRequest(
        instanceIndex,
        targetGeometry,
        m_positionedWindowIds,
        borderless,
        60000
    );
}

void SessionRunner::onWindowPositioned(int requestId, const QString &windowId)
{
    Q_UNUSED(requestId)
    if (!m_positionedWindowIds.contains(windowId)) {
        m_positionedWindowIds.append(windowId);
    }

    // Trigger next sequential instance launch
    if (!m_pendingInstanceConfigs.isEmpty()) {
        ++m_nextInstanceToStart;
        startNextInstance();
    }
}

void SessionRunner::onWindowPositioningTimeout(int requestId)
{
    qWarning() << "SessionRunner: Failed to position window for instance" << requestId 
               << "after timeout - stopping session";
    Q_EMIT errorOccurred(QStringLiteral("Failed to position window for instance %1. Session stopped.").arg(requestId));
    stop();
}

void SessionRunner::setupGlobalShortcut()
{
    m_stopAction = new QAction(this);
    m_stopAction->setObjectName(QStringLiteral("stop-couchplay-session"));
    m_stopAction->setText(i18nc("@action", "Stop CouchPlay Session"));
    m_stopAction->setProperty("componentName", QStringLiteral("couchplay"));
    
    connect(m_stopAction, &QAction::triggered, this, [this]() {
        if (isRunning()) {
            stop();
        }
    });
    
    // Default shortcut: Meta+Shift+Escape
    KGlobalAccel::setGlobalShortcut(m_stopAction, 
        QList<QKeySequence>() << QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Escape));
}

void SessionRunner::onDeviceReconnected(const QString &stableId, int eventNumber, int instanceIndex)
{
    if (!isRunning()) {
        return;
    }
    
    if (!m_helperClient || !m_sessionManager) {
        return;
    }
    
    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot restore device ownership";
        return;
    }
    
    const auto &profile = m_sessionManager->currentProfile();
    if (instanceIndex < 0 || instanceIndex >= profile.instances.size()) {
        qWarning() << "SessionRunner: Invalid instance index" << instanceIndex << "for reconnected device";
        return;
    }
    
    const QString &username = profile.instances[instanceIndex].username;
    if (username.isEmpty()) {
        qWarning() << "SessionRunner: No username for instance" << instanceIndex;
        return;
    }
    
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    if (!pw) {
        qWarning() << "SessionRunner: User" << username << "not found";
        return;
    }
    int uid = static_cast<int>(pw->pw_uid);
    
    QString devicePath = QStringLiteral("/dev/input/event%1").arg(eventNumber);
    
    qDebug() << "SessionRunner: Device reconnected, restoring ownership:"
             << devicePath << "(stableId:" << stableId << ") to user" << username;
    
    if (m_helperClient->setDeviceOwner(devicePath, uid)) {
        if (!m_ownedDevicePaths.contains(devicePath)) {
            m_ownedDevicePaths.append(devicePath);
        }
        qDebug() << "SessionRunner: Successfully restored ownership of" << devicePath;
    } else {
        qWarning() << "SessionRunner: Failed to restore ownership of" << devicePath;
        Q_EMIT errorOccurred(QStringLiteral("Failed to restore device ownership for %1").arg(devicePath));
    }
    
    QString hidrawPath = m_deviceManager->findHidrawForEvent(eventNumber);
    if (!hidrawPath.isEmpty() && m_helperClient->setDeviceOwner(hidrawPath, uid)) {
        if (!m_ownedDevicePaths.contains(hidrawPath)) {
            m_ownedDevicePaths.append(hidrawPath);
        }
        qDebug() << "SessionRunner: Restored hidraw ownership on reconnection:" << hidrawPath;
    }
}

QList<qint64> SessionRunner::getGamescopePids() const
{
    QList<qint64> pids;
    for (const auto *instance : m_instances) {
        if (instance && instance->gamescopePid() > 0) {
            pids.append(instance->gamescopePid());
        }
    }
    return pids;
}

qint64 SessionRunner::findSteamProcess(qint64 gamescopePid, int maxDepth) const
{
    if (maxDepth <= 0 || gamescopePid <= 0) {
        return 0;
    }

    QString childrenPath = QStringLiteral("/proc/%1/task/%1/children").arg(gamescopePid);
    QFile childrenFile(childrenPath);
    if (!childrenFile.open(QIODevice::ReadOnly)) {
        return 0;
    }

    QString childrenData = QString::fromLocal8Bit(childrenFile.readAll());
    QStringList childPids = childrenData.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    for (const QString &childPidStr : childPids) {
        qint64 childPid = childPidStr.toLongLong();
        if (childPid <= 0) {
            continue;
        }

        QString commPath = QStringLiteral("/proc/%1/comm").arg(childPid);
        QFile commFile(commPath);
        if (commFile.open(QIODevice::ReadOnly)) {
            QString comm = QString::fromLocal8Bit(commFile.readAll()).trimmed().toLower();
            if (comm.contains(QStringLiteral("steam"))) {
                return childPid;
            }
        }

        qint64 grandchildSteam = findSteamProcess(childPid, maxDepth - 1);
        if (grandchildSteam > 0) {
            return grandchildSteam;
        }
    }

    return 0;
}

// Synchronous /proc/<pid>/fd/ traversal (see AGENTS.md anti-patterns).
// Steam has bounded FDs and VirtualDeviceWatcher debounces, so impact is minimal.
bool SessionRunner::hasUinputOpen(qint64 pid) const
{
    QString fdDir = QStringLiteral("/proc/%1/fd").arg(pid);
    QDir dir(fdDir);

    for (const QString &fdLink : dir.entryList(QDir::Files)) {
        QString linkTarget = QFile::symLinkTarget(fdDir + QStringLiteral("/") + fdLink);
        if (linkTarget == QStringLiteral("/dev/uinput")) {
            return true;
        }
    }

    return false;
}

bool SessionRunner::hasFdOpen(qint64 pid, const QString &targetPath) const
{
    QString fdDir = QStringLiteral("/proc/%1/fd").arg(pid);
    QDir dir(fdDir);

    for (const QString &fdLink : dir.entryList(QDir::Files)) {
        if (QFile::symLinkTarget(fdDir + QStringLiteral("/") + fdLink) == targetPath) {
            return true;
        }
    }

    return false;
}

QString SessionRunner::attributeVirtualDevice(int, const QString &devicePath) const
{
    QList<qint64> gamescopePids = getGamescopePids();

    for (qint64 gamescopePid : gamescopePids) {
        qint64 steamPid = findSteamProcess(gamescopePid);
        if (steamPid == 0) {
            continue;
        }

        if (hasUinputOpen(steamPid) && hasFdOpen(steamPid, devicePath)) {
            for (int i = 0; i < m_instances.size(); ++i) {
                if (m_instances[i]->gamescopePid() == gamescopePid) {
                    if (m_sessionManager) {
                        const auto &profile = m_sessionManager->currentProfile();
                        if (i < profile.instances.size()) {
                            return profile.instances[i].username;
                        }
                    }
                }
            }
        }
    }

    return QString();
}

void SessionRunner::onVirtualDeviceAppeared(int eventNumber, const QString &devicePath, const QString &deviceName)
{
    qDebug() << "SessionRunner: Virtual device appeared:" << devicePath << deviceName;

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot set virtual device ownership";
        return;
    }

    QString username = attributeVirtualDevice(eventNumber, devicePath);
    if (username.isEmpty()) {
        qWarning() << "SessionRunner: Could not attribute virtual device" << deviceName << devicePath << "- no Steam process with matching FD found";
        return;
    }

    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    if (!pw) {
        qWarning() << "SessionRunner: User" << username << "not found";
        return;
    }

    if (m_helperClient->setDeviceOwner(devicePath, pw->pw_uid)) {
        if (!m_ownedDevicePaths.contains(devicePath)) {
            m_ownedDevicePaths.append(devicePath);
        }
        qDebug() << "SessionRunner: Set virtual device ownership" << devicePath << "for user" << username;
    } else {
        qWarning() << "SessionRunner: Failed to set virtual device ownership" << devicePath;
    }
}

void SessionRunner::inhibitScreenSaver()
{
    if (m_screenSaverCookie != 0) {
        return;
    }

    QDBusInterface screenSaver(
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("/org/freedesktop/ScreenSaver"),
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QDBusConnection::sessionBus()
    );

    if (!screenSaver.isValid()) {
        qCDebug(couchplayCore) << "ScreenSaver D-Bus interface not available, skipping inhibition";
        return;
    }

    QDBusReply<uint> reply = screenSaver.call(
        QStringLiteral("Inhibit"),
        QStringLiteral("io.github.hikaps.couchplay"),
        QStringLiteral("Split-screen gaming session active")
    );

    if (reply.isValid()) {
        m_screenSaverCookie = reply.value();
        qCDebug(couchplayCore) << "ScreenSaver inhibited, cookie:" << m_screenSaverCookie;
    } else {
        qCWarning(couchplayCore) << "Failed to inhibit ScreenSaver:" << reply.error().message();
    }
}

void SessionRunner::uninhibitScreenSaver()
{
    if (m_screenSaverCookie == 0) {
        return;
    }

    QDBusInterface screenSaver(
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("/org/freedesktop/ScreenSaver"),
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QDBusConnection::sessionBus()
    );

    if (screenSaver.isValid()) {
        screenSaver.call(QStringLiteral("UnInhibit"), m_screenSaverCookie);
        qCDebug(couchplayCore) << "ScreenSaver uninhibited, cookie:" << m_screenSaverCookie;
    }

    m_screenSaverCookie = 0;
}
