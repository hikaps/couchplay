// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <qqmlintegration.h>

#include "SessionManager.h"
#include "SessionRunner.h"
#include "SettingsManager.h"
#include "StreamManager.h"
#include "VirtualDisplayManager.h"

/**
 * Temporary Sunshine for pairing a Moonlight client outside a session.
 *
 * Sunshine refuses to start without a capture output, so a virtual display is created first.
 * The pairing session owns its own VirtualDisplayManager and StreamManager, separate from
 * SessionRunner's. VirtualDisplayManager names displays couchplay-p<index+1>, the same as a
 * session does, so a pairing session and a running session must never coexist: start() is
 * refused while the session is active, and a session that becomes active stops the pairing.
 */
class PairingSession : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int instanceIndex READ instanceIndex NOTIFY instanceIndexChanged)
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
    int instanceIndex() const { return m_instanceIndex; }
    QString username() const { return m_username; }
    bool busy() const { return m_state == Starting || m_state == Ready || m_stopping; }
    StreamManager *streamManager() const { return m_streamManager; }

    SessionManager *sessionManager() const { return m_sessionManager; }
    void setSessionManager(SessionManager *manager);
    SessionRunner *sessionRunner() const { return m_sessionRunner; }
    void setSessionRunner(SessionRunner *runner);
    SettingsManager *settingsManager() const { return m_settingsManager; }
    void setSettingsManager(SettingsManager *manager);

    Q_INVOKABLE void start(int instanceIndex, const QString &username);
    Q_INVOKABLE void stop();
    Q_INVOKABLE int pairedClientCount(const QString &username) const;

    // Test seams
    void setVirtualDisplayProgramOverride(const QString &program);
    void setScreenLookup(VirtualDisplayManager::ScreenLookup lookup);
    void setVirtualDisplayTimeoutMs(int timeoutMs);
    void setSunshineProgramOverride(const QString &program);
    VirtualDisplayManager *virtualDisplays() const { return m_virtualDisplays; }

Q_SIGNALS:
    void stateChanged();
    void errorChanged();
    void instanceIndexChanged();
    void usernameChanged();
    void busyChanged();
    void sessionManagerChanged();
    void sessionRunnerChanged();
    void settingsManagerChanged();
    // Paired clients of this gaming user may have changed
    void pairedChanged(const QString &username);

private:
    void setState(State state);
    void setError(const QString &error);
    void setInstanceIndex(int index);
    void setUsername(const QString &username);
    void updateBusy();
    void fail(const QString &message);
    void teardown();
    void onDisplayReady(int index, const QString &outputName);
    void onDisplayFailed(int index, const QString &message);
    void onStreamStarted(int index);
    void onStreamError(int index, const QString &message);
    void onStreamStopped(int index);
    bool sessionActive() const;

    State m_state = Idle;
    QString m_error;
    int m_instanceIndex = -1;
    QString m_username;
    bool m_stopping = false;
    bool m_lastBusy = false;
    StreamManager *m_streamManager = nullptr;
    VirtualDisplayManager *m_virtualDisplays = nullptr;
    QPointer<SessionManager> m_sessionManager;
    QPointer<SessionRunner> m_sessionRunner;
    QPointer<SettingsManager> m_settingsManager;
};
