// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "CouchPlayHelperClient.h"

#include <QDBusConnection>
#include <QDBusReply>
#include <QDebug>

static const QString SERVICE_NAME = QStringLiteral("io.github.hikaps.CouchPlayHelper");
static const QString OBJECT_PATH = QStringLiteral("/io/github/hikaps/CouchPlayHelper");
static const QString INTERFACE_NAME = QStringLiteral("io.github.hikaps.CouchPlayHelper");

CouchPlayHelperClient::CouchPlayHelperClient(QObject *parent)
    : QObject(parent)
{
    m_interface = new QDBusInterface(
        SERVICE_NAME,
        OBJECT_PATH,
        INTERFACE_NAME,
        QDBusConnection::systemBus(),
        this
    );

    if (!m_interface->isValid()) {
        qWarning() << "CouchPlay helper interface not valid:" << m_interface->lastError().message();
        qWarning() << "Run install-helper.sh to set it up.";
        m_available = false;
        return;
    }

    // Verify we can actually call a method
    QDBusReply<QString> reply = m_interface->call(QStringLiteral("Version"));
    if (reply.isValid()) {
        qWarning() << "CouchPlay helper connected, version:" << reply.value();
        m_available = true;
    } else {
        qWarning() << "CouchPlay helper call failed:" << reply.error().message();
        m_available = false;
    }
}

CouchPlayHelperClient::~CouchPlayHelperClient()
{
    if (m_available) {
        restoreAllDevices();
    }
}

void CouchPlayHelperClient::checkAvailability()
{
    bool wasAvailable = m_available;
    m_available = m_interface && m_interface->isValid();

    if (m_available != wasAvailable) {
        Q_EMIT availabilityChanged();
    }
}

bool CouchPlayHelperClient::setDeviceOwner(const QString &devicePath, int uid)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("ChangeDeviceOwner"),
        devicePath,
        static_cast<uint>(uid)
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}

bool CouchPlayHelperClient::restoreDeviceOwner(const QString &devicePath)
{
    if (!m_available) {
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("ResetDeviceOwner"),
        devicePath
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}

void CouchPlayHelperClient::restoreAllDevices()
{
    if (!m_available) {
        return;
    }

    m_interface->call(QStringLiteral("ResetAllDevices"));
}

bool CouchPlayHelperClient::createUser(const QString &username)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    // The helper expects username and fullName parameters
    // Use a default fullName based on the username
    QString fullName = QStringLiteral("CouchPlay Player (%1)").arg(username);

    QDBusReply<uint> reply = m_interface->call(
        QStringLiteral("CreateUser"),
        username,
        fullName
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    // CreateUser returns the UID of the new user, or 0 on failure
    return reply.value() > 0;
}

qint64 CouchPlayHelperClient::launchInstance(const QString &username, uint compositorUid,
                                              const QStringList &gamescopeArgs,
                                              const QString &gameCommand,
                                              const QStringList &environment)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return 0;
    }

    QDBusReply<qint64> reply = m_interface->call(
        QStringLiteral("LaunchInstance"),
        username,
        compositorUid,
        gamescopeArgs,
        gameCommand,
        environment
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return 0;
    }

    return reply.value();
}

bool CouchPlayHelperClient::stopInstance(qint64 pid)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("StopInstance"),
        pid
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}

bool CouchPlayHelperClient::killInstance(qint64 pid)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("KillInstance"),
        pid
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}
