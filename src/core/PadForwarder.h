// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

/**
 * @brief One host-side `couchplay-padfwd` process: a virtual Xbox 360 pad for one player
 *
 * The tool creates the uinput device and forwards the player's physical controllers into it, so
 * the sandbox only ever sees that one node. It reports `ready <node>`, `lost <path>`, `chord` and
 * `error ...` on stdout and takes `attach <path>` / `detach <path>` on stdin.
 */
class PadForwarder : public QObject
{
    Q_OBJECT

public:
    explicit PadForwarder(QObject *parent = nullptr);
    ~PadForwarder() override;

    /** Start the tool and wait (blocking) for `ready`. On failure emits errorOccurred and returns false. */
    bool start(int playerIndex, int timeoutMs = 5000);

    /** Host path of the virtual pad's event node ("" before start). */
    QString node() const
    {
        return m_node;
    }

    bool isRunning() const;

    void attach(const QString &devicePath);
    void detach(const QString &devicePath);

    /** Close stdin (the tool destroys the pad), then kill if it lingers. */
    void stop();

    /** Use @p program instead of the installed couchplay-padfwd (tests). */
    static void setProgramOverrideForTesting(const QString &program);

Q_SIGNALS:
    void chordTriggered();
    void sourceLost(const QString &devicePath);
    void errorOccurred(const QString &message);

private:
    void handleLine(const QString &line);
    void drainOutput();
    void sendLine(const QString &line);

    QProcess m_process;
    QString m_node;
    QString m_pending;
    QString m_startError;
    bool m_stopping = false;
};
