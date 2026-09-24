// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QList>
#include <QPointer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <qqmlintegration.h>

#include <functional>

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
    Q_PROPERTY(bool cancellable READ cancellable NOTIFY cancellableChanged)
    Q_PROPERTY(bool gameMode READ gameMode NOTIFY gameModeChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(SessionManager *sessionManager READ sessionManager WRITE setSessionManager NOTIFY sessionManagerChanged)
    Q_PROPERTY(SessionRunner *sessionRunner READ sessionRunner WRITE setSessionRunner NOTIFY sessionRunnerChanged)

public:
    explicit SteamShortcutManager(QObject *parent = nullptr);
    ~SteamShortcutManager() override;

    QVariantList accounts() const;
    bool busy() const { return m_busy; }
    bool cancellable() const;
    bool gameMode() const { return m_gameMode; }
    QString status() const { return m_status; }

    SessionManager *sessionManager() const { return m_sessionManager; }
    void setSessionManager(SessionManager *manager);
    SessionRunner *sessionRunner() const { return m_sessionRunner; }
    void setSessionRunner(SessionRunner *runner);

    Q_INVOKABLE bool prepare(const QString &profileName);
    Q_INVOKABLE void addToSteam(int accountIndex, bool allowRestart);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openSteam(int accountIndex);

Q_SIGNALS:
    void accountsChanged();
    void busyChanged();
    void cancellableChanged();
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

    struct HostProcessGroup {
        quint64 generation = 0;
        qint64 processGroupId = 0;
        qint64 leaderStartTime = 0;
        QPointer<QProcess> process;
    };

    void setBusy(bool busy);
    void setGameMode(bool gameMode);
    void setStatus(const QString &status);
    void fail(const QString &message);
    void finishCancelled();
    void finishShutdownTimeout();
    void runHost(const QString &operation,
                 const QStringList &arguments,
                 const QByteArray &input,
                 std::function<void(int, const QByteArray &, const QString &)> callback,
                 int timeoutMs = -1);
    void parseProbe(const QByteArray &output);
    bool selectedAccountRunning() const;
    void beginWrite();
    void readShortcuts();
    void exportGameMode();
    void exportIcon();
    void exportLauncher();
    void commitShortcut();
    void pollSteamStopped();
    void scheduleSteamPoll();
    void stopSteamPoll();
    void reopenSteam(bool success, bool updated);
    void finishRegistration(bool updated, bool steamReopened);
    QString profileIdentity() const;
    QString profileName() const;
    QString profileFilePath() const;
    QString launcherContents(const QString &gameModePath) const;
    void setPhase(Phase phase);
    void detachHostProcessGroup(quint64 generation, qint64 processGroupId, QProcess *process);
    void releaseHostProcessGroup(quint64 generation, qint64 processGroupId);
    bool findHostProcessGroup(quint64 generation,
                              qint64 processGroupId,
                              HostProcessGroup *group = nullptr) const;
    bool hostProcessGroupIdentityMatches(quint64 generation, qint64 processGroupId) const;
    bool signalOwnedHostProcessGroup(quint64 generation, qint64 processGroupId, int signal);
    void scheduleHostProcessGroupEscalation(quint64 generation, qint64 processGroupId, QProcess *process);

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
    qint64 m_hostProcessGroupId = 0;
    qint64 m_hostProcessGroupStartTime = 0;
    quint64 m_hostProcessGeneration = 0;
    quint64 m_hostOperationGeneration = 0;
    QList<HostProcessGroup> m_pendingHostProcessGroups;
    QTimer *m_timeout = nullptr;
    QTimer *m_pollTimer = nullptr;
    std::function<void(int, const QByteArray &, const QString &)> m_hostCallback;
    bool m_busy = false;
    bool m_gameMode = false;
    bool m_wasRunning = false;
    bool m_reopenAttempted = false;
    bool m_shutdownConfirmed = false;
    QDeadlineTimer m_shutdownDeadline;
    quint64 m_pollGeneration = 0;
    bool m_cancelled = false;
    QString m_status;
};
