// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QDBusContext>
#include <QString>
#include <QStringList>

/**
 * CouchPlayHelper - Privileged D-Bus service for device ownership management
 * 
 * This helper runs as a system service with elevated privileges to perform
 * operations that require root access, such as changing device ownership
 * for input device isolation between gamescope instances.
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
     * Change ownership of a device to a specific user
     * 
     * @param devicePath Path to the device (e.g., /dev/input/event5)
     * @param uid User ID to assign ownership to
     * @return true if successful, false otherwise
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
     * @return true if successful, false otherwise
     */
    bool ResetDeviceOwner(const QString &devicePath);

    /**
     * Reset ownership of all managed devices to root
     * 
     * @return Number of devices successfully reset
     */
    int ResetAllDevices();

    /**
     * Create a new Linux user for split-screen gaming
     * 
     * @param username Desired username
     * @param fullName Full name for the user
     * @return UID of created user, or 0 on failure
     */
    uint CreateUser(const QString &username, const QString &fullName);

    /**
     * Check if a user exists
     * 
     * @param username Username to check
     * @return true if user exists
     */
    bool UserExists(const QString &username);

    /**
     * Get the UID of a username
     * 
     * @param username Username to look up
     * @return UID or 0 if not found
     */
    uint GetUserUid(const QString &username);

    /**
     * Configure PipeWire for a specific user
     * Sets up audio routing for split-screen sessions
     * 
     * @param uid User ID to configure
     * @return true if successful
     */
    bool ConfigurePipeWireForUser(uint uid);

    /**
     * Get version of the helper daemon
     * 
     * @return Version string
     */
    QString Version();

private:
    /**
     * Verify caller is authorized via PolicyKit
     * 
     * @param action PolicyKit action to check
     * @return true if authorized
     */
    bool checkAuthorization(const QString &action);

    /**
     * Validate device path is a legitimate input device
     * 
     * @param path Device path to validate
     * @return true if valid
     */
    bool isValidDevicePath(const QString &path);

    /**
     * Track devices we've modified for cleanup
     */
    QStringList m_modifiedDevices;
};
