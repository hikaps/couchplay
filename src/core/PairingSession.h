// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <qqmlintegration.h>

#include "SessionManager.h"
#include "SessionRunner.h"
#include "SettingsManager.h"
#include "StreamManager.h"
#include "VirtualDisplayManager.h"

/**
 * Temporary Sunshine for pairing a Moonlight client outside a session, keyed by gaming user.
 *
 * Sunshine refuses to start without a capture output, so a virtual display is created first.
 * The pairing session owns its own VirtualDisplayManager and StreamManager, separate from
 * SessionRunner's. It always uses player slot 0 internally (display couchplay-p1, stream entry index 0),
 * which is the same name a session gives player 1, so a pairing session and a running session must never
 * coexist: start() is refused while the session is active, and a session that becomes active stops the pairing.
 * Sunshine binds the user's stable port (SunshineConfig::userPort), so the address a client pairs with
 * is the one the user's streams use later.
 */
class PairingSession : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString username READ username NOTIFY usernameChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(StreamManager *streamManager READ streamManager CONSTANT)
    Q_PROPERTY(SessionManager *sessionManager READ sessionManager WRITE setSessionManager NOTIFY sessionManagerChanged)
    Q_PROPERTY(SessionRunner *sessionRunner READ sessionRunner WRITE setSessionRunner NOTIFY sessionRunnerChanged)
    Q_PROPERTY(SettingsManager *settingsManager READ settingsManager WRITE setSettingsManager NOTIFY settingsManagerChanged)

public:
    enum State {
        Idle,
        Starting,
        Ready,
        Failed,
    };
    Q_ENUM(State)

    explicit PairingSession(QObject *parent = nullptr);
    ~PairingSession() override;

    State state() const { return m_state; }
    QString error() const { return m_error; }
    QString username() const { return m_username; }
    bool busy() const { return m_state == Starting || m_state == Ready || m_stopping; }
    StreamManager *streamManager() const { return m_streamManager; }

    SessionManager *sessionManager() const { return m_sessionManager; }
    void setSessionManager(SessionManager *manager);
    SessionRunner *sessionRunner() const { return m_sessionRunner; }
    void setSessionRunner(SessionRunner *runner);
    SettingsManager *settingsManager() const { return m_settingsManager; }
    void setSettingsManager(SettingsManager *manager);

    Q_INVOKABLE void start(const QString &username);
    Q_INVOKABLE void stop();
    /**
     * Unpair one Moonlight client of a gaming user. Uses the user's live stream while a session runs it,
     * the open pairing session when it is Ready for this user, and otherwise starts a temporary pairing
     * Sunshine for the call and stops it again. The outcome arrives as unpairFinished().
     */
    Q_INVOKABLE void unpairClient(const QString &username, const QString &uuid);
    Q_INVOKABLE int pairedClientCount(const QString &username) const;
    /// Paired Moonlight clients of the gaming user: [{name, uuid}]
    Q_INVOKABLE QVariantList pairedClients(const QString &username) const;
    /// The user's stable Sunshine base port as text; empty for an empty username
    Q_INVOKABLE QString hostPort(const QString &username) const;

    // Test seams
    void setVirtualDisplayProgramOverride(const QString &program);
    void setScreenLookup(VirtualDisplayManager::ScreenLookup lookup);
    void setVirtualDisplayTimeoutMs(int timeoutMs);
    void setSunshineProgramOverride(const QString &program);
    VirtualDisplayManager *virtualDisplays() const { return m_virtualDisplays; }

Q_SIGNALS:
    void stateChanged();
    void errorChanged();
    void usernameChanged();
    void busyChanged();
    void sessionManagerChanged();
    void sessionRunnerChanged();
    void settingsManagerChanged();
    // Paired clients of this gaming user may have changed
    void pairedChanged(const QString &username);
    // Outcome of unpairClient(); message is empty on success
    void unpairFinished(const QString &username, bool ok, const QString &message);

private:
    void setState(State state);
    void setError(const QString &error);
    void setUsername(const QString &username);
    void updateBusy();
    void fail(const QString &message);
    void teardown();
    void onDisplayReady(int index, const QString &outputName);
    void onDisplayFailed(int index, const QString &message);
    void onStreamStarted(int index);
    void onStreamError(int index, const QString &message);
    void onStreamStopped(int index);
    void onUnpairResult(StreamManager *manager, int index, bool ok, const QString &message);
    void finishUnpair(bool ok, const QString &message);
    bool sessionActive() const;

    State m_state = Idle;
    QString m_error;
    QString m_username;
    bool m_stopping = false;
    bool m_lastBusy = false;
    StreamManager *m_streamManager = nullptr;
    VirtualDisplayManager *m_virtualDisplays = nullptr;
    QPointer<SessionManager> m_sessionManager;
    QPointer<SessionRunner> m_sessionRunner;
    QPointer<SettingsManager> m_settingsManager;
    // Pending unpairClient(); empty uuid means none
    QString m_unpairUuid;
    QString m_unpairUser;
    bool m_unpairTemporary = false;
    int m_unpairIndex = 0;
    QPointer<StreamManager> m_unpairManager;
};
