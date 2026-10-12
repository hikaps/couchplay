// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDateTime>
#include <QSignalSpy>
#include <QTest>

#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#define private public
#include "WindowManager.h"
#undef private

struct WindowMatch {
    QString id;
    QString caption;
    QString icon;
    qint32 relevance = 0;
    double score = 0;
    QVariantMap properties;
};
Q_DECLARE_METATYPE(WindowMatch)
Q_DECLARE_METATYPE(QList<WindowMatch>)

QDBusArgument &operator<<(QDBusArgument &arg, const WindowMatch &match)
{
    arg.beginStructure();
    arg << match.id << match.caption << match.icon << match.relevance << match.score << match.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, WindowMatch &match)
{
    arg.beginStructure();
    arg >> match.id >> match.caption >> match.icon >> match.relevance >> match.score >> match.properties;
    arg.endStructure();
    return arg;
}

class MockWindowsRunner : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.krunner1")
public Q_SLOTS:
    QList<WindowMatch> Match(const QString &) const
    {
        return {{QStringLiteral("0_{launcher}"), QStringLiteral("OnlineFix Linux Launcher"), {}, 0, 0,
                 {{QStringLiteral("description"), QStringLiteral("Window")}}}};
    }
};

class MockKWin : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin")

public:
    QVariantMap windowInfo;

public Q_SLOTS:
    QVariantMap getWindowInfo(const QString &windowId) const
    {
        Q_UNUSED(windowId);
        return windowInfo;
    }
};

class MockKWinScripting : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")

public Q_SLOTS:
    int loadScript(const QString &filePath, const QString &pluginName)
    {
        Q_UNUSED(filePath);
        Q_UNUSED(pluginName);
        return 1;
    }

    void unloadScript(const QString &pluginName)
    {
        Q_UNUSED(pluginName);
    }
};

class MockKWinScript : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Script")

public Q_SLOTS:
    void run() {}
};

class TestWindowManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void directPositionSupersedesOlderConfirmation();
    void identifiesUnlabelledGamescopeWindowByPid();

private:
    static constexpr auto serviceName = "org.kde.KWin";
    MockKWin m_kwin;
    MockKWinScripting m_scripting;
    MockKWinScript m_script;
    MockWindowsRunner m_runner;
};

void TestWindowManager::initTestCase()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.registerService(QString::fromLatin1(serviceName)));
    qDBusRegisterMetaType<WindowMatch>();
    qDBusRegisterMetaType<QList<WindowMatch>>();
    QVERIFY(bus.registerObject(QStringLiteral("/WindowsRunner"), &m_runner, QDBusConnection::ExportAllSlots));
    QVERIFY(bus.registerObject(QStringLiteral("/KWin"), &m_kwin, QDBusConnection::ExportAllSlots));
    QVERIFY(bus.registerObject(QStringLiteral("/Scripting"), &m_scripting, QDBusConnection::ExportAllSlots));
    QVERIFY(bus.registerObject(QStringLiteral("/Scripting/Script1"), &m_script, QDBusConnection::ExportAllSlots));
}

void TestWindowManager::cleanupTestCase()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterObject(QStringLiteral("/KWin"));
    bus.unregisterObject(QStringLiteral("/WindowsRunner"));
    bus.unregisterObject(QStringLiteral("/Scripting"));
    bus.unregisterObject(QStringLiteral("/Scripting/Script1"));
    bus.unregisterService(QString::fromLatin1(serviceName));
}

void TestWindowManager::directPositionSupersedesOlderConfirmation()
{
    WindowManager manager;
    QVERIFY(manager.isAvailable());
    manager.m_positionTimer->setInterval(60000);

    const QString windowId = QStringLiteral("gamescope-window");
    const QRect olderGeometry(0, 0, 800, 600);
    const QRect latestGeometry(800, 0, 800, 600);
    QVERIFY(manager.positionWindow(windowId, olderGeometry));
    QVERIFY(manager.positionWindow(windowId, latestGeometry));

    m_kwin.windowInfo = {
        {QStringLiteral("x"), latestGeometry.x()},
        {QStringLiteral("y"), latestGeometry.y()},
        {QStringLiteral("width"), latestGeometry.width()},
        {QStringLiteral("height"), latestGeometry.height()},
    };
    QSignalSpy positioned(&manager, &WindowManager::windowPositioned);
    QSignalSpy failed(&manager, &WindowManager::positioningFailed);
    QVERIFY(positioned.isValid());
    QVERIFY(failed.isValid());

    QVERIFY(QMetaObject::invokeMethod(&manager, "checkPositionConfirmations", Qt::DirectConnection));
    QCOMPARE(positioned.count(), 1);
    QCOMPARE(positioned.constFirst().at(0).toString(), windowId);
    QCOMPARE(qvariant_cast<QRect>(positioned.constFirst().at(1)), latestGeometry);

    for (auto &pending : manager.m_pendingPositions) {
        pending.expiresAt = QDateTime::currentMSecsSinceEpoch() - 1;
    }
    QVERIFY(QMetaObject::invokeMethod(&manager, "checkPositionConfirmations", Qt::DirectConnection));
    QCOMPARE(failed.count(), 0);
    QVERIFY(!manager.hasPendingRequests());
}

void TestWindowManager::identifiesUnlabelledGamescopeWindowByPid()
{
    WindowManager manager;
    m_kwin.windowInfo = {{QStringLiteral("pid"), 4242},
                         {QStringLiteral("resourceClass"), QString()},
                         {QStringLiteral("desktopFile"), QString()}};
    QCOMPARE(manager.findAllGamescopeWindows(4242), QStringList{QStringLiteral("{launcher}")});
    QVERIFY(manager.findAllGamescopeWindows(4243).isEmpty());
    m_kwin.windowInfo[QStringLiteral("resourceClass")] = QStringLiteral("gamescope");
    QVERIFY(manager.findAllGamescopeWindows(4243).isEmpty());
}

QTEST_MAIN(TestWindowManager)
#include "test_windowmanager.moc"
