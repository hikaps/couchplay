// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>
#include <QRect>
#include <qqmlintegration.h>

#include "../dbus/CouchPlayHelperClient.h"

class QAction;

class GamescopeInstance;
class DeviceManager;
class SessionManager;
class WindowManager;
class PresetManager;

/**
 * @brief Orchestrates running a complete split-screen gaming session
 * 
 * SessionRunner manages the lifecycle of multiple GamescopeInstance objects,
 * handles window layout calculations, device ownership transfers, and
 * coordinates with the SessionManager for configuration.
 * 
 * Typical usage:
 * 1. Set sessionManager, deviceManager, helperClient
 * 2. Call start() to begin the session
 * 3. Monitor via runningInstances property
 * 4. Call stop() to end the session
 */
class SessionRunner : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(int runningInstanceCount READ runningInstanceCount NOTIFY runningInstanceCountChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList instances READ instancesAsVariant NOTIFY instancesChanged)
    Q_PROPERTY(bool borderlessWindows READ borderlessWindows WRITE setBorderlessWindows NOTIFY borderlessWindowsChanged)
    
    // Dependencies
    Q_PROPERTY(SessionManager* sessionManager READ sessionManager WRITE setSessionManager NOTIFY sessionManagerChanged)
    Q_PROPERTY(DeviceManager* deviceManager READ deviceManager WRITE setDeviceManager NOTIFY deviceManagerChanged)
    Q_PROPERTY(CouchPlayHelperClient* helperClient READ helperClient WRITE setHelperClient NOTIFY helperClientChanged)
    Q_PROPERTY(PresetManager* presetManager READ presetManager WRITE setPresetManager NOTIFY presetManagerChanged)

public:
    explicit SessionRunner(QObject *parent = nullptr);
    ~SessionRunner() override;

    /**
     * @brief Start all instances in the current session
     * @return true if all instances started successfully
     */
    Q_INVOKABLE bool start();

    /**
     * @brief Stop all running instances
     */
    Q_INVOKABLE void stop();

    /**
     * @brief Stop a specific instance by index
     * @param index Instance index
     */
    Q_INVOKABLE void stopInstance(int index);

    /**
     * @brief Check if any instance is running
     */
    bool isRunning() const;

    /**
     * @brief Get the number of currently running instances
     */
    int runningInstanceCount() const;

    /**
     * @brief Get current status message
     */
    QString status() const { return m_status; }

    /**
     * @brief Get list of instances with their status
     */
    QVariantList instancesAsVariant() const;

    // Dependency getters/setters
    SessionManager* sessionManager() const { return m_sessionManager; }
    void setSessionManager(SessionManager *manager);

    DeviceManager* deviceManager() const { return m_deviceManager; }
    void setDeviceManager(DeviceManager *manager);

    CouchPlayHelperClient* helperClient() const { return m_helperClient; }
    void setHelperClient(CouchPlayHelperClient *client);

    PresetManager* presetManager() const { return m_presetManager; }
    void setPresetManager(PresetManager *manager);

    bool borderlessWindows() const { return m_borderlessWindows; }
    void setBorderlessWindows(bool borderless);

    /**
     * @brief Calculate window geometries for a given layout
     * @param layout Layout type: "horizontal", "vertical", "multi-monitor", "grid"
     * @param instanceCount Number of instances
     * @param screenGeometry Available screen geometry
     * @return List of QRect geometries for each instance
     */
    static QList<QRect> calculateLayout(const QString &layout, 
                                         int instanceCount,
                                         const QRect &screenGeometry);

Q_SIGNALS:
    void runningChanged();
    void runningInstanceCountChanged();
    void statusChanged();
    void instancesChanged();
    void sessionManagerChanged();
    void deviceManagerChanged();
    void helperClientChanged();
    void presetManagerChanged();
    void borderlessWindowsChanged();
    void errorOccurred(const QString &message);
    void sessionStarted();
    void sessionStopped();
    void instanceStarted(int index);
    void instanceStopped(int index);

private Q_SLOTS:
    void onInstanceStarted();
    void onInstanceStopped();
    void onInstanceError(const QString &message);
    void onWindowPositioned(int requestId, const QString &windowId);
    void onWindowPositioningTimeout(int requestId);

private:
    void setStatus(const QString &status);
    void cleanupInstances();
    bool setupDeviceOwnership();
    void restoreDeviceOwnership();
    QRect getScreenGeometry() const;
    void positionInstanceWindow(GamescopeInstance *instance);
    void setupGlobalShortcut();

    QList<GamescopeInstance*> m_instances;
    SessionManager *m_sessionManager = nullptr;
    DeviceManager *m_deviceManager = nullptr;
    CouchPlayHelperClient *m_helperClient = nullptr;
    PresetManager *m_presetManager = nullptr;
    WindowManager *m_windowManager = nullptr;
    QAction *m_stopAction = nullptr;
    QString m_status;
    QStringList m_ownedDevicePaths; // Devices we've taken ownership of
    QStringList m_positionedWindowIds; // Window IDs we've positioned (for excluding)
    bool m_borderlessWindows = false; // Default to decorated windows
};
