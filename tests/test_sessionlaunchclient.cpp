// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QApplication>
#include <atomic>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <csignal>
#include <thread>
#include <utility>

#include "CommandLineBridge.h"
#include "SessionLaunchClient.h"

class StopRaceLauncherAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.CouchPlay.SessionLauncher")

public:
    explicit StopRaceLauncherAdaptor(QObject *parent)
        : QDBusAbstractAdaptor(parent)
    {
    }

public Q_SLOTS:
    bool IsReady() const
    {
        return true;
    }

    bool LaunchProfile(const QString &, const QString &requestId, const QString &)
    {
        m_requestId = requestId;
        QTimer::singleShot(100, this, [] { std::raise(SIGTERM); });
        return true;
    }

    bool StopSession(const QString &requestId)
    {
        if (requestId != m_requestId) {
            return false;
        }
        QTimer::singleShot(75, this, [this, requestId] { Q_EMIT LaunchFinished(requestId, 23); });
        m_requestId.clear();
        return false;
    }

Q_SIGNALS:
    void LaunchFinished(const QString &requestId, int exitCode);

private:
    QString m_requestId;
};

class FailedLaunchAdaptor final : public QDBusAbstractAdaptor, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.CouchPlay.SessionLauncher")

public:
    explicit FailedLaunchAdaptor(QObject *parent)
        : QDBusAbstractAdaptor(parent)
    {
    }

public Q_SLOTS:
    bool IsReady() const
    {
        return true;
    }

    bool LaunchProfile(const QString &, const QString &, const QString &)
    {
        sendErrorReply(QDBusError::Failed, QStringLiteral("launch transport failed"));
        return false;
    }
    bool StopSession(const QString &)
    {
        return false;
    }
};

class MalformedReadinessAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.CouchPlay.SessionLauncher")

public:
    explicit MalformedReadinessAdaptor(QObject *parent)
        : QDBusAbstractAdaptor(parent)
    {
    }

    int launchCalls() const { return m_launchCalls; }

public Q_SLOTS:
    int IsReady() const { return 1; }
    bool LaunchProfile(const QString &, const QString &, const QString &)
    {
        ++m_launchCalls;
        return true;
    }
    bool StopSession(const QString &) { return false; }

private:
    int m_launchCalls = 0;
};
class ReplacingLauncherAdaptor final : public QDBusAbstractAdaptor, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.CouchPlay.SessionLauncher")

public:
    ReplacingLauncherAdaptor(QObject *parent,
                             QString serviceName,
                             QDBusConnection ownerBus,
                             QDBusConnection replacementBus)
        : QDBusAbstractAdaptor(parent)
        , m_serviceName(std::move(serviceName))
        , m_ownerBus(std::move(ownerBus))
        , m_replacementBus(std::move(replacementBus))
    {
    }

    int launchCalls() const
    {
        return m_launchCalls;
    }

    int stopCalls() const
    {
        return m_stopCalls;
    }

    bool replacementRegistered() const
    {
        return m_replacementRegistered;
    }

public Q_SLOTS:
    bool IsReady() const
    {
        return true;
    }

    bool LaunchProfile(const QString &, const QString &requestId, const QString &)
    {
        ++m_launchCalls;
        m_requestId = requestId;
        QTimer::singleShot(500, this, [this, requestId] { Q_EMIT LaunchFinished(requestId, 93); });
        const QDBusMessage reply = message().createReply(true);
        const QDBusConnection replyBus = connection();
        setDelayedReply(true);
        QTimer::singleShot(100, this, [this, replyBus, reply] {
            m_ownerBus.unregisterService(m_serviceName);
            m_replacementRegistered = m_replacementBus.registerService(m_serviceName);
            if (!m_replacementRegistered) {
                Q_EMIT LaunchFinished(m_requestId, 91);
            }
            replyBus.send(reply);
        });
        return false;
    }

    bool StopSession(const QString &requestId)
    {
        if (requestId != m_requestId) {
            return false;
        }
        ++m_stopCalls;
        Q_EMIT LaunchFinished(requestId, 41);
        return true;
    }

