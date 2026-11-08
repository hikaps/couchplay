// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QObject>

class QSocketNotifier;

/**
 * Turns SIGTERM, SIGINT and SIGHUP into a Qt signal on the event loop.
 *
 * Without this the process dies on the default signal action and skips the session teardown, leaving
 * gamescope/Steam instances running. The handler only writes the signal number to a socketpair (the
 * async-signal-safe self-pipe pattern); everything else happens in the event loop.
 *
 * Only one instance may be alive at a time; the previous signal dispositions are restored on destruction.
 */
class SignalBridge : public QObject
{
    Q_OBJECT

public:
    explicit SignalBridge(QObject *parent = nullptr);
    ~SignalBridge() override;

    bool isActive() const
    {
        return m_active;
    }

Q_SIGNALS:
    void terminationRequested(int signalNumber);

private:
    void onSocketActivated();

    QSocketNotifier *m_notifier = nullptr;
    bool m_active = false;
};
