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
     * Also enables linger for the user automatically and adds to couchplay group
     * 
     * @param username Desired username (lowercase, alphanumeric)
     * @param fullName Full name for the user
     * @return UID of created user, or 0 on failure
     */
    uint CreateUser(const QString &username, const QString &fullName);

    /**
     * Delete a CouchPlay user account
     * Only users in the couchplay group can be deleted
     * 
     * @param username Username to delete (must be in couchplay group)
     * @param removeHome If true, also delete the user's home directory
     * @return true if successful
     */
    bool DeleteUser(const QString &username, bool removeHome);

    /**
     * Check if a user is in the couchplay group
     * 
     * @param username Username to check
     * @return true if user is in couchplay group
     */
    bool IsInCouchPlayGroup(const QString &username);

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

    /**
     * Mount shared directories for a user
     * 
     * Bind-mounts the specified directories into the user's home directory.
     * For paths inside the compositor's home, they're mounted at the same
     * relative path. For paths outside, they're mounted at the specified alias
     * or under ~/.couchplay/mounts/ if no alias is provided.
     * 
     * @param username Target user (must exist)
     * @param compositorUid UID of the compositor user (for determining home-relative paths)
     * @param directories List of "source|alias" strings (alias empty for home-relative)
     * @return Number of successful mounts
     */
    int MountSharedDirectories(const QString &username, uint compositorUid,
                               const QStringList &directories);

    /**
     * Unmount all shared directories for a user
     * 
     * @param username Target user
     * @return Number of successful unmounts
     */
    int UnmountSharedDirectories(const QString &username);

    /**
     * Unmount all shared directories for all users
     * Called on session stop or app exit
     * 
     * @return Total number of unmounts performed
     */
    int UnmountAllSharedDirectories();

    /**
     * Copy a file to a user's directory with proper ownership
     * 
     * Used for copying Steam config files (libraryfolders.vdf, shortcuts.vdf)
     * to gaming users' Steam directories.
     * 
     * @param sourcePath Source file path
     * @param targetPath Target file path (will be created/overwritten)
     * @param username Target user (file will be owned by this user)
     * @return true if successful
     */
    bool CopyFileToUser(const QString &sourcePath, const QString &targetPath,
                        const QString &username);

    /**
     * Create a directory with proper ownership
     * 
     * Creates the directory and all parent directories, setting ownership
     * on each created directory.
     * 
     * @param path Directory path to create
     * @param username User who should own the directory
     * @return true if successful
     */
    bool CreateUserDirectory(const QString &path, const QString &username);

    /**
     * Set ACL on a directory to grant a user read+execute access
     * 
     * Uses setfacl to grant the specified user access to the directory.
     * When recursive is true, applies ACLs to all files and subdirectories.
     * 
     * @param path Directory path to set ACL on
     * @param username User to grant access to
     * @param recursive Apply recursively to all contents
     * @return true if successful
     */
    bool SetDirectoryAcl(const QString &path, const QString &username, bool recursive);

    /**
     * Set ACLs on a path and all parent directories needed for traversal
     * 
     * This is useful for paths on external drives or in /run/media/username/
     * where the user needs rx access to all parent directories to reach the
     * target path. Traversal stops at safe boundaries like /run/media, /mnt,
     * /media, or the filesystem root.
     * 
     * @param path Target path to set ACL on
     * @param username User to grant access to
     * @return true if successful
     */
    bool SetPathAclWithParents(const QString &path, const QString &username);

    /**
     * Get a user's Steam user ID
     * 
     * Looks in the user's Steam userdata directory to find their Steam ID.
     * This requires root access since other users' home directories are
     * typically not readable.
     * 
     * @param username User to get Steam ID for
     * @return Steam user ID string, or empty if not found
     */
    QString GetUserSteamId(const QString &username);

    /**
     * Write content directly to a file in a user's directory
     * 
     * Used for writing generated config files (e.g., shortcuts.vdf) directly
     * to gaming users' directories without needing a temp file.
     * 
     * @param content File content as bytes
     * @param targetPath Target file path (will be created/overwritten)
     * @param username Target user (file will be owned by this user)
     * @return true if successful
     */
    bool WriteFileToUser(const QByteArray &content, const QString &targetPath,
                         const QString &username);

private:
    bool checkAuthorization(const QString &action);
    bool isValidDevicePath(const QString &path);
    
    // Internal helpers (not exposed via D-Bus)
    bool userExists(const QString &username);
    uint getUserUid(const QString &username);
    QString getUserHome(const QString &username);
    QString getUserHomeByUid(uint uid);
    QString buildInstanceCommand(const QString &username, uint compositorUid,
                                  const QStringList &gamescopeArgs,
                                  const QString &gameCommand,
                                  const QStringList &environment);
    QString computeMountTarget(const QString &source, const QString &alias,
                               const QString &userHome, const QString &compositorHome);

    QStringList m_modifiedDevices;
    QMap<qint64, QProcess *> m_launchedProcesses;  // PID -> QProcess
    
    // Track active mounts per user for cleanup
    struct MountInfo {
        QString source;
        QString target;
    };
    QMap<QString, QList<MountInfo>> m_activeMounts;  // username -> list of mounts
};
