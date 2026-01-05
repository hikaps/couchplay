// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QList>
#include <QRect>
#include <QVariantMap>
#include <qqmlintegration.h>

struct InstanceConfig;

/**
 * @brief Manages a single gamescope instance and its child Steam process
 * 
 * Handles starting gamescope with appropriate arguments for:
 * - Resolution (internal and output)
 * - Window positioning for split-screen layouts
 * - Input device isolation
 * - Secondary user execution via sudo
 * - PipeWire audio forwarding
 */
class GamescopeInstance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int index READ index CONSTANT)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(qint64 pid READ pid NOTIFY runningChanged)
    Q_PROPERTY(QString username READ username NOTIFY configChanged)
    Q_PROPERTY(QRect windowGeometry READ windowGeometry NOTIFY configChanged)

public:
    explicit GamescopeInstance(QObject *parent = nullptr);
    ~GamescopeInstance() override;

    /**
     * @brief Start the gamescope instance
     * @param config Instance configuration containing:
     *   - username: Linux user to run as (empty = current user)
     *   - internalWidth/Height: Game render resolution
     *   - outputWidth/Height: Window size
     *   - refreshRate: Target refresh rate
     *   - scalingMode: fit, stretch, integer, etc.
     *   - filterMode: linear, nearest, FSR, NIS
     *   - monitor: Monitor index for multi-monitor
     *   - positionX/Y: Window position for split-screen
     *   - devicePaths: List of /dev/input/eventN paths for input isolation
     *   - gameCommand: Steam app ID or command to launch
     * @param index Instance index (0 = primary, 1+ = secondary)
     * @return true if started successfully
     */
    Q_INVOKABLE bool start(const QVariantMap &config, int index);

    /**
     * @brief Stop the gamescope instance gracefully
     * @param timeoutMs Timeout before force kill (default 5000ms)
     */
    Q_INVOKABLE void stop(int timeoutMs = 5000);

    /**
     * @brief Force kill the gamescope instance
     */
    Q_INVOKABLE void kill();

    /**
     * @brief Check if instance is running
     */
    bool isRunning() const;

    // Property getters
    int index() const { return m_index; }
    qint64 pid() const { return m_process ? m_process->processId() : 0; }
    QString status() const { return m_status; }
    QString username() const { return m_username; }
    QRect windowGeometry() const { return m_windowGeometry; }

    /**
     * @brief Build gamescope command line arguments from config
     * @param config Configuration map
     * @return List of command line arguments
     */
    static QStringList buildGamescopeArgs(const QVariantMap &config);

    /**
     * @brief Build environment variables for the instance
     * @param config Configuration map
     * @param isPrimary Whether this is the primary instance
     * @return Environment variable assignments as strings
     */
    static QStringList buildEnvironment(const QVariantMap &config, bool isPrimary);

    /**
     * @brief Build the full command for secondary user execution
     * @param username Target username
     * @param environment Environment variables
     * @param gamescopeArgs Gamescope arguments
     * @param steamArgs Steam arguments
     * @param xauthPath Path to temporary xauth file for X11 authentication
     * @return Full shell command string
     */
    static QString buildSecondaryUserCommand(const QString &username,
                                              const QStringList &environment,
                                              const QStringList &gamescopeArgs,
                                              const QString &steamArgs,
                                              const QString &xauthPath = QString());

Q_SIGNALS:
    void runningChanged();
    void statusChanged();
    void configChanged();
    void started();
    void stopped();
    void errorOccurred(const QString &message);
    void outputReceived(const QString &output);

private Q_SLOTS:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();

private:
    void setStatus(const QString &status);
    
    /**
     * @brief Set up Wayland socket access for a secondary user via helper service
     * Calls the D-Bus helper to grant the secondary user permission to access
     * the primary user's Wayland socket via ACLs
     * @param username Target username
     * @return true if ACLs were set successfully
     */
    bool setupWaylandAccessForUser(const QString &username);
    
    /**
     * @brief Fallback method to set up Wayland access directly
     * Used when the helper service is not available
     * @param username Target username
     * @return true if ACLs were set successfully
     */
    bool setupWaylandAccessFallback(const QString &username);
    
    /**
     * @brief Clean up Wayland socket ACLs for a secondary user
     */
    void cleanupWaylandAccess();

    QProcess *m_process = nullptr;
    int m_index = -1;
    QString m_status;
    QString m_username;
    QRect m_windowGeometry;
    bool m_isPrimary = true;
    bool m_waylandAclSet = false;  // Track if we set up Wayland ACLs
};
