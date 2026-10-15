// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDateTime>
#include <QFile>
#include <QJSEngine>
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
public:
    QString windowId = QStringLiteral("{launcher}");
public Q_SLOTS:
    QList<WindowMatch> Match(const QString &) const
    {
        return {{QStringLiteral("0_") + windowId, QStringLiteral("Example Launcher"), {}, 0, 0,
                 {{QStringLiteral("description"), QStringLiteral("Window")}}}};
    }
};

class MockKWin : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin")

public:
    QVariantMap windowInfo;
    QStringList hiddenWindows;
public Q_SLOTS:
    QVariantMap getWindowInfo(const QString &windowId) const
    {
        if (hiddenWindows.contains(windowId)) return {};
        return windowInfo;
    }
};

class MockKWinScripting : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")
public:
    QString source;
    QJSEngine *engine = nullptr;
public Q_SLOTS:
    int loadScript(const QString &filePath, const QString &pluginName)
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) return -1;
        source = QString::fromUtf8(file.readAll());
        Q_UNUSED(pluginName);
        return 1;
    }

    void unloadScript(const QString &pluginName)
    {
        Q_UNUSED(pluginName);
        if (engine) engine->evaluate(QStringLiteral("workspace.windowAdded.handlers = [];"));
    }
};

class MockKWinScript : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Script")

public:
    QJSEngine *engine = nullptr;
    QString *source = nullptr;
public Q_SLOTS:
    void run() { if (engine && source) engine->evaluate(*source); }
};

class TestWindowManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void directPositionSupersedesOlderConfirmation();
    void replacementBeforeConfirmation();
    void replacementWindowKeepsInstanceGeometry();
    void identifiesUnlabelledGamescopeWindowByPid();

private:
    QJSEngine m_engine;
    static constexpr auto serviceName = "org.kde.KWin";
    MockKWin m_kwin;
    MockKWinScripting m_scripting;
    MockKWinScript m_script;
    MockWindowsRunner m_runner;
};

void TestWindowManager::initTestCase()
{
    m_scripting.engine = &m_engine;
    m_script.engine = &m_engine;
    m_script.source = &m_scripting.source;
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

void TestWindowManager::init()
{
    m_kwin.windowInfo.clear();
    m_kwin.hiddenWindows.clear();
    m_runner.windowId = QStringLiteral("{launcher}");
    m_engine.evaluate(QStringLiteral(R"(
        var workspace = {
            windows: [],
            windowList: function() { return this.windows; },
            windowAdded: {handlers: [], connect: function(handler) { this.handlers.push(handler); }}
        };
        function makeWindow(id, pid) {
            return {internalId: id, pid: pid, fullScreen: false,
                    setMaximize: function() {},
                    frameGeometry: {x: 480, y: 0, width: 960, height: 1080}};
        }
    )"));
}

void TestWindowManager::replacementWindowKeepsInstanceGeometry()
{
    WindowManager manager;
    const QRect target(0, 0, 960, 1080);
    m_kwin.windowInfo = {{QStringLiteral("pid"), 4242}, {QStringLiteral("x"), 0},
                        {QStringLiteral("y"), 0}, {QStringLiteral("width"), 960}, {QStringLiteral("height"), 1080}};
    m_engine.evaluate(QStringLiteral("workspace.windows = [makeWindow('{launcher}', 4242)];"));
    QSignalSpy ready(&manager, &WindowManager::gamescopeWindowPositioned);
    manager.queuePositionRequest(0, 4242, target, {}, true, 60000);
    manager.checkPositionConfirmations();
    QCOMPARE(ready.count(), 1);
    const auto frame = [&](const QString &name) {
        const QJSValue value = m_engine.globalObject().property(name).property(QStringLiteral("frameGeometry"));
        return QRect(value.property(QStringLiteral("x")).toInt(), value.property(QStringLiteral("y")).toInt(),
                     value.property(QStringLiteral("width")).toInt(), value.property(QStringLiteral("height")).toInt());
    };
    m_engine.evaluate(QStringLiteral(R"(
        var replacement = makeWindow('{replacement}', 4242);
        workspace.windows = [replacement];
        workspace.windowAdded.handlers.forEach(function(handler) { handler(replacement); });
    )"));
    QCOMPARE(frame(QStringLiteral("replacement")), target);
    QCOMPARE(ready.count(), 1);
    m_engine.evaluate(QStringLiteral(R"(
        var unrelated = makeWindow('{unrelated}', 4243);
        workspace.windowAdded.handlers.forEach(function(handler) { handler(unrelated); });
    )"));
    QCOMPARE(frame(QStringLiteral("unrelated")), QRect(480, 0, 960, 1080));
    manager.cancelAllRequests();
    m_engine.evaluate(QStringLiteral(R"(
        var afterStop = makeWindow('{after-stop}', 4242);
        workspace.windowAdded.handlers.forEach(function(handler) { handler(afterStop); });
    )"));
    QCOMPARE(frame(QStringLiteral("afterStop")), QRect(480, 0, 960, 1080));
}
void TestWindowManager::replacementBeforeConfirmation()
{
    WindowManager manager;
    m_kwin.windowInfo = {{QStringLiteral("pid"), 4242}, {QStringLiteral("x"), 0},
                        {QStringLiteral("y"), 0}, {QStringLiteral("width"), 960}, {QStringLiteral("height"), 1080}};
    m_engine.evaluate(QStringLiteral("workspace.windows = [makeWindow('{launcher}', 4242)];"));
    QSignalSpy ready(&manager, &WindowManager::gamescopeWindowPositioned);
    manager.queuePositionRequest(0, 4242, QRect(0, 0, 960, 1080), {}, true, 60000);
    m_kwin.hiddenWindows.append(QStringLiteral("{launcher}"));
    m_runner.windowId = QStringLiteral("{replacement}");
    m_engine.evaluate(QStringLiteral(R"(
        var replacement = makeWindow('{replacement}', 4242);
        workspace.windows = [replacement];
        workspace.windowAdded.handlers.forEach(function(handler) { handler(replacement); });
    )"));
    manager.checkPositionConfirmations();
    QCOMPARE(ready.count(), 1);
    QCOMPARE(ready.constFirst().at(1).toString(), QStringLiteral("{replacement}"));
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
