// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QVariant>      // Must be before QVariantMap
#include <QVariantMap>
#include <QMap>          // Ensures QMap<QString, QVariant> is complete
#include <QString>
#include <QStringList>
#include <QRect>
#include <QProcess>      // For QProcess::ExitStatus used in signals

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
    void testBuildArgsRefreshRate();
    void testBuildArgsScalingMode();
    void testBuildArgsFilterMode();
    void testBuildArgsPosition();
    void testBuildArgsInputDevices();
    void testBuildArgsFullConfig();
    
    // buildEnvironment tests
    void testBuildEnvPrimary();
    void testBuildEnvSecondary();
    void testBuildEnvCustomPulseServer();
    
    // buildSecondaryUserCommand tests
    void testBuildSecondaryCommandBasic();
    void testBuildSecondaryCommandWithEnv();
    void testBuildSecondaryCommandWithArgs();
    
    // Instance state tests
    void testInitialState();
    void testNotRunningWithoutProcess();
    void testDoubleStartPrevention();
    
    // Property tests
    void testIndexProperty();
    void testUsernameProperty();
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
    
    // Should always include -e (Steam integration)
    QVERIFY(args.contains(QStringLiteral("-e")));
    
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
    QCOMPARE(posIdx, -1);  // Should NOT be present
}

void TestGamescopeInstance::testBuildArgsInputDevices()
{
    // Create a temporary file to simulate a device path
    // Since we can't create real device files, test with a file that exists
    QVariantMap config;
    QVariantList devices;
    devices << QStringLiteral("/dev/null"); // Use /dev/null as a stand-in
    config[QStringLiteral("devicePaths")] = devices;
    
    QStringList args = GamescopeInstance::buildGamescopeArgs(config);
    
    int inputIdx = args.indexOf(QStringLiteral("--input-device"));
    QVERIFY(inputIdx >= 0);
    QCOMPARE(args[inputIdx + 1], QStringLiteral("/dev/null"));
    
    // Should also have --grab when devices are specified
    QVERIFY(args.contains(QStringLiteral("--grab")));
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
    config[QStringLiteral("borderless")] = true;  // Enable borderless for this test
    
    QVariantList devices;
    devices << QStringLiteral("/dev/null");
    config[QStringLiteral("devicePaths")] = devices;
    
    QStringList args = GamescopeInstance::buildGamescopeArgs(config);
    
    // Verify all expected args are present
    QVERIFY(args.contains(QStringLiteral("-e")));
    QVERIFY(args.contains(QStringLiteral("-b")));  // Now enabled because borderless=true
    QVERIFY(args.contains(QStringLiteral("-w")));
    QVERIFY(args.contains(QStringLiteral("-h")));
    QVERIFY(args.contains(QStringLiteral("-W")));
    QVERIFY(args.contains(QStringLiteral("-H")));
    QVERIFY(args.contains(QStringLiteral("-r")));
    QVERIFY(args.contains(QStringLiteral("-S")));
    QVERIFY(args.contains(QStringLiteral("-F")));
    // NOTE: --position is disabled due to gamescope version compatibility
    QVERIFY(!args.contains(QStringLiteral("--position")));
    QVERIFY(args.contains(QStringLiteral("--input-device")));
    QVERIFY(args.contains(QStringLiteral("--grab")));
}

// ============ buildEnvironment Tests ============

void TestGamescopeInstance::testBuildEnvPrimary()
{
    QVariantMap config;
    QStringList env = GamescopeInstance::buildEnvironment(config, true);
    
    // Primary instance should NOT have PULSE_SERVER (uses local PipeWire)
    bool hasPulseServer = false;
    for (const QString &var : env) {
        if (var.startsWith(QStringLiteral("PULSE_SERVER="))) {
            hasPulseServer = true;
            break;
        }
    }
    QVERIFY(!hasPulseServer);
    
    // Note: SDL_VIDEODRIVER is no longer set since gamescope handles its own backend
    // Secondary users use --backend wayland with ACL-granted access to Wayland socket
}

void TestGamescopeInstance::testBuildEnvSecondary()
{
    QVariantMap config;
    QStringList env = GamescopeInstance::buildEnvironment(config, false);
    
    // With machinectl shell, each user has their own PipeWire session
    // so buildEnvironment now returns empty (no PULSE_SERVER forwarding needed)
    QVERIFY(env.isEmpty());
}

void TestGamescopeInstance::testBuildEnvCustomPulseServer()
{
    QVariantMap config;
    config[QStringLiteral("pulseServer")] = QStringLiteral("tcp:192.168.1.100:4713");
    
    QStringList env = GamescopeInstance::buildEnvironment(config, false);
    
    // With machinectl shell, PULSE_SERVER config is ignored
    // Each user has their own PipeWire session
    QVERIFY(env.isEmpty());
}

// ============ buildSecondaryUserCommand Tests ============

