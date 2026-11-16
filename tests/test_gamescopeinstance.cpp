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
#include "GamescopeInstance.h"


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
    void testBuildArgsScaledOutput();
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
    void testBuildEnvSteamIpcOverride();
    void testBuildEnvCustomPulseServer();

    // Instance state and readiness tests
    void testInitialState();
    void testNotRunningWithoutProcess();
    void testEmptyLaunchCommandRejected();
    // Property tests
    void testIndexProperty();
    void testPlayerProperty();
    void testWindowGeometryProperty();

private:
    GamescopeInstance *m_instance = nullptr;
};

void TestGamescopeInstance::initTestCase()
{
}

void TestGamescopeInstance::cleanupTestCase()
{
}

void TestGamescopeInstance::init()
{
    m_instance = new GamescopeInstance();
}

void TestGamescopeInstance::cleanup()
{
    delete m_instance;
    m_instance = nullptr;
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

// On a 150% output KWin places a 640x720 logical window, which is 960x1080 physical pixels; gamescope takes its
// window size in physical pixels, so a logical -W/-H would make the game render and display stretched.
void TestGamescopeInstance::testBuildArgsScaledOutput()
{
    QVariantMap config;
    config[QStringLiteral("internalWidth")] = 960;
    config[QStringLiteral("internalHeight")] = 1080;
    config[QStringLiteral("outputWidth")] = 640;
    config[QStringLiteral("outputHeight")] = 720;
    config[QStringLiteral("outputScale")] = 1.5;

    const QStringList args = GamescopeInstance::buildGamescopeArgs(config);
    QCOMPARE(args.value(args.indexOf(QStringLiteral("-W")) + 1), QStringLiteral("960"));
    QCOMPARE(args.value(args.indexOf(QStringLiteral("-H")) + 1), QStringLiteral("1080"));
    QCOMPARE(args.value(args.indexOf(QStringLiteral("-w")) + 1), QStringLiteral("960"));
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

void TestGamescopeInstance::testBuildEnvSteamIpcOverride()
{
    QVariantMap config;
    QVERIFY(!GamescopeInstance::buildEnvironment(config).join(QLatin1Char(' ')).contains(QStringLiteral("steam_master_ipc")));

    config[QStringLiteral("steamIpcAddress")] = QStringLiteral("127.0.0.1:41234");
    QVERIFY(GamescopeInstance::buildEnvironment(config).contains(
        QStringLiteral("steam_master_ipc_name_override=127.0.0.1:41234")));
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
    QVERIFY(m_instance->player().isEmpty());
}

void TestGamescopeInstance::testNotRunningWithoutProcess()
{
    // A fresh instance should not be running
    QVERIFY(!m_instance->isRunning());
}

void TestGamescopeInstance::testEmptyLaunchCommandRejected()
{
    QVariantMap config;
    config[QStringLiteral("player")] = QStringLiteral("player");
    QSignalSpy errorSpy(m_instance, &GamescopeInstance::errorOccurred);
    QVERIFY(!m_instance->start(config, 0, {}));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Launch command is empty"));
    QVERIFY(!m_instance->isRunning());
}

// ============ Property Tests ============

void TestGamescopeInstance::testIndexProperty()
{
    QVariantMap config;
    QVERIFY(!m_instance->start(config, 5, {}));
    QCOMPARE(m_instance->index(), 5);
}

void TestGamescopeInstance::testPlayerProperty()
{
    QVariantMap config;
    config[QStringLiteral("player")] = QStringLiteral("testuser");
    QVERIFY(!m_instance->start(config, 0, {}));
    QCOMPARE(m_instance->player(), QStringLiteral("testuser"));
}

void TestGamescopeInstance::testWindowGeometryProperty()
{
    QVariantMap config;
    config[QStringLiteral("positionX")] = 100;
    config[QStringLiteral("positionY")] = 200;
    config[QStringLiteral("outputWidth")] = 800;
    config[QStringLiteral("outputHeight")] = 600;
    QVERIFY(!m_instance->start(config, 0, {}));
    const QRect geometry = m_instance->windowGeometry();
    QCOMPARE(geometry.x(), 100);
    QCOMPARE(geometry.y(), 200);
    QCOMPARE(geometry.width(), 800);
    QCOMPARE(geometry.height(), 600);
}

QTEST_MAIN(TestGamescopeInstance)
#include "test_gamescopeinstance.moc"
