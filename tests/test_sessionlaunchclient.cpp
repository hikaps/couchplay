// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "CommandLineBridge.h"
#include "SessionLaunchClient.h"
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
};

QTEST_MAIN(TestSessionLaunchClient)
#include "test_sessionlaunchclient.moc"
