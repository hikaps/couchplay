// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SessionRunner.h"
#include "GamescopeInstance.h"
#include "Logging.h"
#include "SessionManager.h"
#include "DeviceManager.h"
#include "PresetManager.h"
#include "SharingManager.h"
#include "SteamConfigManager.h"
#include "WindowManager.h"
#include "../dbus/CouchPlayHelperClient.h"

#include <QAction>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>

#include <KGlobalAccel>
#include <KLocalizedString>

#include <pwd.h>

SessionRunner::SessionRunner(QObject *parent)
    : QObject(parent)
    , m_windowManager(new WindowManager(this))
{
    setStatus(QStringLiteral("Ready"));
    
    // Set up global shortcut for stopping session
    setupGlobalShortcut();
    
    // Connect to window positioning signals
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
        m_deviceManager = manager;
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

void SessionRunner::setSharingManager(SharingManager *manager)
{
    if (m_sharingManager != manager) {
        m_sharingManager = manager;
        Q_EMIT sharingManagerChanged();
    }
}

void SessionRunner::setSteamConfigManager(SteamConfigManager *manager)
{
    if (m_steamConfigManager != manager) {
        m_steamConfigManager = manager;
        Q_EMIT steamConfigManagerChanged();
    }
}

void SessionRunner::setBorderlessWindows(bool borderless)
{
    if (m_borderlessWindows != borderless) {
        m_borderlessWindows = borderless;
        Q_EMIT borderlessWindowsChanged();
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

    // Clean up any previous instances
    cleanupInstances();

    // Get session configuration
    const SessionProfile &profile = m_sessionManager->currentProfile();
    int instanceCount = profile.instances.size();

    if (instanceCount < 1) {
        Q_EMIT errorOccurred(QStringLiteral("No instances configured"));
        setStatus(QStringLiteral("Error"));
        return false;
    }

    // Check for duplicate users - Steam can't run multiple instances under same user
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

    // Calculate window layouts
    QRect screenGeometry = getScreenGeometry();
    QList<QRect> layouts = calculateLayout(profile.layout, instanceCount, screenGeometry);

    // Set up device ownership (requires polkit helper)
    if (!setupDeviceOwnership()) {
        qWarning() << "Failed to set up device ownership - continuing anyway";
    }

    // Set up shared directory mounts (requires polkit helper)
    if (!setupSharedDirectories()) {
        qWarning() << "Failed to set up shared directories - continuing anyway";
    }

    // Sync Steam config to gaming users
    if (!setupSteamConfig()) {
        qWarning() << "Failed to sync Steam config - continuing anyway";
    }

    // Create and start instances
    for (int i = 0; i < instanceCount; ++i) {
        const InstanceConfig &instConfig = profile.instances[i];
        
        // Create instance
        auto *instance = new GamescopeInstance(this);
        connect(instance, &GamescopeInstance::started, this, &SessionRunner::onInstanceStarted);
        connect(instance, &GamescopeInstance::stopped, this, &SessionRunner::onInstanceStopped);
        connect(instance, &GamescopeInstance::errorOccurred, this, &SessionRunner::onInstanceError);

        // Build config map for the instance
        QVariantMap config;
        config[QStringLiteral("username")] = instConfig.username;
        config[QStringLiteral("monitor")] = instConfig.monitor;
        
        // Derive resolution from layout - internal resolution matches output resolution
        // This ensures games render at the correct size for their window
        config[QStringLiteral("internalWidth")] = layouts[i].width();
        config[QStringLiteral("internalHeight")] = layouts[i].height();
        config[QStringLiteral("outputWidth")] = layouts[i].width();
        config[QStringLiteral("outputHeight")] = layouts[i].height();
        config[QStringLiteral("positionX")] = layouts[i].x();
        config[QStringLiteral("positionY")] = layouts[i].y();
        config[QStringLiteral("refreshRate")] = instConfig.refreshRate;
        config[QStringLiteral("scalingMode")] = instConfig.scalingMode;
        config[QStringLiteral("filterMode")] = instConfig.filterMode;
        config[QStringLiteral("gameCommand")] = instConfig.gameCommand;
        config[QStringLiteral("steamAppId")] = instConfig.steamAppId;
        config[QStringLiteral("borderless")] = m_borderlessWindows;

        // Look up preset and add resolved command/settings
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
            // Fallback if no PresetManager
            config[QStringLiteral("presetId")] = QStringLiteral("steam");
            config[QStringLiteral("presetCommand")] = QStringLiteral("steam -tenfoot -steamdeck");
            config[QStringLiteral("steamIntegration")] = true;
        }

        // Get device paths for this instance
        if (m_deviceManager) {
            QStringList devicePaths = m_deviceManager->getDevicePathsForInstance(i);
            QVariantList pathList;
            for (const QString &path : devicePaths) {
                pathList.append(path);
            }
            config[QStringLiteral("devicePaths")] = pathList;
        }

        m_instances.append(instance);

        // Start with slight delay between instances to avoid resource contention
        if (!instance->start(config, i)) {
            qWarning() << "Failed to start instance" << i;
        }
    }

    setStatus(QStringLiteral("Session running"));
    Q_EMIT runningChanged();
    Q_EMIT instancesChanged();
    Q_EMIT sessionStarted();

    return true;
}

void SessionRunner::stop()
{
    if (!isRunning()) {
        return;
    }

    setStatus(QStringLiteral("Stopping session..."));

    // Stop all instances
    for (auto *instance : m_instances) {
        if (instance->isRunning()) {
            instance->stop();
        }
    }

    // Restore device ownership
    restoreDeviceOwnership();

    // Teardown shared directory mounts
    teardownSharedDirectories();

    cleanupInstances();

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

void SessionRunner::cleanupInstances()
{
    // Cancel any pending window positioning requests
    if (m_windowManager) {
        m_windowManager->cancelAllRequests();
    }
    
    for (auto *instance : m_instances) {
        instance->deleteLater();
    }
    m_instances.clear();
    m_positionedWindowIds.clear(); // Clear tracked window IDs for next session
}

bool SessionRunner::setupDeviceOwnership()
{
    if (!m_deviceManager || !m_helperClient) {
        return true; // No helper, skip ownership setup
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, skipping device ownership setup";
        return true;
    }

    // Clear previous ownership tracking
    m_ownedDevicePaths.clear();

    // For each instance, get its assigned devices and set ownership
    if (!m_sessionManager) {
        return true;
    }

    const auto &profile = m_sessionManager->currentProfile();
    
    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        
        if (username.isEmpty()) {
            continue;
        }
        
        // Get UID for this user
        struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
        if (!pw) {
            qWarning() << "SessionRunner: User" << username << "not found, skipping device ownership for instance" << i;
            continue;
        }
        int uid = static_cast<int>(pw->pw_uid);
        
        // Get device paths assigned to this instance
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

    // Use restoreAllDevices() which resets all modified devices tracked by the helper
    m_helperClient->restoreAllDevices();
    
    m_ownedDevicePaths.clear();
}

bool SessionRunner::setupSharedDirectories()
{
    if (!m_sharingManager || !m_helperClient || !m_sessionManager) {
        return true; // No sharing configured, skip
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, skipping shared directory setup";
        return true;
    }

    QStringList directories = m_sharingManager->directoryList();
    if (directories.isEmpty()) {
        return true; // Nothing to share
    }

    // Get compositor UID (current user running CouchPlay)
    uint compositorUid = static_cast<uint>(getuid());

    const auto &profile = m_sessionManager->currentProfile();
    bool allSucceeded = true;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        
        if (username.isEmpty()) {
            continue;
        }

        qDebug() << "SessionRunner: Mounting shared directories for user" << username;
        
        int mountResult = m_helperClient->mountSharedDirectories(username, compositorUid, directories);
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

    // Unmount all shared directories for all users
    m_helperClient->unmountAllSharedDirectories();
}

bool SessionRunner::setupSteamConfig()
{
    if (!m_steamConfigManager || !m_sessionManager) {
        return true; // No Steam config manager, skip
    }

    // Check if shortcut sync is enabled
    if (!m_steamConfigManager->syncShortcutsEnabled()) {
        qCDebug(couchplaySteam) << "Shortcut sync disabled, skipping";
        return true;
    }

    // Ensure Steam paths are detected
    if (!m_steamConfigManager->isSteamDetected()) {
        m_steamConfigManager->detectSteamPaths();
    }

    if (!m_steamConfigManager->isSteamDetected()) {
        qCDebug(couchplaySteam) << "Steam not detected, skipping config sync";
        return true;
    }

    // Load shortcuts from compositor's Steam
    m_steamConfigManager->loadShortcuts();
    
    // Extract directories from shortcuts for ACL setup
    QStringList shortcutDirs = m_steamConfigManager->extractShortcutDirectories();
    qCDebug(couchplaySteam) << "Found" << shortcutDirs.size() << "directories in shortcuts";

    const auto &profile = m_sessionManager->currentProfile();
    bool allSucceeded = true;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        
        if (username.isEmpty()) {
            qCDebug(couchplaySteam) << "Skipping instance" << i << "- no username";
            continue;
        }

        qCDebug(couchplaySteam) << "Setting up Steam shortcuts for user" << username;

        // Set ACLs on directories referenced in shortcuts (including parent paths for traversal)
        for (const QString &dir : shortcutDirs) {
            if (QDir(dir).exists()) {
                qCDebug(couchplaySteam) << "Setting ACL with parents on" << dir << "for" << username;
                if (!m_helperClient->setPathAclWithParents(dir, username)) {
                    qCWarning(couchplaySteam) << "Failed to set ACL on" << dir;
                    // Continue anyway - some directories might not need ACLs
                }
            }
        }

        // Sync shortcuts to this user
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
    // Get the primary screen geometry
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        return screen->geometry();
    }
    
    // Fallback to a reasonable default
    return QRect(0, 0, 1920, 1080);
}

QList<QRect> SessionRunner::calculateLayout(const QString &layout,
                                             int instanceCount,
                                             const QRect &screenGeometry)
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
        // 2x2 grid for 4 players, 2x1 for 2, etc.
        int cols, rows;
        if (instanceCount <= 2) {
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
    } else if (layout == QStringLiteral("multi-monitor")) {
        // Each instance on a different monitor - just use full screen for now
        // In a real implementation, we'd get each monitor's geometry
        for (int i = 0; i < instanceCount; ++i) {
            result.append(screenGeometry);
        }
    } else {
        // Default to horizontal
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
        // Position the window after a short delay to allow it to appear
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

        // Check if all instances have stopped
        if (!isRunning()) {
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

    // Queue a position request - the WindowManager will find and position
    // the next gamescope window that appears (excluding already-positioned ones)
    m_windowManager->queuePositionRequest(
        instanceIndex,
        targetGeometry,
        m_positionedWindowIds,
        60000  // 60 second timeout for Steam login on secondary instances
    );
}

void SessionRunner::onWindowPositioned(int requestId, const QString &windowId)
{
    Q_UNUSED(requestId)
    // Track this window so it's excluded from future positioning
    if (!m_positionedWindowIds.contains(windowId)) {
        m_positionedWindowIds.append(windowId);
    }
}

void SessionRunner::onWindowPositioningTimeout(int requestId)
{
    qWarning() << "SessionRunner: Failed to position window for instance" << requestId 
               << "after timeout";
    Q_EMIT errorOccurred(QStringLiteral("Failed to position window for instance %1").arg(requestId));
}

void SessionRunner::setupGlobalShortcut()
{
    m_stopAction = new QAction(this);
    m_stopAction->setObjectName(QStringLiteral("stop-couchplay-session"));
    m_stopAction->setText(i18nc("@action", "Stop CouchPlay Session"));
    m_stopAction->setProperty("componentName", QStringLiteral("couchplay"));
    
    // Only stop if session is actually running
    connect(m_stopAction, &QAction::triggered, this, [this]() {
        if (isRunning()) {
            stop();
        }
    });
    
    // Set default shortcut: Meta+Shift+Escape
    KGlobalAccel::setGlobalShortcut(m_stopAction, 
        QList<QKeySequence>() << QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Escape));
}
