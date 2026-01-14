// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "UserManager.h"
#include "../dbus/CouchPlayHelperClient.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>
#include <QDir>

#include <unistd.h>
#include <pwd.h>
#include <grp.h>

// Name of the couchplay group for managed users
static const QString COUCHPLAY_GROUP = QStringLiteral("couchplay");

UserManager::UserManager(QObject *parent)
    : QObject(parent)
{
    // Get current user
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        m_currentUser = QString::fromLocal8Bit(pw->pw_name);
    }

    refresh();
}

UserManager::~UserManager() = default;

void UserManager::setHelperClient(CouchPlayHelperClient *client)
{
    m_helperClient = client;
}

void UserManager::setHelper(QObject *helper)
{
    setHelperClient(qobject_cast<CouchPlayHelperClient *>(helper));
}

void UserManager::refresh()
{
    m_users.clear();
    parseUsers();
    Q_EMIT usersChanged();
}

QSet<QString> UserManager::getCouchPlayGroupMembers() const
{
    QSet<QString> members;
    
    struct group *grp = getgrnam(COUCHPLAY_GROUP.toLocal8Bit().constData());
    if (!grp) {
        return members;  // Group doesn't exist yet
    }

    // Get all members from the group
    for (char **member = grp->gr_mem; *member != nullptr; ++member) {
        members.insert(QString::fromLocal8Bit(*member));
    }

    // Also check for users with couchplay as primary group
    QFile file(QStringLiteral("/etc/passwd"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            QString line = stream.readLine();
            QStringList parts = line.split(QLatin1Char(':'));
            if (parts.size() >= 4) {
                int userGid = parts[3].toInt();
                if (static_cast<gid_t>(userGid) == grp->gr_gid) {
                    members.insert(parts[0]);
                }
            }
        }
        file.close();
    }

    return members;
}

void UserManager::parseUsers()
{
    // Get couchplay group members
    QSet<QString> couchplayMembers = getCouchPlayGroupMembers();
    
    QFile file(QStringLiteral("/etc/passwd"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to read /etc/passwd"));
        return;
    }

    QTextStream stream(&file);

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        QStringList parts = line.split(QLatin1Char(':'));

        if (parts.size() < 7) {
            continue;
        }

        QString username = parts[0];
        int uid = parts[2].toInt();
        QString homeDir = parts[5];
        QString shell = parts[6];

        // Skip current user - they run the app, not available for assignment
        if (username == m_currentUser) {
            continue;
        }

        // Only include users in the couchplay group
        if (!couchplayMembers.contains(username)) {
            continue;
        }

        // Filter: only regular users (UID >= 1000 and < 65534)
        if (uid < 1000 || uid >= 65534) {
            continue;
        }

        // Check for valid login shell (not nologin or false)
        if (shell.contains(QStringLiteral("nologin")) || 
            shell.contains(QStringLiteral("false"))) {
            continue;
        }

        // Check if home directory exists
        if (!QDir(homeDir).exists()) {
            continue;
        }

        UserInfo user;
        user.username = username;
        user.uid = uid;
        user.homeDir = homeDir;
        user.shell = shell;

        m_users.append(user);
    }

    file.close();
}

QVariantList UserManager::usersAsVariant() const
{
    QVariantList list;

    for (const auto &user : m_users) {
        QVariantMap map;
        map[QStringLiteral("username")] = user.username;
        map[QStringLiteral("uid")] = user.uid;
        map[QStringLiteral("homeDir")] = user.homeDir;
        // isCurrent is always false since we filter out current user
        map[QStringLiteral("isCurrent")] = false;
        list.append(map);
    }

    return list;
}

bool UserManager::createUser(const QString &username)
{
    if (!isValidUsername(username)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid username"));
        return false;
    }

    if (userExists(username)) {
        Q_EMIT errorOccurred(QStringLiteral("User already exists"));
        return false;
    }

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        Q_EMIT errorOccurred(QStringLiteral("Helper service not available. Please run install-helper.sh"));
        return false;
    }

    if (m_helperClient->createUser(username)) {
        refresh();
        Q_EMIT userCreated(username);
        return true;
    }

    return false;
}

bool UserManager::deleteUser(const QString &username, bool removeHome)
{
    if (!isValidUsername(username)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid username"));
        return false;
    }

    if (!userExists(username)) {
        Q_EMIT errorOccurred(QStringLiteral("User does not exist"));
        return false;
    }

    // Cannot delete current user (extra safety check)
    if (username == m_currentUser) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot delete the current user"));
        return false;
    }

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        Q_EMIT errorOccurred(QStringLiteral("Helper service not available"));
        return false;
    }

    // Helper will verify user is in couchplay group
    if (m_helperClient->deleteUser(username, removeHome)) {
        refresh();
        Q_EMIT userDeleted(username);
        return true;
    }

    return false;
}

bool UserManager::isValidUsername(const QString &username) const
{
    if (username.isEmpty() || username.length() > 32) {
        return false;
    }

    // Must start with lowercase letter
    if (!username[0].isLower()) {
        return false;
    }

    // Can only contain lowercase letters, numbers, underscore, dash
    static const QRegularExpression validChars(QStringLiteral("^[a-z][a-z0-9_-]*$"));
    return validChars.match(username).hasMatch();
}

bool UserManager::userExists(const QString &username) const
{
    // Check getpwnam for any user
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    return pw != nullptr;
}

bool UserManager::isInCouchPlayGroup(const QString &username) const
{
    QSet<QString> members = getCouchPlayGroupMembers();
    return members.contains(username);
}
