// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QDBusContext>
#include <QMap>
#include <QProcess>
#include <QString>
#include <QStringList>

/**
 * CouchPlayHelper - Privileged D-Bus service for split-screen gaming
 * 
 * This helper runs as a system service with elevated privileges to perform
 * operations that require root access:
 * - Creating Linux users for secondary players
 * - Enabling systemd linger for user sessions
 * - Setting up Wayland socket ACLs
 * - Changing input device ownership
 * 
 * D-Bus interface: io.github.hikaps.CouchPlayHelper
 * Object path: /io/github/hikaps/CouchPlayHelper
 */
class CouchPlayHelper : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.hikaps.CouchPlayHelper")

public:
    explicit CouchPlayHelper(QObject *parent = nullptr);
    ~CouchPlayHelper() override;

public Q_SLOTS:
    /**
     * Create a new Linux user for split-screen gaming
     * Also enables linger for the user automatically
     * 
     * @param username Desired username (lowercase, alphanumeric)
     * @param fullName Full name for the user
     * @return UID of created user, or 0 on failure
     */
    uint CreateUser(const QString &username, const QString &fullName);

    /**
     * Enable systemd linger for a user
     * Required for machinectl shell to work properly
     * 
     * @param username Username to enable linger for
     * @return true if successful
     */
    bool EnableLinger(const QString &username);

    /**
     * Check if linger is enabled for a user
     * 
     * @param username Username to check
     * @return true if linger is enabled
     */
    bool IsLingerEnabled(const QString &username);

    /**
     * Set up Wayland socket access for a user via ACLs
     * 
     * @param username Username to grant access to
     * @param compositorUid UID of the compositor user (Wayland socket owner)
     * @return true if successful
     */
    bool SetupWaylandAccess(const QString &username, uint compositorUid);

    /**
     * Remove Wayland socket access for a user
     * 
     * @param username Username to revoke access from
     * @param compositorUid UID of the compositor user
     * @return true if successful
     */
    bool RemoveWaylandAccess(const QString &username, uint compositorUid);

    /**
     * Change ownership of a device to a specific user
     * Used for input device isolation between instances
     * 
     * @param devicePath Path to the device (e.g., /dev/input/event5)
     * @param uid User ID to assign ownership to
     * @return true if successful
     */
    bool ChangeDeviceOwner(const QString &devicePath, uint uid);

    /**
     * Change ownership of multiple devices to a specific user
     * 
     * @param devicePaths List of device paths
     * @param uid User ID to assign ownership to
     * @return Number of devices successfully changed
     */
    int ChangeDeviceOwnerBatch(const QStringList &devicePaths, uint uid);

    /**
     * Reset device ownership to root
     * 
     * @param devicePath Path to the device
     * @return true if successful
     */
    bool ResetDeviceOwner(const QString &devicePath);

    /**
     * Reset ownership of all managed devices to root
     * Called automatically on helper shutdown
     * 
     * @return Number of devices successfully reset
     */
    int ResetAllDevices();

    /**
     * Get version of the helper daemon
     * 
     * @return Version string
     */
    QString Version();

    /**
     * Launch a gamescope instance as a specified user
     * 
     * This method handles all the complexity of running gamescope as any user:
     * - Sets up Wayland socket ACLs (if user differs from compositor user)
     * - Spawns the process via machinectl shell
     * - Returns the PID for tracking
     * 
     * @param username User to run as
     * @param compositorUid UID of compositor user (for Wayland socket access)
     * @param gamescopeArgs Gamescope command-line arguments
     * @param gameCommand Command to run inside gamescope (e.g., "steam -tenfoot")
     * @param environment Additional environment variables (VAR=value format)
     * @return PID of launched process, or 0 on failure
     */
    qint64 LaunchInstance(const QString &username, uint compositorUid,
                          const QStringList &gamescopeArgs,
                          const QString &gameCommand,
                          const QStringList &environment);

    /**
     * Stop a launched instance
     * 
     * @param pid Process ID to stop
     * @return true if the process was successfully signaled
     */
    bool StopInstance(qint64 pid);

    /**
     * Kill a launched instance forcefully
     * 
     * @param pid Process ID to kill
     * @return true if the process was successfully signaled
     */
    bool KillInstance(qint64 pid);

private:
    bool checkAuthorization(const QString &action);
    bool isValidDevicePath(const QString &path);
    
    // Internal helpers (not exposed via D-Bus)
    bool userExists(const QString &username);
    uint getUserUid(const QString &username);
    QString buildInstanceCommand(const QString &username, uint compositorUid,
                                  const QStringList &gamescopeArgs,
                                  const QString &gameCommand,
                                  const QStringList &environment);

    QStringList m_modifiedDevices;
    QMap<qint64, QProcess *> m_launchedProcesses;  // PID -> QProcess
};
