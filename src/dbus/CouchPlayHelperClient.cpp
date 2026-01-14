// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "CouchPlayHelperClient.h"
#include "../core/Logging.h"

#include <QDBusConnection>
#include <QDBusMessage>
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

bool CouchPlayHelperClient::deleteUser(const QString &username, bool removeHome)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("DeleteUser"),
        username,
        removeHome
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}

bool CouchPlayHelperClient::isInCouchPlayGroup(const QString &username)
{
    if (!m_available) {
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("IsInCouchPlayGroup"),
        username
    );

    if (!reply.isValid()) {
        return false;
    }

    return reply.value();
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

int CouchPlayHelperClient::mountSharedDirectories(const QString &username, uint compositorUid,
                                                    const QStringList &directories)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return -1;
    }

    if (directories.isEmpty()) {
        // Nothing to mount, that's fine
        return 0;
    }

    QDBusReply<int> reply = m_interface->call(
        QStringLiteral("MountSharedDirectories"),
        username,
        compositorUid,
        directories
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return -1;
    }

    return reply.value();
}

int CouchPlayHelperClient::unmountSharedDirectories(const QString &username)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return -1;
    }

    QDBusReply<int> reply = m_interface->call(
        QStringLiteral("UnmountSharedDirectories"),
        username
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return -1;
    }

    return reply.value();
}

int CouchPlayHelperClient::unmountAllSharedDirectories()
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return -1;
    }

    QDBusReply<int> reply = m_interface->call(
        QStringLiteral("UnmountAllSharedDirectories")
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return -1;
    }

    return reply.value();
}

bool CouchPlayHelperClient::copyFileToUser(const QString &sourcePath, const QString &targetPath,
                                            const QString &username)
{
    qCDebug(couchplayHelper) << "copyFileToUser:" << sourcePath << "->" << targetPath << "for" << username;
    
    if (!m_available) {
        qCWarning(couchplayHelper) << "copyFileToUser: Helper not available";
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    if (!m_interface->isValid()) {
        qCWarning(couchplayHelper) << "copyFileToUser: Interface not valid:"
                                   << m_interface->lastError().message();
        Q_EMIT errorOccurred(QStringLiteral("Helper interface not valid"));
        return false;
    }

    // Use QDBusMessage directly (more reliable than QDBusInterface::call)
    QDBusMessage msg = QDBusMessage::createMethodCall(
        SERVICE_NAME,
        OBJECT_PATH,
        INTERFACE_NAME,
        QStringLiteral("CopyFileToUser")
    );
    msg << sourcePath << targetPath << username;
    
    QDBusMessage replyMsg = QDBusConnection::systemBus().call(msg, QDBus::Block, 30000);

    if (replyMsg.type() == QDBusMessage::ErrorMessage) {
        qCWarning(couchplayHelper) << "copyFileToUser failed:" << replyMsg.errorMessage();
        Q_EMIT errorOccurred(replyMsg.errorMessage());
        return false;
    }

    if (replyMsg.type() != QDBusMessage::ReplyMessage) {
        qCWarning(couchplayHelper) << "copyFileToUser: Unexpected reply type:" << replyMsg.type();
        Q_EMIT errorOccurred(QStringLiteral("Unexpected D-Bus reply type"));
        return false;
    }

    bool result = replyMsg.arguments().value(0).toBool();
    if (!result) {
        qCWarning(couchplayHelper) << "copyFileToUser: Helper returned false";
    }

    return result;
}

bool CouchPlayHelperClient::createUserDirectory(const QString &path, const QString &username)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("CreateUserDirectory"),
        path,
        username
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}

bool CouchPlayHelperClient::setDirectoryAcl(const QString &path, const QString &username, bool recursive)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("SetDirectoryAcl"),
        path,
        username,
        recursive
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}

bool CouchPlayHelperClient::setPathAclWithParents(const QString &path, const QString &username)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    QDBusReply<bool> reply = m_interface->call(
        QStringLiteral("SetPathAclWithParents"),
        path,
        username
    );

    if (!reply.isValid()) {
        Q_EMIT errorOccurred(reply.error().message());
        return false;
    }

    return reply.value();
}

QString CouchPlayHelperClient::getUserSteamId(const QString &username)
{
    if (!m_available) {
        qWarning() << "CouchPlayHelperClient: Helper not available";
        return QString();
    }

    QDBusReply<QString> reply = m_interface->call(
        QStringLiteral("GetUserSteamId"),
        username
    );

    if (!reply.isValid()) {
        qWarning() << "CouchPlayHelperClient: GetUserSteamId failed:" << reply.error().message();
        return QString();
    }

    return reply.value();
}

bool CouchPlayHelperClient::writeFileToUser(const QByteArray &content, const QString &targetPath,
                                             const QString &username)
{
    qCDebug(couchplayHelper) << "writeFileToUser:" << content.size() << "bytes to" << targetPath << "for" << username;
    
    if (!m_available) {
        qCWarning(couchplayHelper) << "writeFileToUser: Helper not available";
        Q_EMIT errorOccurred(QStringLiteral("Helper not available"));
        return false;
    }

    if (!m_interface->isValid()) {
        qCWarning(couchplayHelper) << "writeFileToUser: Interface not valid:"
                                   << m_interface->lastError().message();
        Q_EMIT errorOccurred(QStringLiteral("Helper interface not valid"));
        return false;
    }

    // Use QDBusMessage directly for reliable byte array transfer
    QDBusMessage msg = QDBusMessage::createMethodCall(
        SERVICE_NAME,
        OBJECT_PATH,
        INTERFACE_NAME,
        QStringLiteral("WriteFileToUser")
    );
    msg << content << targetPath << username;
    
    QDBusMessage replyMsg = QDBusConnection::systemBus().call(msg, QDBus::Block, 30000);

    if (replyMsg.type() == QDBusMessage::ErrorMessage) {
        qCWarning(couchplayHelper) << "writeFileToUser failed:" << replyMsg.errorMessage();
        Q_EMIT errorOccurred(replyMsg.errorMessage());
        return false;
    }

    if (replyMsg.type() != QDBusMessage::ReplyMessage) {
        qCWarning(couchplayHelper) << "writeFileToUser: Unexpected reply type:" << replyMsg.type();
        Q_EMIT errorOccurred(QStringLiteral("Unexpected D-Bus reply type"));
        return false;
    }

    bool result = replyMsg.arguments().value(0).toBool();
    if (!result) {
        qCWarning(couchplayHelper) << "writeFileToUser: Helper returned false";
    }

    return result;
}
