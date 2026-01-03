// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>
#include <qqmlintegration.h>

/**
 * @brief Manages Linux user accounts for multi-user gaming
 */
class UserManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString currentUser READ currentUser CONSTANT)
    Q_PROPERTY(QVariantList users READ usersAsVariant NOTIFY usersChanged)

public:
    explicit UserManager(QObject *parent = nullptr);
    ~UserManager() override;

    /**
     * @brief Refresh the list of available users
     */
    Q_INVOKABLE void refresh();

    /**
     * @brief Get the current logged-in user
     */
    QString currentUser() const { return m_currentUser; }

    /**
     * @brief Get list of users suitable for gaming
     * Filters out system users, returns regular users
     */
    QVariantList usersAsVariant() const;

    /**
     * @brief Create a new user for gaming
     * @param username The username for the new user
     * @return true if creation was requested (via helper)
     */
    Q_INVOKABLE bool createUser(const QString &username);

    /**
     * @brief Check if a username is valid
     */
    Q_INVOKABLE bool isValidUsername(const QString &username) const;

    /**
     * @brief Check if user exists
     */
    Q_INVOKABLE bool userExists(const QString &username) const;

Q_SIGNALS:
    void usersChanged();
    void userCreated(const QString &username);
    void errorOccurred(const QString &message);

private:
    void parseUsers();

    struct UserInfo {
        QString username;
        int uid;
        QString homeDir;
        QString shell;
    };

    QString m_currentUser;
    QList<UserInfo> m_users;
};
