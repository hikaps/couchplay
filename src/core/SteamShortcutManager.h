// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QFileDevice>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <qqmlintegration.h>

class SessionManager;

class SteamShortcutManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(SessionManager *sessionManager READ sessionManager WRITE setSessionManager NOTIFY sessionManagerChanged)

public:
    explicit SteamShortcutManager(QObject *parent = nullptr);

    QVariantList accounts() const;

    SessionManager *sessionManager() const
    {
        return m_sessionManager;
    }
    void setSessionManager(SessionManager *manager);

    Q_INVOKABLE bool prepare(const QString &profileName);
    Q_INVOKABLE void addToSteam(int accountIndex);

Q_SIGNALS:
    void accountsChanged();
    void sessionManagerChanged();
    void prepared();
    void registrationFinished(const QString &profileName, bool updated);
    void errorOccurred(const QString &message);

private:
    struct Account {
        QString root;
        QString accountId;
        QString kind;
        QString label;
        QString shortcutsPath;
    };

    void fail(const QString &message);
    bool discoverAccounts();
    bool readShortcuts(const QString &path, QByteArray *bytes, bool *exists, QString *error) const;
    bool writeAtomically(const QString &path,
                         const QByteArray &bytes,
                         QFileDevice::Permissions permissions,
                         QString *error) const;
    bool validateWritePath(const QString &path, bool allowMissingFile, QString *error) const;
    QString nativeGameModePath() const;
    QString shortcutExecutable(const Account &account, QString *launchOptions) const;

    SessionManager *m_sessionManager = nullptr;
    QList<Account> m_accounts;
    QString m_profileName;
    QString m_profilePath;
    bool m_prepared = false;
};
