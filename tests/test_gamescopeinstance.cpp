// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QMap> // Ensures QMap<QString, QVariant> is complete
#include <QProcess> // For QProcess::ExitStatus used in signals
#include <QRect>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVariant> // Must be before QVariantMap
#include <QVariantMap>
#include "CouchPlayHelperClient.h"
#include "GamescopeInstance.h"

class MockCouchPlayHelperClient : public CouchPlayHelperClient
{
    Q_OBJECT
public:
    using CouchPlayHelperClient::CouchPlayHelperClient;
    bool isAvailable() const override { return true; }

    qint64 nextPid = 5000;
    qint64 lastPid = 0;
    QString lastUsername;
    QStringList lastGameCommand;
    QVariantList lastPreparations;
    int launchCalls = 0;
    QList<qint64> stopPids;
    QList<qint64> killPids;
    bool readyBeforeReply = true;
    bool stoppedBeforeReply = false;
    bool stopSucceeds = true;
    bool killSucceeds = true;
    QString earlyStopReason = QStringLiteral("failed");

    qint64 launchInstance(const QString &username,
                          const QString &,
                          const QStringList &,
                          const QStringList &gameCommand,
                          const QString &,
                          const QStringList &,
                          const QStringList &,
                          bool,
                          const QVariantList &directoryPreparations) override
    {
        ++launchCalls;
        lastUsername = username;
        lastGameCommand = gameCommand;
        lastPreparations = directoryPreparations;
        lastPid = nextPid++;
        if (readyBeforeReply) Q_EMIT instanceReady(username, lastPid);
        if (stoppedBeforeReply) Q_EMIT instanceStopped(username, lastPid, earlyStopReason);
        return lastPid;
    }

    bool stopInstance(qint64 pid) override
    {
        stopPids.append(pid);
        if (stopSucceeds) Q_EMIT instanceStopped(lastUsername, pid, QStringLiteral("exited"));
        return stopSucceeds;
    }

    bool killInstance(qint64 pid) override
    {
        killPids.append(pid);
        if (killSucceeds) Q_EMIT instanceStopped(lastUsername, pid, QStringLiteral("failed"));
        return killSucceeds;
    }
};


class TestGamescopeInstance : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // buildGamescopeArgs tests
    void testBuildArgsMinimal();
    void testBuildArgsResolution();
    void testBuildArgsRefreshRate();
    void testBuildArgsScalingMode();
    void testBuildArgsFilterMode();
    void testBuildArgsPosition();
    void testBuildArgsInputDevices();
    void testBuildArgsFullConfig();
    void testBuildArgs_SteamLauncherId();
    void testBuildArgs_NonSteamLauncherId();

    // buildEnvironment tests
    void testBuildEnvBasic();
    void testBuildEnvCustomPulseServer();

    // Instance state and readiness tests
    void testInitialState();
    void testNotRunningWithoutProcess();
    void testDoubleStartPrevention();
    void testPreparingUntilMatchingReady();
    void testReadySignalBeforeLaunchReply();
    void testPreparationFailureBeforeLaunchReply();
    void testStopWhilePreparing();
    void testStopFailureRetainsPreparingPid();
    void testEmptyLaunchCommandRejected();

    // Property tests
    void testIndexProperty();
    void testUsernameProperty();
    void testWindowGeometryProperty();

    // Structured launch tests
    void testSteamLaunchMode();
    void testSteamLaunchModeNoAppId();

private:
    GamescopeInstance *m_instance = nullptr;
    MockCouchPlayHelperClient *m_helperClient = nullptr;
};

void TestGamescopeInstance::initTestCase()
{
}

void TestGamescopeInstance::cleanupTestCase()
{
}

void TestGamescopeInstance::init()
{
    m_helperClient = new MockCouchPlayHelperClient();
    m_instance = new GamescopeInstance();
    m_instance->setHelperClient(m_helperClient);
}

void TestGamescopeInstance::cleanup()
{
    delete m_instance;
    m_instance = nullptr;
    delete m_helperClient;
    m_helperClient = nullptr;
}

// ============ buildGamescopeArgs Tests ============

