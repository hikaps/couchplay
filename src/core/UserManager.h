// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QSet>
#include <QVariantMap>
#include <qqmlintegration.h>

class CouchPlayHelperClient;

/**
 * @brief Manages Linux user accounts for multi-user gaming
 * 
 * Only shows users in the 'couchplay' group, excluding the current user.
 * This ensures strict boundaries - only users created by CouchPlay can
 * be managed, assigned to sessions, or deleted.
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
     * @brief Set the helper client for privileged operations
     * Note: This should be called from C++, not QML
     */
    void setHelperClient(CouchPlayHelperClient *client);
    CouchPlayHelperClient *helperClient() const { return m_helperClient; }

    /**
     * @brief Set the helper client from QML (takes QObject* for type safety)
     */
    Q_INVOKABLE void setHelper(QObject *helper);

    /**
     * @brief Refresh the list of available users
     */
    Q_INVOKABLE void refresh();

    /**
     * @brief Get the current logged-in user
     */
    QString currentUser() const { return m_currentUser; }

    /**
     * @brief Get list of users in the couchplay group (excluding current user)
     */
    QVariantList usersAsVariant() const;

    /**
     * @brief Create a new user for gaming
     * @param username The username for the new user
     * @return true if creation was requested (via helper)
     */
    Q_INVOKABLE bool createUser(const QString &username);

    /**
     * @brief Delete a CouchPlay user
     * @param username The username to delete
     * @param removeHome If true, also delete the user's home directory
     * @return true if deletion was successful
     */
    Q_INVOKABLE bool deleteUser(const QString &username, bool removeHome);

    /**
     * @brief Check if a username is valid
     */
    Q_INVOKABLE bool isValidUsername(const QString &username) const;

    /**
     * @brief Check if user exists (in any group)
     */
    Q_INVOKABLE bool userExists(const QString &username) const;

    /**
     * @brief Check if user is in the couchplay group
     */
    Q_INVOKABLE bool isInCouchPlayGroup(const QString &username) const;

Q_SIGNALS:
    void usersChanged();
    void userCreated(const QString &username);
    void userDeleted(const QString &username);
    void errorOccurred(const QString &message);

private:
    void parseUsers();
    QSet<QString> getCouchPlayGroupMembers() const;

    struct UserInfo {
        QString username;
        int uid;
        QString homeDir;
        QString shell;
    };

    CouchPlayHelperClient *m_helperClient = nullptr;
    QString m_currentUser;
    QList<UserInfo> m_users;
};
