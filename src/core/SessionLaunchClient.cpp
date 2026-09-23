// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SessionLaunchClient.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QDBusReply>
#include <QEventLoop>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace {

class DbusSignalReceiver final : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    void onLaunchFinished(const QString &requestId, int exitCode)
    {
        Q_EMIT launchFinished(requestId, exitCode);
    }

Q_SIGNALS:
    void launchFinished(const QString &requestId, int exitCode);
};

class TerminationNotifier final : public QObject
{
    Q_OBJECT

public:
    TerminationNotifier(QObject *parent, std::function<void()> callback)
        : QObject(parent)
        , m_callback(std::move(callback))
    {
        if (s_readFd < 0) {
            int pipeFds[2];
            if (::pipe(pipeFds) != 0) {
                return;
            }
            s_readFd = pipeFds[0];
            s_writeFd = pipeFds[1];
            const int readFlags = ::fcntl(s_readFd, F_GETFL, 0);
            ::fcntl(s_readFd, F_SETFL, readFlags | O_NONBLOCK);
            const int writeFlags = ::fcntl(s_writeFd, F_GETFL, 0);
            ::fcntl(s_writeFd, F_SETFL, writeFlags | O_NONBLOCK);
            std::signal(SIGTERM, &TerminationNotifier::signalHandler);
            std::signal(SIGINT, &TerminationNotifier::signalHandler);
        }
        if (s_readFd >= 0) {
            m_notifier = new QSocketNotifier(s_readFd, QSocketNotifier::Read, this);
            connect(m_notifier, &QSocketNotifier::activated, this, [this] {
                char signalByte;
                while (::read(s_readFd, &signalByte, sizeof(signalByte)) > 0) {
                }
                if (m_callback) {
                    m_callback();
                }
            });
        }
    }

    ~TerminationNotifier() override
    {
        if (m_notifier) {
            m_notifier->setEnabled(false);
        }
    }

private:
    static void signalHandler(int signal)
    {
        if (s_writeFd >= 0) {
            const char signalByte = static_cast<char>(signal);
            const int savedErrno = errno;
            (void)::write(s_writeFd, &signalByte, sizeof(signalByte));
            errno = savedErrno;
        }
    }

    static inline int s_readFd = -1;
    static inline int s_writeFd = -1;
    QSocketNotifier *m_notifier = nullptr;
    std::function<void()> m_callback;
};

} // namespace

int SessionLaunchClient::run(QApplication &application, const CommandLineRequest &request, const QString &serviceName)
{
    Q_UNUSED(application)
    if (!request.requested() || !request.start || !request.exitAfterSession || serviceName.isEmpty()) {
        return 2;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return 1;
    }

    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (display.isEmpty()) {
        display = qEnvironmentVariable("DISPLAY");
    }

    QDBusConnectionInterface *busInterface = bus.interface();
    if (!busInterface) {
        return 1;
    }
    bool launcherReady = false;
    for (int attempt = 0; attempt < 100 && !launcherReady; ++attempt) {
        if (busInterface->isServiceRegistered(serviceName)) {
            const QDBusMessage readyCall = QDBusMessage::createMethodCall(
                serviceName,
                QStringLiteral("/SessionLauncher"),
                QStringLiteral("com.github.CouchPlay.SessionLauncher"),
                QStringLiteral("IsReady"));
            const QDBusMessage readyReply = bus.call(readyCall, QDBus::Block, 250);
            launcherReady = readyReply.type() == QDBusMessage::ReplyMessage
                && readyReply.arguments().value(0).toBool();
        }
        if (!launcherReady) {
            QThread::msleep(50);
        }
    }
    if (!launcherReady) {
        return 1;
    }

    QDBusInterface interface(serviceName,
                             QStringLiteral("/SessionLauncher"),
                             QStringLiteral("com.github.CouchPlay.SessionLauncher"),
                             bus);
    if (!interface.isValid()) {
        return 1;
    }

    QEventLoop loop;
    int result = 1;
    bool completed = false;
    bool serviceLost = false;
    bool stopSent = false;
    DbusSignalReceiver receiver;
    QTimer stopRejectedTimeout;
    stopRejectedTimeout.setSingleShot(true);
    QObject::connect(&stopRejectedTimeout, &QTimer::timeout, &loop, [&] {
        if (!completed && !serviceLost) {
            result = 1;
            loop.quit();
        }
    });
    QDBusServiceWatcher watcher(serviceName, bus, QDBusServiceWatcher::WatchForUnregistration);

    QObject::connect(&receiver, &DbusSignalReceiver::launchFinished, &loop,
                     [&](const QString &finishedRequestId, int exitCode) {
        if (finishedRequestId != requestId) {
            return;
        }
        completed = true;
        result = exitCode;
        loop.quit();
    });
    QObject::connect(&watcher, &QDBusServiceWatcher::serviceUnregistered, &loop, [&](const QString &service) {
        if (service == serviceName && !completed) {
            serviceLost = true;
            result = 1;
            loop.quit();
        }
    });
    if (!bus.connect(serviceName,
                     QStringLiteral("/SessionLauncher"),
                     QStringLiteral("com.github.CouchPlay.SessionLauncher"),
                     QStringLiteral("LaunchFinished"),
                     &receiver,
                     SLOT(onLaunchFinished(QString, int)))) {
        return 1;
    }

    auto *termination = static_cast<TerminationNotifier *>(SessionLaunchClient::watchTermination(&loop, [&] {
        if (stopSent || completed) {
            return;
        }
        stopSent = true;
        const QDBusMessage stopReply = interface.call(QStringLiteral("StopSession"), requestId);
        if (stopReply.type() == QDBusMessage::ErrorMessage || !stopReply.arguments().value(0).toBool()) {
            // The owner may have completed the request before processing StopSession.
            // Let an already-queued LaunchFinished/unregistration determine the result.
            stopRejectedTimeout.start(500);
        }
    }));
    Q_UNUSED(termination)

    const QDBusMessage launchReply = interface.call(QStringLiteral("LaunchProfile"), request.profileName, requestId, display);
    if (launchReply.type() == QDBusMessage::ErrorMessage || launchReply.arguments().value(0).toBool() == false) {
        bus.disconnect(serviceName,
                       QStringLiteral("/SessionLauncher"),
                       QStringLiteral("com.github.CouchPlay.SessionLauncher"),
                       QStringLiteral("LaunchFinished"),
                       &receiver,
                       SLOT(onLaunchFinished(QString, int)));
        return 2;
    }
    if (!completed && !serviceLost) {
        loop.exec();
    }
    stopRejectedTimeout.stop();
    bus.disconnect(serviceName,
                   QStringLiteral("/SessionLauncher"),
                   QStringLiteral("com.github.CouchPlay.SessionLauncher"),
                   QStringLiteral("LaunchFinished"),
                   &receiver,
                   SLOT(onLaunchFinished(QString, int)));
    return result;
}

QObject *SessionLaunchClient::watchTermination(QObject *parent, std::function<void()> onTermination)
{
    return new TerminationNotifier(parent, std::move(onTermination));
}

#include "SessionLaunchClient.moc"