void TestGamescopeInstance::testBuildArgsMinimal()
{
    QVariantMap config;
    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    // Should NOT include -e by default (no launcherId set)
    QVERIFY(!args.contains(QStringLiteral("-e")));

    // Should NOT include -b by default (decorated windows for resizing)
    QVERIFY(!args.contains(QStringLiteral("-b")));

    // Should have default resolutions
    QVERIFY(args.contains(QStringLiteral("-w")));
    QVERIFY(args.contains(QStringLiteral("-h")));
    QVERIFY(args.contains(QStringLiteral("-W")));
    QVERIFY(args.contains(QStringLiteral("-H")));
}

void TestGamescopeInstance::testBuildArgsResolution()
{
    QVariantMap config;
    config[QStringLiteral("internalWidth")] = 2560;
    config[QStringLiteral("internalHeight")] = 1440;
    config[QStringLiteral("outputWidth")] = 1280;
    config[QStringLiteral("outputHeight")] = 720;

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    // Check internal resolution
    int wIdx = args.indexOf(QStringLiteral("-w"));
    QVERIFY(wIdx >= 0);
    QCOMPARE(args[wIdx + 1], QStringLiteral("2560"));

    int hIdx = args.indexOf(QStringLiteral("-h"));
    QVERIFY(hIdx >= 0);
    QCOMPARE(args[hIdx + 1], QStringLiteral("1440"));

    // Check output resolution
    int WIdx = args.indexOf(QStringLiteral("-W"));
    QVERIFY(WIdx >= 0);
    QCOMPARE(args[WIdx + 1], QStringLiteral("1280"));

    int HIdx = args.indexOf(QStringLiteral("-H"));
    QVERIFY(HIdx >= 0);
    QCOMPARE(args[HIdx + 1], QStringLiteral("720"));
}

void TestGamescopeInstance::testBuildArgsRefreshRate()
{
    QVariantMap config;
    config[QStringLiteral("refreshRate")] = 144;

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    int rIdx = args.indexOf(QStringLiteral("-r"));
    QVERIFY(rIdx >= 0);
    QCOMPARE(args[rIdx + 1], QStringLiteral("144"));
}

void TestGamescopeInstance::testBuildArgsScalingMode()
{
    QVariantMap config;
    config[QStringLiteral("scalingMode")] = QStringLiteral("integer");

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    int SIdx = args.indexOf(QStringLiteral("-S"));
    QVERIFY(SIdx >= 0);
    QCOMPARE(args[SIdx + 1], QStringLiteral("integer"));
}

void TestGamescopeInstance::testBuildArgsFilterMode()
{
    QVariantMap config;
    config[QStringLiteral("filterMode")] = QStringLiteral("fsr");

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    int FIdx = args.indexOf(QStringLiteral("-F"));
    QVERIFY(FIdx >= 0);
    QCOMPARE(args[FIdx + 1], QStringLiteral("fsr"));
}

void TestGamescopeInstance::testBuildArgsPosition()
{
    QVariantMap config;
    config[QStringLiteral("positionX")] = 960;
    config[QStringLiteral("positionY")] = 0;

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    // NOTE: --position is NOT available in all gamescope versions (e.g., Bazzite)
    // So we currently skip position args and rely on window manager or manual positioning
    // This test verifies that we DON'T add --position (since it's disabled)
    int posIdx = args.indexOf(QStringLiteral("--position"));
    QCOMPARE(posIdx, -1); // Should NOT be present
}

void TestGamescopeInstance::testBuildArgsInputDevices()
{
    // Input device isolation is NOT done via gamescope flags.
    // Gamescope has no --input-device flag, and --grab only grabs keyboard.
    // Instead, isolation is achieved by changing device ownership (chown/chmod 600)
    // so each user can only read devices they own.
    // This test verifies we DON'T pass invalid gamescope flags.

    QVariantMap config;
    QVariantList devices;
    devices << QStringLiteral("/dev/null"); // Use /dev/null as a stand-in
    config[QStringLiteral("devicePaths")] = devices;

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    // Should NOT contain --input-device (not a valid gamescope flag)
    QVERIFY(!args.contains(QStringLiteral("--input-device")));

    // Should NOT contain --grab (only grabs keyboard, not useful for gamepad isolation)
    QVERIFY(!args.contains(QStringLiteral("--grab")));
}