Q_SIGNALS:
    void LaunchFinished(const QString &requestId, int exitCode);

private:
    QString m_serviceName;
    QDBusConnection m_ownerBus;
    QDBusConnection m_replacementBus;
    QString m_requestId;
    int m_launchCalls = 0;
    int m_stopCalls = 0;
    bool m_replacementRegistered = false;
};

class ReplacementLauncherAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.CouchPlay.SessionLauncher")

public:
    explicit ReplacementLauncherAdaptor(QObject *parent)
        : QDBusAbstractAdaptor(parent)
    {
    }

    int launchCalls() const
    {
        return m_launchCalls;
    }

public Q_SLOTS:
    bool IsReady() const
    {
        return true;
    }

    bool LaunchProfile(const QString &, const QString &requestId, const QString &)
    {
        ++m_launchCalls;
        Q_EMIT LaunchFinished(requestId, 89);
        return true;
    }

    bool StopSession(const QString &)
    {
        return false;
    }

Q_SIGNALS:
    void LaunchFinished(const QString &requestId, int exitCode);

private:
    int m_launchCalls = 0;
};

namespace {

volatile std::sig_atomic_t restoredTerminationSignalCount = 0;

void restoredTerminationSignalHandler(int)
{
    ++restoredTerminationSignalCount;
}

class DelayedFinishLauncherAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.CouchPlay.SessionLauncher")

public:
    explicit DelayedFinishLauncherAdaptor(QObject *parent)
        : QDBusAbstractAdaptor(parent)
    {
    }

    int stopCalls() const
    {
        return m_stopCalls;
    }

public Q_SLOTS:
    bool IsReady() const
    {
        return true;
    }

    bool LaunchProfile(const QString &, const QString &requestId, const QString &)
    {
        QTimer::singleShot(100, this, [this, requestId] { Q_EMIT LaunchFinished(requestId, 31); });
        return true;
    }

    bool StopSession(const QString &requestId)
    {
        ++m_stopCalls;
        Q_EMIT LaunchFinished(requestId, 99);
        return true;
    }

Q_SIGNALS:
    void LaunchFinished(const QString &requestId, int exitCode);

private:
    int m_stopCalls = 0;
};

} // namespace

