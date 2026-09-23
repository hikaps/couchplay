// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "CommandLineBridge.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QRegularExpression>

namespace {

class SessionLauncherAdaptor final : public QDBusAbstractAdaptor
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
    bool IsReady() const
    {
        return m_bridge->isReady();
    }
    bool LaunchProfile(const QString &profileName, const QString &requestId, const QString &display)
    {
        return m_bridge->launchProfile(profileName, requestId, display, m_bridge->currentDbusSender());
    }

    bool StopSession(const QString &requestId)
    {
        return m_bridge->stopSession(requestId, m_bridge->currentDbusSender());
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
QString CommandLineBridge::currentDbusSender() const
{
    return calledFromDBus() ? message().service() : QString();
}

bool CommandLineBridge::launchProfile(const QString &profileName,
                                      const QString &requestId,
                                      const QString &display,
                                      const QString &sender)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    auto *busInterface = bus.interface();
    if (profileName.isEmpty() || requestId.isEmpty() || sender.isEmpty() || !displayMatches(display)
        || !m_ready || !m_activeRequestId.isEmpty() || !busInterface || !busInterface->isServiceRegistered(sender)) {
        return false;
    }

    m_activeRequestId = requestId;
    m_activeSender = sender;
    m_activeDisplay = display;
    m_requestAccepted = false;
    m_senderGone = false;

    auto *watcher = new QDBusServiceWatcher(sender, bus, QDBusServiceWatcher::WatchForUnregistration, this);
    m_senderWatcher = watcher;
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this, watcher](const QString &service) {
        if (service == m_activeSender && !m_activeRequestId.isEmpty() && !m_senderGone) {
            m_senderGone = true;
            Q_EMIT stopRequested();
        }
        if (m_senderWatcher == watcher) {
            m_senderWatcher = nullptr;
        }
        watcher->deleteLater();
    });

    auto clearPendingRequest = [this, watcher] {
        if (m_senderWatcher == watcher) {
            m_senderWatcher = nullptr;
        }
        watcher->deleteLater();
        m_activeRequestId.clear();
        m_activeSender.clear();
        m_activeDisplay.clear();
        m_senderGone = false;
    };
    if (!busInterface->isServiceRegistered(sender)) {
        clearPendingRequest();
        return false;
    }

    Q_EMIT launchRequested(profileName, true, true);
    if (!m_requestAccepted) {
        clearPendingRequest();
        return false;
    }
    if (!busInterface->isServiceRegistered(sender) && !m_senderGone) {
        m_senderGone = true;
        Q_EMIT stopRequested();
    }
    return !m_senderGone;
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
    m_senderGone = false;
    if (m_senderWatcher) {
        m_senderWatcher->deleteLater();
        m_senderWatcher = nullptr;
    }
    Q_EMIT launchFinished(requestId, exitCode);
}

#include "CommandLineBridge.moc"