void TestGamescopeInstance::testBuildArgsFullConfig()
{
    QVariantMap config;
    config[QStringLiteral("internalWidth")] = 1920;
    config[QStringLiteral("internalHeight")] = 1080;
    config[QStringLiteral("outputWidth")] = 960;
    config[QStringLiteral("outputHeight")] = 1080;
    config[QStringLiteral("refreshRate")] = 60;
    config[QStringLiteral("scalingMode")] = QStringLiteral("fit");
    config[QStringLiteral("filterMode")] = QStringLiteral("linear");
    config[QStringLiteral("positionX")] = 0;
    config[QStringLiteral("positionY")] = 0;
    config[QStringLiteral("borderless")] = true; // Enable borderless for this test

    QVariantList devices;
    devices << QStringLiteral("/dev/null");
    config[QStringLiteral("devicePaths")] = devices;

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    // Verify all expected args are present
    QVERIFY(!args.contains(QStringLiteral("-e"))); // Should NOT include -e (no launcherId set)
    QVERIFY(args.contains(QStringLiteral("-b"))); // Now enabled because borderless=true
    QVERIFY(args.contains(QStringLiteral("-w")));
    QVERIFY(args.contains(QStringLiteral("-h")));
    QVERIFY(args.contains(QStringLiteral("-W")));
    QVERIFY(args.contains(QStringLiteral("-H")));
    QVERIFY(args.contains(QStringLiteral("-r")));
    QVERIFY(args.contains(QStringLiteral("-S")));
    QVERIFY(args.contains(QStringLiteral("-F")));
    // NOTE: --position is disabled due to gamescope version compatibility
    QVERIFY(!args.contains(QStringLiteral("--position")));
    // NOTE: Input device isolation is done via ownership, not gamescope flags
    QVERIFY(!args.contains(QStringLiteral("--input-device")));
    QVERIFY(!args.contains(QStringLiteral("--grab")));
}

// ============ buildEnvironment Tests ============

void TestGamescopeInstance::testBuildArgs_SteamLauncherId()
{
    QVariantMap config;
    config[QStringLiteral("launcherId")] = QStringLiteral("steam");

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    QVERIFY2(args.contains(QStringLiteral("-e")), "Expected -e flag for Steam launcher");
}

void TestGamescopeInstance::testBuildArgs_NonSteamLauncherId()
{
    QVariantMap config;
    config[QStringLiteral("launcherId")] = QStringLiteral("heroic");

    QStringList args = GamescopeInstance::buildGamescopeArgs(config);

    QVERIFY(!args.contains(QStringLiteral("-e")));
}

void TestGamescopeInstance::testBuildEnvBasic()
{
    QVariantMap config;
    QStringList env = GamescopeInstance::buildEnvironment(config);

    // Should have essential env vars for Vulkan games
    QVERIFY(!env.isEmpty());
    QVERIFY(env.contains(QStringLiteral("ENABLE_GAMESCOPE_WSI=1")));
    QVERIFY(env.contains(QStringLiteral("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0")));
}

void TestGamescopeInstance::testBuildEnvCustomPulseServer()
{
    QVariantMap config;
    config[QStringLiteral("pulseServer")] = QStringLiteral("tcp:192.168.1.100:4713");

    QStringList env = GamescopeInstance::buildEnvironment(config);

    // pulseServer config is no longer used (each user has own PipeWire session)
    // but we still get the base env vars
    QVERIFY(!env.isEmpty());
    QVERIFY(env.contains(QStringLiteral("ENABLE_GAMESCOPE_WSI=1")));

    // PULSE_SERVER should NOT be in the env
    bool hasPulseServer = false;
    for (const QString &var : env) {
        if (var.startsWith(QStringLiteral("PULSE_SERVER="))) {
            hasPulseServer = true;
            break;
        }
    }
    QVERIFY(!hasPulseServer);
}

// ============ Instance State Tests ============