class TestSessionLaunchClient : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRejectsNonWaitingRequest()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        CommandLineRequest request;
        request.profileName = QStringLiteral("Profile");
        request.start = true;
        QVERIFY(!request.exitAfterSession);
        QCOMPARE(SessionLaunchClient::run(*application, request, QStringLiteral("com.github.CouchPlay")), 2);
    }
    void testBridgeContextAndSynchronousCompletion()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());

        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        const QString interfaceName = QStringLiteral("com.github.CouchPlay.SessionLauncher");
        CommandLineBridge bridge;
        QVERIFY(bus.registerObject(objectPath, &bridge, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));

        auto call = [&](const QString &member, const QVariantList &arguments = {}) {
            QDBusMessage message = QDBusMessage::createMethodCall(service, objectPath, interfaceName, member);
            message.setArguments(arguments);
            QDBusPendingCallWatcher watcher(bus.asyncCall(message));
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            if (!watcher.isFinished()) {
                timeout.start(3000);
                loop.exec();
            }
            timeout.stop();
            return watcher.isFinished() ? watcher.reply() : QDBusMessage();
        };

        QDBusMessage notReady = call(QStringLiteral("IsReady"));
        QVERIFY(notReady.type() == QDBusMessage::ReplyMessage);
        QVERIFY(!notReady.arguments().value(0).toBool());
        bridge.setReady(true);
        QDBusMessage ready = call(QStringLiteral("IsReady"));
        QVERIFY(ready.type() == QDBusMessage::ReplyMessage);
        QVERIFY(ready.arguments().value(0).toBool());

        QSignalSpy finished(&bridge, &CommandLineBridge::launchFinished);
        QObject::connect(&bridge, &CommandLineBridge::launchRequested, &bridge,
                         [&bridge](const QString &, bool, bool) {
            bridge.setRequestAccepted(true);
            bridge.finishRequest(17);
        });
        QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
        if (display.isEmpty()) {
            display = qEnvironmentVariable("DISPLAY");
        }
        const QDBusMessage launch = call(QStringLiteral("LaunchProfile"),
                                         {QStringLiteral("Family"), QStringLiteral("request-1"), display});
        QVERIFY(launch.type() == QDBusMessage::ReplyMessage);
        QVERIFY(launch.arguments().value(0).toBool());
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.constFirst().at(0).toString(), QStringLiteral("request-1"));
        QCOMPARE(finished.constFirst().at(1).toInt(), 17);

        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }
    void testRunWaitsForReadinessAndSynchronousCompletion()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchRunTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        CommandLineBridge bridge;
        QVERIFY(bus.registerObject(objectPath, &bridge, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));

        QSignalSpy launched(&bridge, &CommandLineBridge::launchRequested);
        QObject::connect(&bridge, &CommandLineBridge::launchRequested, &bridge,
                         [&bridge](const QString &, bool, bool) {
            bridge.setRequestAccepted(true);
            bridge.finishRequest(17);
        });
        QTimer::singleShot(100, &bridge, [&bridge] { bridge.setReady(true); });

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 7000);
        client.join();

        QCOMPARE(exitCode.load(), 17);
        QCOMPARE(launched.size(), 1);
        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }

    void testRunTimesOutWhenServiceNeverBecomesReady()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchNotReadyTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        CommandLineBridge bridge;
        QVERIFY(bus.registerObject(objectPath, &bridge, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));

        QSignalSpy launched(&bridge, &CommandLineBridge::launchRequested);
        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 7000);
        client.join();

        QCOMPARE(exitCode.load(), 1);
        QCOMPARE(launched.size(), 0);
        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }

    void testRunFailsWhenServiceDisappearsDuringLaunch()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchGoneTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        CommandLineBridge bridge;
        QVERIFY(bus.registerObject(objectPath, &bridge, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));
        bridge.setReady(true);

        QSignalSpy launched(&bridge, &CommandLineBridge::launchRequested);
        QObject::connect(&bridge, &CommandLineBridge::launchRequested, &bridge,
                         [&bridge, &bus, &service](const QString &, bool, bool) {
            bridge.setRequestAccepted(true);
            QTimer::singleShot(0, &bridge, [&bridge, &bus, &service] {
                QTimer::singleShot(250, &bridge, [&bus, &service] { bus.unregisterService(service); });
            });
        });

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 5000);
        client.join();

        QCOMPARE(exitCode.load(), 1);
        QCOMPARE(launched.size(), 1);
        bus.unregisterObject(objectPath);
    }
    void testRunClassifiesLaunchRejectionSeparatelyFromTransportError()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchRejectTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        CommandLineBridge bridge;
        QVERIFY(bus.registerObject(objectPath, &bridge, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));
        bridge.setReady(true);

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 5000);
        client.join();

        QCOMPARE(exitCode.load(), 2);
        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }

    void testRunClassifiesLaunchTransportErrorAsOperationalFailure()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchTransportErrorTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        QObject serviceObject;
        new FailedLaunchAdaptor(&serviceObject);
        QVERIFY(bus.registerObject(objectPath, &serviceObject, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 5000);
        client.join();

        QCOMPARE(exitCode.load(), 1);
        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }

    void testRunRejectsNonBooleanReadinessReply()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchMalformedReadyTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        QObject serviceObject;
        auto *adaptor = new MalformedReadinessAdaptor(&serviceObject);
        QVERIFY(bus.registerObject(objectPath, &serviceObject, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 8000);
        client.join();

        QCOMPARE(exitCode.load(), 1);
        QCOMPARE(adaptor->launchCalls(), 0);
        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }

    void testRunStopsAcceptedLaunchOnOwnerReplacement()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchOwnerReplacementTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        const QString ownerConnectionName = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString replacementConnectionName = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDBusConnection ownerBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, ownerConnectionName);
        QDBusConnection replacementBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, replacementConnectionName);
        QVERIFY(ownerBus.isConnected());
        QVERIFY(replacementBus.isConnected());
        QObject ownerObject;
        ReplacingLauncherAdaptor owner(&ownerObject, service, ownerBus, replacementBus);
        QObject replacementObject;
        ReplacementLauncherAdaptor replacement(&replacementObject);
        QVERIFY(ownerBus.registerObject(objectPath, &ownerObject, QDBusConnection::ExportAdaptors));
        QVERIFY(replacementBus.registerObject(objectPath, &replacementObject, QDBusConnection::ExportAdaptors));
        QVERIFY(ownerBus.registerService(service));

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 5000);
        client.join();

        QCOMPARE(exitCode.load(), 1);
        QCOMPARE(owner.launchCalls(), 1);
        QCOMPARE(owner.stopCalls(), 1);
        QCOMPARE(owner.replacementRegistered(), true);
        QCOMPARE(replacement.launchCalls(), 0);
        if (owner.replacementRegistered()) {
            replacementBus.unregisterService(service);
        }
        ownerBus.unregisterObject(objectPath);
        replacementBus.unregisterObject(objectPath);
        QDBusConnection::disconnectFromBus(ownerConnectionName);
        QDBusConnection::disconnectFromBus(replacementConnectionName);
    }
    void testRunRestoresTerminationNotifierBetweenRuns()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchNotifierLifecycleTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        QObject serviceObject;
        auto *adaptor = new DelayedFinishLauncherAdaptor(&serviceObject);
        QVERIFY(bus.registerObject(objectPath, &serviceObject, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        restoredTerminationSignalCount = 0;
        const auto previousHandler = std::signal(SIGTERM, &restoredTerminationSignalHandler);

        std::atomic<int> firstExitCode{-1};
        std::jthread firstClient([&] {
            firstExitCode.store(SessionLaunchClient::run(*application, request, service));
        });
        QTRY_VERIFY_WITH_TIMEOUT(firstExitCode.load() != -1, 5000);
        firstClient.join();
        QCOMPARE(firstExitCode.load(), 31);

        std::raise(SIGTERM);
        QCOMPARE(static_cast<int>(restoredTerminationSignalCount), 1);

        std::atomic<int> secondExitCode{-1};
        std::jthread secondClient([&] {
            secondExitCode.store(SessionLaunchClient::run(*application, request, service));
        });
        QTRY_VERIFY_WITH_TIMEOUT(secondExitCode.load() != -1, 5000);
        secondClient.join();
        QCOMPARE(secondExitCode.load(), 31);
        QCOMPARE(adaptor->stopCalls(), 0);

        std::signal(SIGTERM, previousHandler);
        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }
    void testRunPreservesExitStatusAfterRejectedStop()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("com.github.CouchPlay.SessionLaunchStopRaceTest");
        const QString objectPath = QStringLiteral("/SessionLauncher");
        QObject serviceObject;
        new StopRaceLauncherAdaptor(&serviceObject);
        QVERIFY(bus.registerObject(objectPath, &serviceObject, QDBusConnection::ExportAdaptors));
        QVERIFY(bus.registerService(service));

        CommandLineRequest request;
        request.profileName = QStringLiteral("Family");
        request.start = true;
        request.exitAfterSession = true;
        std::atomic<int> exitCode{-1};
        std::jthread client([&] { exitCode.store(SessionLaunchClient::run(*application, request, service)); });
        QTRY_VERIFY_WITH_TIMEOUT(exitCode.load() != -1, 5000);
        client.join();

        QCOMPARE(exitCode.load(), 23);
        bus.unregisterObject(objectPath);
        bus.unregisterService(service);
    }
};

QTEST_MAIN(TestSessionLaunchClient)
#include "test_sessionlaunchclient.moc"
