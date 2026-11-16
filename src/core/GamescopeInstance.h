// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include "SandboxCommand.h"

#include <QList>
#include <QObject>
#include <QProcess>
#include <qqmlintegration.h>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <memory>

/**
 * @brief Manages a single gamescope instance running inside a rootless bwrap sandbox
 *
 * Launches `bwrap → dbus-run-session → gamescope` as the desktop user (through
 * HostProcess, so it also works from Flatpak) and finds gamescope's host PID by the
 * COUCHPLAY_SANDBOX_ID token in its environment.
 */
class GamescopeInstance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int index READ index CONSTANT)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(qint64 pid READ pid NOTIFY runningChanged)
    Q_PROPERTY(qint64 gamescopePid READ gamescopePid NOTIFY gamescopePidChanged)
    Q_PROPERTY(QString player READ player NOTIFY configChanged)
    Q_PROPERTY(QRect windowGeometry READ windowGeometry NOTIFY configChanged)

public:
    explicit GamescopeInstance(QObject *parent = nullptr);
    ~GamescopeInstance() override;

    virtual bool start(const QVariantMap &config, int index, const QList<SandboxMount> &mounts);

    virtual void stop(int timeoutMs = 5000);
    virtual void kill();

    bool isRunning() const;

    int index() const
    {
        return m_index;
    }
    /** PID of the launcher process (bwrap, or flatpak-spawn); 0 when not launched. */
    qint64 pid() const
    {
        return m_launched ? m_process.processId() : 0;
    }
    qint64 gamescopePid() const
    {
        return m_gamescopePid;
    }
    QString status() const
    {
        return m_status;
    }
    QString player() const
    {
        return m_player;
    }
    QRect windowGeometry() const
    {
        return m_windowGeometry;
    }

    static QStringList buildGamescopeArgs(const QVariantMap &config);

    /** "KEY=VALUE" entries for the sandbox environment. */
    static QStringList buildEnvironment(const QVariantMap &config);

Q_SIGNALS:
    void runningChanged();
    void statusChanged();
    void configChanged();
    void gamescopePidChanged();
    void started();
    void stopped();
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void pollForGamescope();

private:
    void setStatus(const QString &status);
    void signalHost(const QString &signal);
    void finishStop(const QString &status);
    void resetLaunchState();

    int m_index = -1;
    QString m_status;
    QString m_player;
    QRect m_windowGeometry;
    QProcess m_process;
    std::unique_ptr<QProcess> m_scan;
    QTimer m_pollTimer;
    QString m_token;
    QByteArray m_stderrTail;
    qint64 m_gamescopePid = 0;
    qint64 m_startDeadlineMs = 0;
    bool m_launched = false;
    bool m_stopping = false;
};