void TestGamescopeInstance::testInitialState()
{
    QVERIFY(!m_instance->isRunning());
    QCOMPARE(m_instance->index(), -1);
    QCOMPARE(m_instance->pid(), 0);
    QVERIFY(m_instance->status().isEmpty());
    QVERIFY(m_instance->username().isEmpty());
}

void TestGamescopeInstance::testNotRunningWithoutProcess()
{
    // A fresh instance should not be running
    QVERIFY(!m_instance->isRunning());
}

void TestGamescopeInstance::testDoubleStartPrevention()
{
    m_helperClient->readyBeforeReply = false;
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    QVERIFY(m_instance->start(config, 0, {}));
    QCOMPARE(m_instance->pid(), m_helperClient->lastPid);
    QCOMPARE(m_instance->gamescopePid(), qint64(0));
    QVERIFY(!m_instance->isRunning());

    QVERIFY(!m_instance->start(config, 1, {}));
    QCOMPARE(m_helperClient->launchCalls, 1);
    QCOMPARE(m_instance->index(), 0);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player"), m_helperClient->lastPid);
    QVERIFY(m_instance->isRunning());
    QVERIFY(!m_instance->start(config, 2, {}));
    QCOMPARE(m_instance->gamescopePid(), m_helperClient->lastPid);
    QCOMPARE(m_instance->index(), 0);
    m_instance->stop();
}

void TestGamescopeInstance::testPreparingUntilMatchingReady()
{
    QVariantMap config;
    m_helperClient->readyBeforeReply = false;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    const QVariantList preparations{QVariantMap{{QStringLiteral("source"), QStringLiteral("/source")},
                                                {QStringLiteral("mode"), QStringLiteral("copy")},
                                                {QStringLiteral("targetAlias"), QStringLiteral(".couchplay/copies/test")},
                                                {QStringLiteral("stagingPath"), QStringLiteral("/staging")}}};
    QSignalSpy startedSpy(m_instance, &GamescopeInstance::started);
    QVERIFY(m_instance->start(config, 0, preparations));
    const qint64 pid = m_helperClient->lastPid;
    QCOMPARE(m_helperClient->lastPreparations, preparations);
    QCOMPARE(m_instance->pid(), pid);
    QCOMPARE(m_instance->gamescopePid(), qint64(0));
    QCOMPARE(m_instance->status(), QStringLiteral("Preparing data"));
    QVERIFY(!m_instance->isRunning());

    Q_EMIT m_helperClient->instanceReady(QStringLiteral("other"), pid);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player"), pid + 1);
    QCOMPARE(m_instance->gamescopePid(), qint64(0));
    QVERIFY(!m_instance->isRunning());
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player"), pid);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player"), pid);
    QCOMPARE(m_instance->gamescopePid(), pid);
    QVERIFY(m_instance->isRunning());
    QCOMPARE(startedSpy.count(), 1);
    m_instance->stop();
}

void TestGamescopeInstance::testReadySignalBeforeLaunchReply()
{
    m_helperClient->readyBeforeReply = true;
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    QSignalSpy startedSpy(m_instance, &GamescopeInstance::started);
    QVERIFY(m_instance->start(config, 0, {}));
    QCOMPARE(m_instance->pid(), m_helperClient->lastPid);
    QCOMPARE(m_instance->gamescopePid(), m_helperClient->lastPid);
    QVERIFY(m_instance->isRunning());
    QCOMPARE(startedSpy.count(), 1);
    m_instance->stop();
}

void TestGamescopeInstance::testPreparationFailureBeforeLaunchReply()
{
    m_helperClient->stoppedBeforeReply = true;
    m_helperClient->readyBeforeReply = false;
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    QSignalSpy errorSpy(m_instance, &GamescopeInstance::errorOccurred);
    QSignalSpy startedSpy(m_instance, &GamescopeInstance::started);
    QSignalSpy stoppedSpy(m_instance, &GamescopeInstance::stopped);
    QVERIFY(m_instance->start(config, 0, {}));
    QCOMPARE(m_instance->pid(), qint64(0));
    QCOMPARE(m_instance->gamescopePid(), qint64(0));
    QVERIFY(!m_instance->isRunning());
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), m_helperClient->earlyStopReason);
}

