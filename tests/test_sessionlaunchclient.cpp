// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QApplication>
#include <atomic>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <csignal>
#include <thread>

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
