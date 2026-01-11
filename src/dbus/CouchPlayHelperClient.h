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

Q_SIGNALS:
    void availabilityChanged();
    void errorOccurred(const QString &message);

private:
    QDBusInterface *m_interface = nullptr;
    bool m_available = false;
};