void TestGamescopeInstance::testStopWhilePreparing()
{
    m_helperClient->readyBeforeReply = false;
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    QSignalSpy stoppedSpy(m_instance, &GamescopeInstance::stopped);
    QVERIFY(m_instance->start(config, 0, {}));
    const qint64 preparingPid = m_instance->pid();
    QVERIFY(preparingPid > 0);
    QVERIFY(!m_instance->isRunning());
    m_instance->stop();
    QCOMPARE(m_helperClient->stopPids, QList<qint64>{preparingPid});
    QCOMPARE(m_instance->pid(), qint64(0));
    QCOMPARE(stoppedSpy.count(), 1);
}

void TestGamescopeInstance::testStopFailureRetainsPreparingPid()
{
    m_helperClient->readyBeforeReply = false;
    m_helperClient->stopSucceeds = false;
    m_helperClient->killSucceeds = false;
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    QVERIFY(m_instance->start(config, 0, {}));
    const qint64 preparingPid = m_instance->pid();
    m_instance->stop();
    QCOMPARE(m_instance->pid(), preparingPid);
    QVERIFY(!m_instance->isRunning());
    m_helperClient->stopSucceeds = true;
    m_instance->stop();
    QCOMPARE(m_instance->pid(), qint64(0));
}

void TestGamescopeInstance::testEmptyLaunchCommandRejected()
{
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    QSignalSpy errorSpy(m_instance, &GamescopeInstance::errorOccurred);
    QVERIFY(!m_instance->start(config, 0, {}));
    QCOMPARE(m_helperClient->launchCalls, 0);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Launch command is empty"));
}

// ============ Property Tests ============

void TestGamescopeInstance::testIndexProperty()
{
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    QVERIFY(m_instance->start(config, 5, {}));
    QCOMPARE(m_instance->index(), 5);
    m_instance->stop();
}

void TestGamescopeInstance::testUsernameProperty()
{
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("testuser");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    QVERIFY(m_instance->start(config, 0, {}));
    QCOMPARE(m_instance->username(), QStringLiteral("testuser"));
    m_instance->stop();
}

void TestGamescopeInstance::testWindowGeometryProperty()
{
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("/usr/bin/true")};
    config[QStringLiteral("positionX")] = 100;
    config[QStringLiteral("positionY")] = 200;
    config[QStringLiteral("outputWidth")] = 800;
    config[QStringLiteral("outputHeight")] = 600;
    QVERIFY(m_instance->start(config, 0, {}));
    const QRect geometry = m_instance->windowGeometry();
    QCOMPARE(geometry.x(), 100);
    QCOMPARE(geometry.y(), 200);
    QCOMPARE(geometry.width(), 800);
    QCOMPARE(geometry.height(), 600);
    m_instance->stop();
}

// ============ Structured Launch Tests ============

void TestGamescopeInstance::testSteamLaunchMode()
{
    m_helperClient->readyBeforeReply = true;
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("launcherId")] = QStringLiteral("steam");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("steam"),
                                                         QStringLiteral("-bigpicture"),
                                                         QStringLiteral("-applaunch"),
                                                         QStringLiteral("1426210")};
    QVERIFY(m_instance->start(config, 0, {}));
    QCOMPARE(m_helperClient->lastGameCommand, config.value(QStringLiteral("gameCommand")).toStringList());
    QVERIFY(m_instance->isRunning());
    m_instance->stop();
}

void TestGamescopeInstance::testSteamLaunchModeNoAppId()
{
    m_helperClient->readyBeforeReply = true;
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("player");
    config[QStringLiteral("launcherId")] = QStringLiteral("steam");
    config[QStringLiteral("gameCommand")] = QStringList{QStringLiteral("steam"), QStringLiteral("-bigpicture")};
    QVERIFY(m_instance->start(config, 0, {}));
    QCOMPARE(m_helperClient->lastGameCommand, config.value(QStringLiteral("gameCommand")).toStringList());
    QVERIFY(m_instance->isRunning());
    m_instance->stop();
}

QTEST_MAIN(TestGamescopeInstance)
#include "test_gamescopeinstance.moc"
