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
    checkAvailability();
}

CouchPlayHelperClient::~CouchPlayHelperClient()
{
    // Restore all devices on destruction
    if (m_available) {
        restoreAllDevices();
    }
}

void CouchPlayHelperClient::checkAvailability()
{
    m_interface = new QDBusInterface(
        SERVICE_NAME,
        OBJECT_PATH,
        INTERFACE_NAME,
        QDBusConnection::systemBus(),
        this
    );

    bool wasAvailable = m_available;
    m_available = m_interface->isValid();

    if (m_available != wasAvailable) {
        Q_EMIT availabilityChanged();
    }

    if (!m_available) {
        qWarning() << "CouchPlay helper is not available. Run install-helper.sh to set it up.";
    }
}

bool CouchPlayHelperClient::setDeviceOwner(const QString &devicePath, int uid, int gid)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("SetDeviceOwner"),
        devicePath,
        static_cast<uint>(uid),
        static_cast<uint>(gid)
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
        QStringLiteral("RestoreDeviceOwner"),
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

    m_interface->call(QStringLiteral("RestoreAllDevices"));
}

bool CouchPlayHelperClient::createUser(const QString &username)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("CreateUser"),
        username
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}