void TestGamescopeInstance::testBuildSecondaryCommandBasic()
{
    QString username = QStringLiteral("player2");
    QStringList environment;
    QStringList gamescopeArgs = {QStringLiteral("-e"), QStringLiteral("-b")};
    QString steamArgs = QStringLiteral("-gamepadui");
    
    QString command = GamescopeInstance::buildSecondaryUserCommand(
        username, environment, gamescopeArgs, steamArgs);
    
    // Should contain username
    QVERIFY(command.contains(QStringLiteral("player2")));
    // Should contain gamescope
    QVERIFY(command.contains(QStringLiteral("gamescope")));
    // Should contain steam
    QVERIFY(command.contains(QStringLiteral("steam")));
    // Should contain gamepadui
    QVERIFY(command.contains(QStringLiteral("-gamepadui")));
    // Should use pkexec, sudo, or machinectl for privilege escalation
    QVERIFY(command.contains(QStringLiteral("pkexec")) || 
            command.contains(QStringLiteral("sudo")) || 
            command.contains(QStringLiteral("machinectl")));
}

void TestGamescopeInstance::testBuildSecondaryCommandWithEnv()
{
    QString username = QStringLiteral("player2");
    // With machinectl shell, each user has their own PipeWire session
    // so PULSE_SERVER is intentionally filtered out from the command
    // Instead, test that WAYLAND_DISPLAY is set to an absolute path
    QStringList environment = {QStringLiteral("PULSE_SERVER=tcp:127.0.0.1:4713")};
    QStringList gamescopeArgs;
    QString steamArgs;
    
    QString command = GamescopeInstance::buildSecondaryUserCommand(
        username, environment, gamescopeArgs, steamArgs);
    
    // Should use machinectl shell for the user's session
    QVERIFY(command.contains(QStringLiteral("machinectl shell player2@")) ||
            command.contains(QStringLiteral("runuser")));  // fallback
    
    // Should contain WAYLAND_DISPLAY with absolute path to primary's socket
    QVERIFY(command.contains(QStringLiteral("WAYLAND_DISPLAY=/run/user/")));
    
    // PULSE_SERVER should be filtered out (each user has own PipeWire)
    QVERIFY(!command.contains(QStringLiteral("PULSE_SERVER=")));
}

void TestGamescopeInstance::testBuildSecondaryCommandWithArgs()
{
    QString username = QStringLiteral("player2");
    QStringList environment;
    QStringList gamescopeArgs = {
        QStringLiteral("-w"), QStringLiteral("1920"),
        QStringLiteral("-h"), QStringLiteral("1080"),
        QStringLiteral("-W"), QStringLiteral("960")
    };
    QString steamArgs = QStringLiteral("-gamepadui -applaunch 730");
    
    QString command = GamescopeInstance::buildSecondaryUserCommand(
        username, environment, gamescopeArgs, steamArgs);
    
    // Should contain gamescope args
    QVERIFY(command.contains(QStringLiteral("-w 1920")));
    QVERIFY(command.contains(QStringLiteral("-W 960")));
    // Should contain steam args
    QVERIFY(command.contains(QStringLiteral("-applaunch 730")));
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
    // Note: We can't actually start gamescope in tests, but we can verify
    // the protection logic exists in the code
    // The start() method checks m_process->state() before starting
    
    // For now, just verify the instance can receive start() calls
    QSignalSpy errorSpy(m_instance, &GamescopeInstance::errorOccurred);
    
    QVariantMap config;
    config[QStringLiteral("internalWidth")] = 1920;
    config[QStringLiteral("internalHeight")] = 1080;
    
    // First start will attempt to run (may fail if gamescope not installed)
    // We're mainly testing that the method is callable
    m_instance->start(config, 0);
    
    // We don't assert on the result because gamescope may not be installed
    // in the test environment
}

// ============ Property Tests ============

void TestGamescopeInstance::testIndexProperty()
{
    QVariantMap config;
    m_instance->start(config, 5);
    QCOMPARE(m_instance->index(), 5);
    m_instance->stop();
}

void TestGamescopeInstance::testUsernameProperty()
{
    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("testuser");
    m_instance->start(config, 0);
    QCOMPARE(m_instance->username(), QStringLiteral("testuser"));
    m_instance->stop();
}

void TestGamescopeInstance::testWindowGeometryProperty()
{
    QVariantMap config;
    config[QStringLiteral("positionX")] = 100;
    config[QStringLiteral("positionY")] = 200;
    config[QStringLiteral("outputWidth")] = 800;
    config[QStringLiteral("outputHeight")] = 600;
    
    m_instance->start(config, 0);
    
    QRect geometry = m_instance->windowGeometry();
    QCOMPARE(geometry.x(), 100);
    QCOMPARE(geometry.y(), 200);
    QCOMPARE(geometry.width(), 800);
    QCOMPARE(geometry.height(), 600);
    
    m_instance->stop();
}

QTEST_MAIN(TestGamescopeInstance)
#include "test_gamescopeinstance.moc"
