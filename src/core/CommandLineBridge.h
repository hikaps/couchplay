// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QDBusContext>
#include <QString>

class QDBusServiceWatcher;
class CommandLineBridge : public QObject, protected QDBusContext
{
    Q_OBJECT

public:
    explicit CommandLineBridge(QObject *parent = nullptr);
    QString currentDbusSender() const;
    bool isReady() const { return m_ready; }
    void setReady(bool ready) { m_ready = ready; }

    bool requestAccepted() const
    {
        return m_requestAccepted;
    }

    Q_INVOKABLE void setRequestAccepted(bool accepted)
    {
        m_requestAccepted = accepted;
    }

    bool launchProfile(const QString &profileName,
                       const QString &requestId,
                       const QString &display,
                       const QString &sender);
    bool stopSession(const QString &requestId, const QString &sender);

    Q_INVOKABLE void requestStop();
public Q_SLOTS:
    void finishRequest(int exitCode);

Q_SIGNALS:
    void launchRequested(const QString &profileName, bool start, bool exitAfterSession);
    void stopRequested();
    void launchFinished(const QString &requestId, int exitCode);
private:
    bool m_ready = false;
    bool m_requestAccepted = false;
    QString m_activeRequestId;
    QString m_activeSender;
    QString m_activeDisplay;
    bool m_senderGone = false;
    QDBusServiceWatcher *m_senderWatcher = nullptr;
};
