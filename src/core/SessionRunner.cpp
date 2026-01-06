// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SessionRunner.h"
#include "GamescopeInstance.h"
#include "SessionManager.h"
#include "DeviceManager.h"
#include "../dbus/CouchPlayHelperClient.h"

#include <QDebug>
#include <QScreen>
#include <QGuiApplication>
#include <QSet>

SessionRunner::SessionRunner(QObject *parent)
    : QObject(parent)
{
    setStatus(QStringLiteral("Ready"));
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
        config[QStringLiteral("internalWidth")] = instConfig.internalWidth;
        config[QStringLiteral("internalHeight")] = instConfig.internalHeight;
        config[QStringLiteral("outputWidth")] = layouts[i].width();
        config[QStringLiteral("outputHeight")] = layouts[i].height();
        config[QStringLiteral("positionX")] = layouts[i].x();
        config[QStringLiteral("positionY")] = layouts[i].y();
        config[QStringLiteral("refreshRate")] = instConfig.refreshRate;
        config[QStringLiteral("scalingMode")] = instConfig.scalingMode;
        config[QStringLiteral("filterMode")] = instConfig.filterMode;
        config[QStringLiteral("gameCommand")] = instConfig.gameCommand;
        config[QStringLiteral("steamAppId")] = instConfig.steamAppId;
        config[QStringLiteral("launchMode")] = instConfig.launchMode;
        config[QStringLiteral("borderless")] = m_borderlessWindows;

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
    for (auto *instance : m_instances) {
        instance->deleteLater();
    }
    m_instances.clear();
}

bool SessionRunner::setupDeviceOwnership()
{
    if (!m_deviceManager || !m_helperClient) {
        return true; // No helper, skip ownership setup
    }

    // Get all device paths that need ownership changes
    m_ownedDevicePaths.clear();

    // For each instance, get its assigned devices
    if (m_sessionManager) {
        const auto &profile = m_sessionManager->currentProfile();
        for (int i = 0; i < profile.instances.size(); ++i) {
            QStringList paths = m_deviceManager->getDevicePathsForInstance(i);
            for (const QString &path : paths) {
                if (!m_ownedDevicePaths.contains(path)) {
                    m_ownedDevicePaths.append(path);
                }
            }
        }
    }

    // TODO: Call helper to change device ownership
    // This requires the polkit helper to be running
    // for (const QString &path : m_ownedDevicePaths) {
    //     m_helperClient->setDeviceOwner(path, targetUid, targetGid);
    // }

    return true;
}

void SessionRunner::restoreDeviceOwnership()
{
    if (!m_helperClient || m_ownedDevicePaths.isEmpty()) {
        return;
    }

    // TODO: Call helper to restore device ownership
    // m_helperClient->restoreAllDevices();

    m_ownedDevicePaths.clear();
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
