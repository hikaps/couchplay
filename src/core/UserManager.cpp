// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "UserManager.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>
#include <QDir>

#include <unistd.h>
#include <pwd.h>

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

void UserManager::refresh()
{
    m_users.clear();
    parseUsers();
    Q_EMIT usersChanged();
}

void UserManager::parseUsers()
{
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

        // Filter: only regular users (UID >= 1000 and < 65534)
        // Also check for valid login shell (not nologin or false)
        if (uid < 1000 || uid >= 65534) {
            continue;
        }

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
        map[QStringLiteral("isCurrent")] = (user.username == m_currentUser);
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

    // User creation will be done via the privileged helper
    // For now, just emit a signal indicating the request
    // The actual implementation will call the D-Bus helper

    Q_EMIT errorOccurred(QStringLiteral("User creation requires the CouchPlay helper. Please run the install script."));
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
    for (const auto &user : m_users) {
        if (user.username == username) {
            return true;
        }
    }

    // Also check getpwnam for users not in our filtered list
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    return pw != nullptr;
}
