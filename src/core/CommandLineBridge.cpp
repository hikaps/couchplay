// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "CommandLineBridge.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QRegularExpression>

namespace {

class SessionLauncherAdaptor final : public QDBusAbstractAdaptor, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.CouchPlay.SessionLauncher")

public:
    explicit SessionLauncherAdaptor(CommandLineBridge *bridge)
        : QDBusAbstractAdaptor(bridge)
        , m_bridge(bridge)
    {
        connect(m_bridge, &CommandLineBridge::launchFinished, this, &SessionLauncherAdaptor::LaunchFinished);
    }

public Q_SLOTS:
    bool LaunchProfile(const QString &profileName, const QString &requestId, const QString &display)
    {
        return m_bridge->launchProfile(profileName, requestId, display, message().service());
    }

    bool StopSession(const QString &requestId)
    {
        return m_bridge->stopSession(requestId, message().service());
    }

Q_SIGNALS:
    void LaunchFinished(const QString &requestId, int exitCode);

private:
    CommandLineBridge *m_bridge;
};

bool displayMatches(const QString &display)
{
    QString current = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (current.isEmpty()) {
        current = qEnvironmentVariable("DISPLAY");
    }
    return current == display;
}

} // namespace

CommandLineBridge::CommandLineBridge(QObject *parent)
    : QObject(parent)
{
    new SessionLauncherAdaptor(this);
}

bool CommandLineBridge::launchProfile(const QString &profileName,
                                      const QString &requestId,
                                      const QString &display,
                                      const QString &sender)
{
    if (profileName.isEmpty() || requestId.isEmpty() || sender.isEmpty() || !displayMatches(display)
        || !m_activeRequestId.isEmpty()) {
        return false;
    }

    m_activeRequestId = requestId;
    m_activeSender = sender;
    m_activeDisplay = display;
    m_requestAccepted = false;
    Q_EMIT launchRequested(profileName, true, true);
    if (!m_requestAccepted) {
        m_activeRequestId.clear();
        m_activeSender.clear();
        m_activeDisplay.clear();
        return false;
    }

    auto *watcher = new QDBusServiceWatcher(sender,
                                             QDBusConnection::sessionBus(),
                                             QDBusServiceWatcher::WatchForUnregistration,
                                             this);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this, watcher](const QString &service) {
        if (service == m_activeSender && !m_activeRequestId.isEmpty()) {
            Q_EMIT stopRequested();
        }
        watcher->deleteLater();
    });
    return true;
}

bool CommandLineBridge::stopSession(const QString &requestId, const QString &sender)
{
    if (requestId.isEmpty() || requestId != m_activeRequestId || sender != m_activeSender) {
        return false;
    }
    Q_EMIT stopRequested();
    return true;
}
void CommandLineBridge::requestStop()
{
    Q_EMIT stopRequested();
}

void CommandLineBridge::finishRequest(int exitCode)
{
    if (m_activeRequestId.isEmpty()) {
        return;
    }
    const QString requestId = m_activeRequestId;
    m_activeRequestId.clear();
    m_activeSender.clear();
    m_activeDisplay.clear();
    Q_EMIT launchFinished(requestId, exitCode);
}

#include "CommandLineBridge.moc"
