// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QDBusInterface>
#include <QString>
#include <QStringList>
#include <qqmlintegration.h>

/**
 * @brief D-Bus client for the privileged CouchPlay helper
 */
class CouchPlayHelperClient : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ isAvailable NOTIFY availabilityChanged)

public:
    explicit CouchPlayHelperClient(QObject *parent = nullptr);
    ~CouchPlayHelperClient() override;

    /**
     * @brief Check if the helper is available
     */
    bool isAvailable() const { return m_available; }

    /**
     * @brief Set device ownership for a specific user
     */
    Q_INVOKABLE bool setDeviceOwner(const QString &devicePath, int uid);

    /**
     * @brief Restore device ownership to root:input
     */
    Q_INVOKABLE bool restoreDeviceOwner(const QString &devicePath);

    /**
     * @brief Restore all modified devices
     */
    Q_INVOKABLE void restoreAllDevices();

    /**
     * @brief Create a new user account
     */
    Q_INVOKABLE bool createUser(const QString &username);

    /**
     * @brief Delete a CouchPlay user account
     * Only users in the couchplay group can be deleted
     * 
     * @param username Username to delete (must be in couchplay group)
     * @param removeHome If true, also delete the user's home directory
     * @return true if successful
     */
    Q_INVOKABLE bool deleteUser(const QString &username, bool removeHome);

    /**
     * @brief Check if a user is in the couchplay group
     * 
     * @param username Username to check
     * @return true if user is in couchplay group
     */
    Q_INVOKABLE bool isInCouchPlayGroup(const QString &username);

    /**
     * @brief Launch a gamescope instance as a specified user
     * @param username User to run as
     * @param compositorUid UID of compositor user (for Wayland socket access)
     * @param gamescopeArgs Gamescope command-line arguments
     * @param gameCommand Command to run inside gamescope
     * @param environment Additional environment variables (VAR=value format)
     * @return PID of launched process, or 0 on failure
     */
    Q_INVOKABLE qint64 launchInstance(const QString &username, uint compositorUid,
                                       const QStringList &gamescopeArgs,
                                       const QString &gameCommand,
                                       const QStringList &environment);

    /**
     * @brief Stop a launched instance gracefully (SIGTERM)
     * @param pid Process ID to stop
     * @return true if successfully signaled
     */
    Q_INVOKABLE bool stopInstance(qint64 pid);

    /**
     * @brief Kill a launched instance forcefully (SIGKILL)
     * @param pid Process ID to kill
     * @return true if successfully signaled
     */
    Q_INVOKABLE bool killInstance(qint64 pid);

    /**
     * @brief Check helper availability
     */
    Q_INVOKABLE void checkAvailability();

    /**
     * @brief Mount shared directories for a user
     * @param username Target user to mount for
     * @param compositorUid UID of compositor user (source paths are resolved from this user's home)
     * @param directories List of directories in "source|alias" format
     * @return Number of successful mounts, or -1 on error
     */
    Q_INVOKABLE int mountSharedDirectories(const QString &username, uint compositorUid,
                                            const QStringList &directories);

    /**
     * @brief Unmount shared directories for a user
     * @param username Target user to unmount for
     * @return Number of successful unmounts, or -1 on error
     */
    Q_INVOKABLE int unmountSharedDirectories(const QString &username);

    /**
     * @brief Unmount all shared directories for all users
     * @return Number of successful unmounts, or -1 on error
     */
    Q_INVOKABLE int unmountAllSharedDirectories();

    /**
     * @brief Copy a file to a user's directory with proper ownership
     * @param sourcePath Source file path
     * @param targetPath Target file path (will be created/overwritten)
     * @param username Target user (file will be owned by this user)
     * @return true if successful
     */
    Q_INVOKABLE bool copyFileToUser(const QString &sourcePath, const QString &targetPath,
                                     const QString &username);

    /**
     * @brief Create a directory with proper ownership
     * @param path Directory path to create
     * @param username User who should own the directory
     * @return true if successful
     */
    Q_INVOKABLE bool createUserDirectory(const QString &path, const QString &username);

    /**
     * @brief Set ACL on a directory to grant a user read+execute access
     * @param path Directory path to set ACL on
     * @param username User to grant access to
     * @param recursive Apply recursively to all contents
     * @return true if successful
     */
    Q_INVOKABLE bool setDirectoryAcl(const QString &path, const QString &username, bool recursive);

    /**
     * @brief Set ACLs on a path and all parent directories needed for traversal
     * 
     * This is useful for paths on external drives (e.g., /run/media/user/...)
     * where the user needs rx access to all parent directories to reach the
     * target path.
     * 
     * @param path Target path to set ACL on
     * @param username User to grant access to
     * @return true if successful
     */
    Q_INVOKABLE bool setPathAclWithParents(const QString &path, const QString &username);

    /**
     * @brief Get a user's Steam user ID via the privileged helper
     * @param username User to get Steam ID for
     * @return Steam user ID string, or empty if not found
     */
    Q_INVOKABLE QString getUserSteamId(const QString &username);

    /**
     * @brief Write content directly to a file in a user's directory
     * @param content File content as bytes
     * @param targetPath Target file path (will be created/overwritten)
     * @param username Target user (file will be owned by this user)
     * @return true if successful
     */
    Q_INVOKABLE bool writeFileToUser(const QByteArray &content, const QString &targetPath,
                                      const QString &username);

Q_SIGNALS:
    void availabilityChanged();
    void errorOccurred(const QString &message);

private:
    QDBusInterface *m_interface = nullptr;
    bool m_available = false;
};
