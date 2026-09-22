// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <QByteArray>
#include <QVariantList>
#include <qqmlintegration.h>

class SessionManager;
class SessionRunner;
class QProcess;
class QTimer;

class SteamShortcutManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool gameMode READ gameMode NOTIFY gameModeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(SessionManager *sessionManager READ sessionManager WRITE setSessionManager NOTIFY sessionManagerChanged)
    Q_PROPERTY(SessionRunner *sessionRunner READ sessionRunner WRITE setSessionRunner NOTIFY sessionRunnerChanged)

public:
    explicit SteamShortcutManager(QObject *parent = nullptr);
    ~SteamShortcutManager() override;

    QVariantList accounts() const;
    bool busy() const { return m_busy; }
    bool gameMode() const { return m_gameMode; }
    QString status() const { return m_status; }

    SessionManager *sessionManager() const { return m_sessionManager; }
    void setSessionManager(SessionManager *manager);
    SessionRunner *sessionRunner() const { return m_sessionRunner; }
    void setSessionRunner(SessionRunner *runner);

    Q_INVOKABLE void prepare(const QString &profileName);
    Q_INVOKABLE void addToSteam(int accountIndex, bool allowRestart);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openSteam(int accountIndex);

Q_SIGNALS:
    void accountsChanged();
    void busyChanged();
    void gameModeChanged();
    void statusChanged();
    void sessionManagerChanged();
    void sessionRunnerChanged();
    void prepared();
    void registrationFinished(const QString &profileName, bool updated, bool steamReopened);
    void errorOccurred(const QString &message);

private:
    struct Account {
        QString root;
        QString accountId;
        QString kind;
        QString label;
        bool running = false;
    };

    enum class Phase { Idle, Probing, Prepared, ClosingSteam, Writing, ReopeningSteam };

    void setBusy(bool busy);
    void setGameMode(bool gameMode);
    void setStatus(const QString &status);
    void fail(const QString &message);
    void runHost(const QString &operation,
                 const QStringList &arguments,
                 const QByteArray &input,
                 std::function<void(int, const QByteArray &, const QString &)> callback);
    void parseProbe(const QByteArray &output);
    void beginWrite();
    void readShortcuts();
    void exportGameMode();
    void exportIcon();
    void exportLauncher();
    void commitShortcut();
    void pollSteamStopped();
    void reopenSteam(bool success, bool updated);
    void finishRegistration(bool updated, bool steamReopened);
    QString profileIdentity() const;
    QString profileName() const;
    QString profileFilePath() const;
    QString launcherContents(const QString &gameModePath) const;
    void setPhase(Phase phase);

    SessionManager *m_sessionManager = nullptr;
    SessionRunner *m_sessionRunner = nullptr;
    QList<Account> m_accounts;
    QString m_selectedProfile;
    QString m_profilePath;
    QString m_identity;
    QString m_expectedHash;
    QString m_gameModePath;
    QString m_iconPath;
    QString m_launcherPath;
    QByteArray m_shortcuts;
    QByteArray m_newShortcuts;
    QString m_root;
    QString m_accountId;
    QString m_kind;
    Phase m_phase = Phase::Idle;
    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;
    std::function<void(int, const QByteArray &, const QString &)> m_hostCallback;
    bool m_busy = false;
    bool m_gameMode = false;
    bool m_wasRunning = false;
    bool m_reopenAttempted = false;
    bool m_cancelled = false;
    QString m_status;
};
