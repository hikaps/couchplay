// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <functional>
#include <memory>
#include <vector>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#define private public
#include "DeviceManager.h"
#undef private
#define private public
#include "WindowManager.h"
#include "SessionRunner.h"
#include "PresetManager.h"
#undef private
#define private public
#include "GamescopeInstance.h"
#undef private
#include "PadForwarder.h"
#include "PlayerFiles.h"
#include "PlayerStore.h"
#include "SessionManager.h"
#include "SteamConfigManager.h"
#include "StreamManager.h"
#include "SunshineConfig.h"
#include "VirtualDisplayManager.h"

class TestEnvironmentGuard
{
public:
    explicit TestEnvironmentGuard(const char *name)
        : m_name(name), m_value(qgetenv(name)), m_wasSet(qEnvironmentVariableIsSet(name)) {}
    ~TestEnvironmentGuard()
    {
        if (m_wasSet) qputenv(m_name.constData(), m_value);
        else qunsetenv(m_name.constData());
    }
private:
    QByteArray m_name;
    QByteArray m_value;
    bool m_wasSet;
};

struct FakeLog
{
    QList<QVariantMap> configs;
    QList<int> indices;
    QList<QList<SandboxMount>> mounts;
    QList<int> stops;
    QList<int> kills;
    bool autoStart = true;
    bool startFails = false;
};

// Stands in for the bwrap sandbox: records what the runner passes to start() and keeps a
// harmless child alive so pid() reports a running instance.
class FakeGamescopeInstance : public GamescopeInstance
{
public:
    FakeGamescopeInstance(FakeLog *log, QObject *parent)
        : GamescopeInstance(parent), m_log(log) {}

    ~FakeGamescopeInstance() override
    {
        m_process.kill();
        m_process.waitForFinished(2000);
        m_launched = false;
    }

    bool start(const QVariantMap &config, int index, const QList<SandboxMount> &mounts) override
    {
        m_index = index;
        m_player = config.value(QStringLiteral("player")).toString();
        m_log->configs.append(config);
        m_log->indices.append(index);
        m_log->mounts.append(mounts);
        if (m_log->startFails) {
            Q_EMIT errorOccurred(QStringLiteral("simulated Gamescope launch failure"));
            return false;
        }
        m_process.start(QStringLiteral("sleep"), {QStringLiteral("30")});
        if (!m_process.waitForStarted(3000)) {
            return false;
        }
        m_launched = true;
        if (m_log->autoStart) {
            Q_EMIT started();
        }
        return true;
    }

    void stop(int) override
    {
        m_log->stops.append(m_index);
        finish();
    }

    void kill() override
    {
        m_log->kills.append(m_index);
        finish();
    }

    void exitNaturally() { finish(); }

private:
    void finish()
    {
        if (!m_launched) {
            return;
        }
        m_stopping = true;
        m_process.kill();
        m_process.waitForFinished(2000);
        m_launched = false;
        m_stopping = false;
        Q_EMIT stopped();
    }

    FakeLog *m_log;
};

namespace
{
bool writeExecutable(const QString &path, const QByteArray &body)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write("#!/bin/sh\n" + body + "\n");
    file.close();
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

// Fake couchplay-vdisplay, pactl and sunshine so streaming sessions run without a compositor.
class StreamingFakes
{
public:
    explicit StreamingFakes(SessionRunner *runner, bool vdisplayWorks = true, bool sunshineExists = true)
        : m_oldDataHome(qgetenv("XDG_DATA_HOME"))
        , m_hadDataHome(qEnvironmentVariableIsSet("XDG_DATA_HOME"))
    {
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        const QString vdisplay = m_dir.filePath(QStringLiteral("vdisplay"));
        writeExecutable(vdisplay,
                        vdisplayWorks ? QByteArray("echo 'READY 1'\nexec sleep 30")
                                      : QByteArray("echo 'protocol missing' >&2\nexit 2"));
        runner->setSkipPairingCheck(true);
        const QString pactl = m_dir.filePath(QStringLiteral("pactl"));
        writeExecutable(pactl,
                        "case \"$1\" in\nget-default-sink) echo alsa_default;;\nload-module) echo 7;;\nesac\nexit 0");
        const QString sunshine = m_dir.filePath(QStringLiteral("sunshine"));
        writeExecutable(sunshine,
                        "conf=\"$1\"\ndir=$(dirname \"$conf\")\n"
                        "if [ \"$2\" = \"--creds\" ]; then echo '{}' > \"$dir/credentials.json\"; exit 0; fi\n"
                        "log=$(sed -n 's/^log_path = //p' \"$conf\")\n"
                        "echo 'Info: Configuration UI available at [https://localhost:48020]' >> \"$log\"\n"
                        "exec sleep 30");

        runner->m_virtualDisplays->setProgramOverride(vdisplay);
        runner->m_virtualDisplays->setScreenLookup(
            [](const QString &name, const QSize &size, const QSet<QString> &, QString *outName, QRect *geometry) {
                *outName = name;
                *geometry = QRect(QPoint(5000, 0), size);
                return true;
            });
        runner->m_streamAudio.setProgramOverride(pactl);
        runner->m_streamManager->setProgramOverride(sunshineExists ? sunshine
                                                                   : QStringLiteral("/nonexistent/sunshine"));
    }

    ~StreamingFakes()
    {
        if (m_hadDataHome) {
            qputenv("XDG_DATA_HOME", m_oldDataHome);
        } else {
            qunsetenv("XDG_DATA_HOME");
        }
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_oldDataHome;
    bool m_hadDataHome;
};

SessionProfile displayProfile(const QStringList &displays, const QString &mainScreen = QString())
{
    SessionProfile profile;
    profile.mainScreen = mainScreen;
    for (const QString &display : displays) {
        InstanceConfig instance;
        instance.display = display;
        instance.streamToMoonlight = display == InstanceConfig::DisplayVirtual;
        profile.instances.append(instance);
    }
    return profile;
}

InputDevice makeInputDevice(int eventNumber, const QString &name, const QString &type, const QString &uniq = QString())
{
    InputDevice device;
    device.eventNumber = eventNumber;
    device.path = QStringLiteral("/dev/input/event%1").arg(eventNumber);
    device.name = name;
    device.type = type;
    device.uniq = uniq;
    return device;
}
}

class TestSessionRunner : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    // Start validation
    void testStartRejectsUnknownPlayer();
    void testStartRejectsDuplicatePlayer();
    void testStartRejectsMissingHostTool_data();
    void testStartRejectsMissingHostTool();
    void testStartRejectsInvalidDisplay();
    void testValidateDisplaysMonitorNotConnected();
    void testValidateDisplaysSharedMonitor();
    void testValidateDisplaysMonitorIsMainScreen();
    void testValidateDisplaysStreamWithMain();
    void testValidateDisplaysValidProfile();
    void testValidatePairingGate();
    void testValidatePairingSkipsUnpairedNonStreamingAndNoUser();
    void testStartRejectsUnpairedStreamingPlayer();
    void testStartSkipPairingCheck();
    void testStartAllowsPairedStreamingPlayer();

    // Launch config and pad forwarders
    void testLaunchConfigCarriesPlayerHomeAndPad();
    void testPadForwarderFailureAbortsStartup();
    void testTeardownStopsPadForwarders();
    void testPhysicalInstanceAudioDeviceRoutesOnlyThatPlayer();

    // Data directory modes
    void testDataDirAclRoBindsPathsUnderHome();
    void testDataDirAclIgnoresPathsOutsideHome();
    void testDataDirBindMountsSource();
    void testDataDirMissingSourceFails();
    void testDataDirOverlaySeedsStaging();
    void testDataDirCopyWritesIntoPlayerHome();

    // State machine and hooks
    void testNaturalExitTearsDownSharingState();
    void testPreSessionFailurePreventsLaunch();
    void testPreHookUsesStartingProfileSnapshot();
    void testReadinessAdvancesSequentialStartup();
    void testInstanceErrorAbortsStartup();
    void testCancelWhilePreparing();
    void testStopStopsRunningInstances();
    void testStoppedInstanceCancelsItsPositionListener();
    void testSynchronousLaunchFailureRunsPostHookOnce();
    void testInvalidPostSessionReportsError();
    void testPostSessionRunsOnceAfterStop();

    // Streaming
    void testVirtualDisplayFailureAbortsStartup();
    void testStreamingInstancesGetDistinctSunshinePorts();
    void testStreamingStartupWaitsForSunshineReady();
    void testStreamingLaunchFailureAbortsStartup();

    // Controllers
    void testDeviceReconnectIgnoredDuringPostHook();
    void testDeviceReconnectIgnoredDuringPreHook();
    void testSunshineGamepadClaimsAreFifo();
    void testSteamInstancesGetPrivateIpcAddress();
    void testGameOverrideAppliesToMatchingPlayersOnly();
    void testGameOverrideWithoutMatchingLauncherFails();
    void testApplyLaunchOverrides();
    void testLauncherOverrideFillsEmptyPlayers();
    void testEmptyLauncherWithoutOverrideFails();
    void testSunshineGamepadUniqGroupsDevices();
    void testSunshineGamepadSingleStreamerFallback();
    void testSunshineGamepadClaimExpires();
    void testSunshineKeyboardMouseNotAttached();
    void testSunshineGamepadUnownedWithoutClaim();

    void testParseRenderResolution();

private:
    QStringList padLog() const;
    QStringList attachLines() const;
    bool attached(int padNumber, int eventNumber) const;
    FakeGamescopeInstance *fake(int index) const;
    QString twoPlayerPreset(const QString &name);
    QString onePlayerPreset(const QString &name);
    bool startSessionWithDevices(const QList<int> &streaming, const QList<InputDevice> &devices);
    void writePadScript(const QByteArray &body);

    std::vector<std::unique_ptr<TestEnvironmentGuard>> m_envGuards;
    std::unique_ptr<QTemporaryDir> m_home;
    std::unique_ptr<QTemporaryDir> m_padDir;
    FakeLog m_log;
    PlayerStore *m_store = nullptr;
    SessionRunner *m_runner = nullptr;
    SessionManager *m_sessionManager = nullptr;
    PresetManager *m_presetManager = nullptr;
    SteamConfigManager *m_steamConfigManager = nullptr;
};

void TestSessionRunner::writePadScript(const QByteArray &body)
{
    const QString script = m_padDir->filePath(QStringLiteral("padfwd"));
    QVERIFY(writeExecutable(script, body));
    PadForwarder::setProgramOverrideForTesting(script);
}

void TestSessionRunner::init()
{
    m_home = std::make_unique<QTemporaryDir>();
    m_padDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_home->isValid());
    QVERIFY(m_padDir->isValid());
    for (const char *name : {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "FLATPAK_ID"}) {
        m_envGuards.push_back(std::make_unique<TestEnvironmentGuard>(name));
    }
    qputenv("HOME", m_home->path().toUtf8());
    qputenv("XDG_DATA_HOME", m_home->filePath(QStringLiteral(".local/share")).toUtf8());
    qputenv("XDG_CONFIG_HOME", m_home->filePath(QStringLiteral(".config")).toUtf8());
    qunsetenv("FLATPAK_ID");

    m_log = FakeLog();
    const QString log = m_padDir->filePath(QStringLiteral("pad.log"));
    writePadScript("n=${2##*P}; n=${n%% *}\n"
                   "echo \"ready /dev/input/event80$n\"\n"
                   "while read line; do echo \"P$n $line\" >> '" + log.toUtf8() + "'; done\n"
                   "echo \"P$n eof\" >> '" + log.toUtf8() + "'");

    m_store = new PlayerStore(this);
    QVERIFY(m_store->createPlayer(QStringLiteral("player1")));
    QVERIFY(m_store->createPlayer(QStringLiteral("player2")));

    m_sessionManager = new SessionManager(this);
    m_presetManager = new PresetManager(this);
    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {}));
    m_steamConfigManager = new SteamConfigManager(this);

    m_runner = new SessionRunner(this);
    m_runner->setSessionManager(m_sessionManager);
    m_runner->setSteamConfigManager(m_steamConfigManager);
    m_runner->setPresetManager(m_presetManager);
    m_runner->setPlayerStore(m_store);
    m_runner->setInstanceFactory([this](QObject *parent) -> GamescopeInstance * {
        return new FakeGamescopeInstance(&m_log, parent);
    });
    m_runner->setToolFinderForTesting([](const QString &name) { return QStringLiteral("/usr/bin/") + name; });
}

void TestSessionRunner::cleanup()
{
    delete m_runner;
    m_runner = nullptr;
    delete m_sessionManager;
    m_sessionManager = nullptr;
    delete m_presetManager;
    m_presetManager = nullptr;
    delete m_steamConfigManager;
    m_steamConfigManager = nullptr;
    delete m_store;
    m_store = nullptr;
    PadForwarder::setProgramOverrideForTesting(QString());
    m_envGuards.clear();
    m_padDir.reset();
    m_home.reset();
}

QStringList TestSessionRunner::padLog() const
{
    QFile file(m_padDir->filePath(QStringLiteral("pad.log")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QStringList TestSessionRunner::attachLines() const
{
    return padLog().filter(QStringLiteral(" attach "));
}

bool TestSessionRunner::attached(int padNumber, int eventNumber) const
{
    return padLog().contains(QStringLiteral("P%1 attach /dev/input/event%2").arg(padNumber).arg(eventNumber));
}

FakeGamescopeInstance *TestSessionRunner::fake(int index) const
{
    return static_cast<FakeGamescopeInstance *>(m_runner->m_instances.at(index));
}

QString TestSessionRunner::twoPlayerPreset(const QString &name)
{
    const QString presetId = m_presetManager->addCustomPreset(name, QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, presetId);
    }
    return presetId;
}

QString TestSessionRunner::onePlayerPreset(const QString &name)
{
    const QString presetId = m_presetManager->addCustomPreset(name, QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    return presetId;
}

// Starts a two-player session on the fake instances, then marks the given players as streaming
// and replaces the device list; everything stays synchronous so no timer can refresh the devices.
bool TestSessionRunner::startSessionWithDevices(const QList<int> &streaming, const QList<InputDevice> &devices)
{
    twoPlayerPreset(QStringLiteral("Gamepad attribution"));
    auto *deviceManager = new DeviceManager(m_runner);
    m_runner->setDeviceManager(deviceManager);
    if (!m_runner->start() || !m_runner->m_sessionStartupComplete) {
        return false;
    }
    for (int index : streaming) {
        m_runner->m_streamingInstances[index] = SessionRunner::StreamingInstanceInfo{};
    }
    deviceManager->m_devices = devices;
    return true;
}

// ---------------------------------------------------------------------------
// Start validation
// ---------------------------------------------------------------------------

void TestSessionRunner::testStartRejectsUnknownPlayer()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Unknown"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("ghost"));
    m_sessionManager->setInstancePreset(0, presetId);
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(!m_runner->start());
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().constFirst().toString(), QStringLiteral("Player \"ghost\" does not exist"));
    QCOMPARE(m_runner->status(), QStringLiteral("Error"));
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_log.configs.isEmpty());
}

void TestSessionRunner::testStartRejectsDuplicatePlayer()
{
    twoPlayerPreset(QStringLiteral("Duplicate"));
    m_sessionManager->setInstanceUser(1, QStringLiteral("player1"));
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(!m_runner->start());
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.first().constFirst().toString().contains(QStringLiteral("assigned to multiple instances")));
    QVERIFY(m_log.configs.isEmpty());
}

void TestSessionRunner::testStartRejectsMissingHostTool_data()
{
    QTest::addColumn<QString>("tool");
    QTest::addColumn<QString>("package");
    QTest::newRow("bwrap") << QStringLiteral("bwrap") << QStringLiteral("bubblewrap");
    QTest::newRow("dbus-run-session") << QStringLiteral("dbus-run-session") << QStringLiteral("dbus");
    QTest::newRow("gamescope") << QStringLiteral("gamescope") << QStringLiteral("gamescope");
}

void TestSessionRunner::testStartRejectsMissingHostTool()
{
    QFETCH(QString, tool);
    QFETCH(QString, package);
    twoPlayerPreset(QStringLiteral("Missing tool"));
    m_runner->setToolFinderForTesting([tool](const QString &name) {
        return name == tool ? QString() : QStringLiteral("/usr/bin/") + name;
    });
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(!m_runner->start());
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().constFirst().toString(),
             QStringLiteral("CouchPlay needs '%1' on the host (package: %2)").arg(tool, package));
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_log.configs.isEmpty());
}

void TestSessionRunner::testStartRejectsInvalidDisplay()
{
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceConfig(1, {{QStringLiteral("display"), QStringLiteral("monitor:DP-99")}});
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(!m_runner->start());
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().constFirst().toString(), QStringLiteral("Player 2: monitor DP-99 is not connected"));
    QCOMPARE(m_runner->status(), QStringLiteral("Error"));
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_log.configs.isEmpty());
}

void TestSessionRunner::testValidateDisplaysMonitorNotConnected()
{
    const SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:DP-99")});
    QCOMPARE(SessionRunner::validateDisplays(profile), QStringLiteral("Player 2: monitor DP-99 is not connected"));
}

void TestSessionRunner::testValidateDisplaysSharedMonitor()
{
    const QStringList screens{QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")};
    const SessionProfile profile = displayProfile({QStringLiteral("monitor:DP-1"), QStringLiteral("monitor:DP-1")});
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, QStringLiteral("HDMI-A-1")),
             QStringLiteral("Players 1 and 2 cannot share monitor DP-1"));
}

void TestSessionRunner::testValidateDisplaysMonitorIsMainScreen()
{
    const QStringList screens{QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")};
    const QString primary = QStringLiteral("HDMI-A-1");
    const QString expected =
        QStringLiteral("Monitor HDMI-A-1 is the main screen; choose another main screen or move the players");

    SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:HDMI-A-1")});
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, primary), expected);

    profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:HDMI-A-1")}, QStringLiteral("HDMI-A-1"));
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, QStringLiteral("DP-1")), expected);

    profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:HDMI-A-1")}, QStringLiteral("DP-1"));
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, primary), QString());

    profile = displayProfile({QStringLiteral("monitor:HDMI-A-1"), QStringLiteral("virtual")});
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, primary), QString());
}

void TestSessionRunner::testValidateDisplaysStreamWithMain()
{
    SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("main")});
    profile.instances[1].streamToMoonlight = true;
    QCOMPARE(SessionRunner::validateDisplays(profile),
             QStringLiteral("Player 2: streaming needs a dedicated monitor or a virtual display"));
}

void TestSessionRunner::testValidateDisplaysValidProfile()
{
    QCOMPARE(SessionRunner::validateDisplays(displayProfile({QStringLiteral("main"), QStringLiteral("main")})),
             QString());
    QCOMPARE(SessionRunner::validateDisplays(displayProfile({QStringLiteral("main"), QStringLiteral("virtual")})),
             QString());
    QCOMPARE(SessionRunner::validateDisplays(displayProfile({QStringLiteral("main"), QStringLiteral("virtual")},
                                                            QStringLiteral("DP-99"))),
             QString());
}

void TestSessionRunner::testValidatePairingGate()
{
    SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("virtual")});
    profile.instances[0].player = QStringLiteral("player1");
    profile.instances[1].player = QStringLiteral("player2");
    const auto none = [](const QString &) { return 0; };
    const auto onlyPlayer2 = [](const QString &player) { return player == QStringLiteral("player2") ? 1 : 0; };

    const QString message = SessionRunner::validatePairing(profile, none);
    QVERIFY(message.contains(QStringLiteral("Player 2 (player2) has no paired Moonlight client")));
    QVERIFY(message.contains(QStringLiteral("Pair one for player2")));
    QCOMPARE(SessionRunner::validatePairing(profile, onlyPlayer2), QString());

    // A real monitor only streams when the player opted in
    profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:DP-1")});
    profile.instances[1].player = QStringLiteral("player2");
    QCOMPARE(SessionRunner::validatePairing(profile, none), QString());
    profile.instances[1].streamToMoonlight = true;
    QVERIFY(!SessionRunner::validatePairing(profile, none).isEmpty());
}

void TestSessionRunner::testValidatePairingSkipsUnpairedNonStreamingAndNoUser()
{
    SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("virtual")});
    profile.instances[0].player = QStringLiteral("player1");
    profile.instances[1].player.clear();
    QCOMPARE(SessionRunner::validatePairing(profile, [](const QString &) { return 0; }), QString());
}

void TestSessionRunner::testStartRejectsUnpairedStreamingPlayer()
{
    twoPlayerPreset(QStringLiteral("Unpaired"));
    m_sessionManager->setInstanceConfig(1, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    StreamingFakes fakes(m_runner);
    m_runner->setSkipPairingCheck(false);
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(!m_runner->start());
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.first().constFirst().toString().contains(QStringLiteral("has no paired Moonlight client")));
    QCOMPARE(m_runner->status(), QStringLiteral("Error"));
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_log.configs.isEmpty());
    QVERIFY(!m_runner->m_virtualDisplays->hasDisplays());
}

void TestSessionRunner::testStartSkipPairingCheck()
{
    QVERIFY(!SessionRunner().skipPairingCheck());
    SessionRunner runner;
    QSignalSpy changedSpy(&runner, &SessionRunner::skipPairingCheckChanged);
    runner.setSkipPairingCheck(true);
    runner.setSkipPairingCheck(true);
    QVERIFY(runner.skipPairingCheck());
    QCOMPARE(changedSpy.count(), 1);
}

void TestSessionRunner::testStartAllowsPairedStreamingPlayer()
{
    twoPlayerPreset(QStringLiteral("Paired"));
    m_sessionManager->setInstanceConfig(1, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    // Sunshine is missing, so a start that passes the gate fails later with a Sunshine error, not a pairing one
    StreamingFakes fakes(m_runner, /*vdisplayWorks=*/true, /*sunshineExists=*/false);
    m_runner->setSkipPairingCheck(false);
    const QString stateDir = SunshineConfig::ensureStateDirectory(QStringLiteral("player2"));
    QVERIFY(!stateDir.isEmpty());
    QFile state(stateDir + QStringLiteral("/sunshine_state.json"));
    QVERIFY(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
    state.write(R"({"root":{"uniqueid":"x","named_devices":[{"name":"Phone","uuid":"u1"}]}})");
    state.close();

    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QVERIFY(m_runner->start());
    QCOMPARE(errorSpy.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
    QVERIFY(!failedSpy.first().constFirst().toString().contains(QStringLiteral("paired")));
    QTRY_VERIFY_WITH_TIMEOUT(!m_runner->isActive(), 3000);
}

// ---------------------------------------------------------------------------
// Launch config and pad forwarders
// ---------------------------------------------------------------------------

void TestSessionRunner::testLaunchConfigCarriesPlayerHomeAndPad()
{
    twoPlayerPreset(QStringLiteral("Launch config"));
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);

    QVERIFY(m_runner->start());
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(m_log.configs.size(), 2);
    QCOMPARE(m_log.indices, (QList<int>{0, 1}));
    for (int i = 0; i < 2; ++i) {
        const QString player = QStringLiteral("player%1").arg(i + 1);
        const QVariantMap &config = m_log.configs.at(i);
        QCOMPARE(config.value(QStringLiteral("player")).toString(), player);
        QCOMPARE(config.value(QStringLiteral("playerHome")).toString(), PlayerStore::homeDir(player));
        QCOMPARE(config.value(QStringLiteral("padNode")).toString(), QStringLiteral("/dev/input/event80%1").arg(i + 1));
        QCOMPARE(config.value(QStringLiteral("gameCommand")).toStringList().value(0), QStringLiteral("/usr/bin/true"));
    }
    m_runner->stop();
}

void TestSessionRunner::testPadForwarderFailureAbortsStartup()
{
    twoPlayerPreset(QStringLiteral("Pad failure"));
    writePadScript("echo 'error uinput Permission denied'\nexit 1");
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    m_runner->start();
    QTRY_VERIFY_WITH_TIMEOUT(!m_runner->isActive(), 6000);
    bool controllerError = false;
    for (const QList<QVariant> &args : errorSpy) {
        controllerError = controllerError || args.constFirst().toString().contains(QStringLiteral("controllers"));
    }
    QVERIFY(controllerError);
    QVERIFY(m_log.configs.isEmpty());
}

void TestSessionRunner::testTeardownStopsPadForwarders()
{
    twoPlayerPreset(QStringLiteral("Pad teardown"));
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(m_runner->m_padForwarders.size(), 2);

    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 3000);
    QVERIFY(m_runner->m_padForwarders.isEmpty());
    // Closing stdin makes the tool exit; the script records it.
    QTRY_VERIFY_WITH_TIMEOUT(padLog().contains(QStringLiteral("P1 eof")), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(padLog().contains(QStringLiteral("P2 eof")), 2000);
}

void TestSessionRunner::testPhysicalInstanceAudioDeviceRoutesOnlyThatPlayer()
{
    twoPlayerPreset(QStringLiteral("Audio routing"));
    QVariantMap playerTwo = m_sessionManager->getInstanceConfig(1);
    playerTwo[QStringLiteral("audioDevice")] = QStringLiteral("alsa_output.usb-soundbar.analog-stereo");
    m_sessionManager->setInstanceConfig(1, playerTwo);

    QVERIFY(m_runner->start());
    QCOMPARE(m_log.configs.size(), 2);
    QCOMPARE(m_log.configs.at(0).value(QStringLiteral("player")).toString(), QStringLiteral("player1"));
    QVERIFY(m_log.configs.at(0).value(QStringLiteral("sink")).toString().isEmpty());
    QCOMPARE(m_log.configs.at(1).value(QStringLiteral("player")).toString(), QStringLiteral("player2"));
    QCOMPARE(m_log.configs.at(1).value(QStringLiteral("sink")).toString(),
             QStringLiteral("alsa_output.usb-soundbar.analog-stereo"));
    m_runner->stop();
}

// ---------------------------------------------------------------------------
// Data directory modes
// ---------------------------------------------------------------------------

void TestSessionRunner::testDataDirAclRoBindsPathsUnderHome()
{
    const QString presetId = onePlayerPreset(QStringLiteral("Acl"));
    const QString source = QDir::homePath() + QStringLiteral("/readonly-data");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(m_presetManager->setDataDirectories(
        presetId, {QVariantMap{{QStringLiteral("path"), source}, {QStringLiteral("mode"), QStringLiteral("acl")}}}));

    QVERIFY(m_runner->setupSessionResources());
    const QList<SandboxMount> mounts = m_runner->m_instanceMounts.value(0);
    QCOMPARE(mounts.size(), 1);
    QCOMPARE(mounts.constFirst().kind, SandboxMount::RoBind);
    QCOMPARE(mounts.constFirst().source, source);
    QCOMPARE(mounts.constFirst().target, source);
}

void TestSessionRunner::testDataDirAclIgnoresPathsOutsideHome()
{
    const QString presetId = onePlayerPreset(QStringLiteral("Acl outside"));
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    QVERIFY(m_presetManager->setDataDirectories(
        presetId,
        {QVariantMap{{QStringLiteral("path"), outside.path()}, {QStringLiteral("mode"), QStringLiteral("acl")}},
         QVariantMap{{QStringLiteral("path"), QString(QDir::homePath() + QStringLiteral("/does-not-exist"))},
                     {QStringLiteral("mode"), QStringLiteral("acl")}}}));

    QVERIFY(m_runner->setupSessionResources());
    // Outside the home the path is already visible, and a missing path has nothing to mount.
    QVERIFY(m_runner->m_instanceMounts.value(0).isEmpty());
}

void TestSessionRunner::testDataDirBindMountsSource()
{
    const QString presetId = onePlayerPreset(QStringLiteral("Bind"));
    const QString source = QDir::homePath() + QStringLiteral("/saves");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(m_presetManager->setDataDirectories(
        presetId, {QVariantMap{{QStringLiteral("path"), source}, {QStringLiteral("mode"), QStringLiteral("bind")}}}));

    QVERIFY(m_runner->setupSessionResources());
    const QList<SandboxMount> mounts = m_runner->m_instanceMounts.value(0);
    QCOMPARE(mounts.size(), 1);
    QCOMPARE(mounts.constFirst().kind, SandboxMount::Bind);
    QCOMPARE(mounts.constFirst().source, source);
    QCOMPARE(mounts.constFirst().target, QDir::homePath() + QStringLiteral("/saves"));
    QVERIFY(m_runner->m_instanceMounts.value(1).isEmpty());
}

void TestSessionRunner::testDataDirMissingSourceFails()
{
    const QString presetId = onePlayerPreset(QStringLiteral("Missing source"));
    QVERIFY(m_presetManager->setDataDirectories(
        presetId,
        {QVariantMap{{QStringLiteral("path"), QString(QDir::homePath() + QStringLiteral("/nope"))},
                     {QStringLiteral("mode"), QStringLiteral("bind")}}}));
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(!m_runner->setupSessionResources());
    QVERIFY(!errorSpy.isEmpty());
    QVERIFY(errorSpy.first().constFirst().toString().contains(QStringLiteral("Shared directory does not exist")));
}

void TestSessionRunner::testDataDirOverlaySeedsStaging()
{
    const QString presetId = onePlayerPreset(QStringLiteral("Overlay"));
    const QString source = QDir::homePath() + QStringLiteral("/overlay-src");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(m_presetManager->setDataDirectories(
        presetId, {QVariantMap{{QStringLiteral("path"), source}, {QStringLiteral("mode"), QStringLiteral("overlay")}}}));

    const QString staging = playerDataStagingRoot(presetId, QStringLiteral("player1")) + QLatin1Char('/')
        + PlayerFiles::dataDirectoryStagingSlug(source, QDir::homePath());
    QVERIFY(QDir().mkpath(staging));
    QFile seed(staging + QStringLiteral("/seed.txt"));
    QVERIFY(seed.open(QIODevice::WriteOnly));
    seed.write("seeded");
    seed.close();

    QVERIFY(m_runner->setupSessionResources());
    const QList<SandboxMount> mounts = m_runner->m_instanceMounts.value(0);
    QCOMPARE(mounts.size(), 1);
    const SandboxMount &mount = mounts.constFirst();
    const QString layer = PlayerStore::layerDir(QStringLiteral("player1"), source);
    QCOMPARE(mount.kind, SandboxMount::Overlay);
    QCOMPARE(mount.source, source);
    QCOMPARE(mount.target, source);
    QCOMPARE(mount.upper, layer + QStringLiteral("/upper"));
    QCOMPARE(mount.work, layer + QStringLiteral("/work"));
    QVERIFY(QFileInfo(mount.work).isDir());
    QVERIFY(QFile::exists(mount.upper + QStringLiteral("/seed.txt")));
}

void TestSessionRunner::testDataDirCopyWritesIntoPlayerHome()
{
    const QString presetId = onePlayerPreset(QStringLiteral("Copy"));
    const QString source = QDir::homePath() + QStringLiteral("/conf");
    QVERIFY(QDir().mkpath(source));
    QFile original(source + QStringLiteral("/a.txt"));
    QVERIFY(original.open(QIODevice::WriteOnly));
    original.write("a");
    original.close();
    QVERIFY(QFile::link(QStringLiteral("a.txt"), source + QStringLiteral("/link")));
    QVERIFY(m_presetManager->setDataDirectories(
        presetId, {QVariantMap{{QStringLiteral("path"), source}, {QStringLiteral("mode"), QStringLiteral("copy")}}}));

    const QString staging = playerDataStagingRoot(presetId, QStringLiteral("player1")) + QLatin1Char('/')
        + PlayerFiles::dataDirectoryStagingSlug(source, QDir::homePath());
    QVERIFY(QDir().mkpath(staging));
    QFile staged(staging + QStringLiteral("/staged.txt"));
    QVERIFY(staged.open(QIODevice::WriteOnly));
    staged.write("s");
    staged.close();

    QVERIFY(m_runner->setupSessionResources());
    const QString dest = PlayerStore::homeDir(QStringLiteral("player1")) + QStringLiteral("/conf");
    // The private copy is bound over its own path so a read-only passthrough can never hide it
    const QList<SandboxMount> mounts = m_runner->m_instanceMounts.value(0);
    QCOMPARE(mounts.size(), 1);
    QCOMPARE(mounts.constFirst().kind, SandboxMount::Bind);
    QCOMPARE(mounts.constFirst().source, dest);
    QCOMPARE(mounts.constFirst().target, QDir::homePath() + QStringLiteral("/conf"));
    QVERIFY(QFile::exists(dest + QStringLiteral("/a.txt")));
    QVERIFY(QFile::exists(dest + QStringLiteral("/staged.txt")));
    QVERIFY(QFileInfo(dest + QStringLiteral("/link")).isSymLink());
}

// ---------------------------------------------------------------------------
// State machine and hooks
// ---------------------------------------------------------------------------

void TestSessionRunner::testNaturalExitTearsDownSharingState()
{
    const QString presetId = twoPlayerPreset(QStringLiteral("Natural exit"));
    const QString source = QDir::homePath() + QStringLiteral("/game-data");
    QVERIFY(QDir().mkpath(source));
    QVERIFY(m_presetManager->setDataDirectories(
        presetId, {QVariantMap{{QStringLiteral("path"), source}, {QStringLiteral("mode"), QStringLiteral("overlay")}}}));
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(m_runner->m_sharedStateActive);
    QCOMPARE(m_log.mounts.size(), 2);
    for (const QList<SandboxMount> &mounts : m_log.mounts) {
        QCOMPARE(mounts.size(), 1);
        QCOMPARE(mounts.constFirst().kind, SandboxMount::Overlay);
    }

    FakeGamescopeInstance *first = fake(0);
    FakeGamescopeInstance *second = fake(1);
    first->exitNaturally();
    QVERIFY(m_runner->isActive());
    QVERIFY(m_runner->m_sharedStateActive);
    second->exitNaturally();

    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 3000);
    QVERIFY(!m_runner->m_sharedStateActive);
    QVERIFY(m_runner->m_instances.isEmpty());
    m_runner->stop();
    QCOMPARE(stoppedSpy.count(), 1);
}

void TestSessionRunner::testPreSessionFailurePreventsLaunch()
{
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setPreSessionExecutable(QStringLiteral("/usr/bin/false"));
    QSignalSpy failureSpy(m_runner, &SessionRunner::sessionStartFailed);

    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 2000);
    QCOMPARE(m_log.configs.size(), 0);
    QVERIFY(!m_runner->isActive());
}

void TestSessionRunner::testPreHookUsesStartingProfileSnapshot()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString prePath = scriptDir.filePath(QStringLiteral("pre.sh"));
    const QString originalPostPath = scriptDir.filePath(QStringLiteral("post-original.sh"));
    const QString changedPostPath = scriptDir.filePath(QStringLiteral("post-changed.sh"));
    const QString originalMarker = scriptDir.filePath(QStringLiteral("original.log"));
    const QString changedMarker = scriptDir.filePath(QStringLiteral("changed.log"));
    QVERIFY(writeExecutable(prePath, "sleep 0.2"));
    QVERIFY(writeExecutable(originalPostPath, "echo original >> \"" + originalMarker.toUtf8() + "\""));
    QVERIFY(writeExecutable(changedPostPath, "echo changed >> \"" + changedMarker.toUtf8() + "\""));

    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setPreSessionExecutable(prePath);
    m_sessionManager->setPostSessionExecutable(originalPostPath);
    QTimer::singleShot(50, this, [this, changedPostPath]() {
        m_sessionManager->setPostSessionExecutable(changedPostPath);
    });

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 3000);
    QCOMPARE(m_log.configs.size(), 2);
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 3000);

    QFile originalMarkerFile(originalMarker);
    QVERIFY(originalMarkerFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(originalMarkerFile.readAll(), QByteArray("original\n"));
    QVERIFY(!QFile::exists(changedMarker));
}

void TestSessionRunner::testReadinessAdvancesSequentialStartup()
{
    twoPlayerPreset(QStringLiteral("Serial startup"));
    m_log.autoStart = false;
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    QCOMPARE(m_log.configs.size(), 1);
    QCOMPARE(m_log.configs.constFirst().value(QStringLiteral("player")).toString(), QStringLiteral("player1"));
    QVERIFY(!m_runner->m_sessionStartupComplete);

    Q_EMIT m_runner->m_instances.at(0)->started();
    QCOMPARE(m_log.configs.size(), 2);
    QCOMPARE(m_log.configs.constLast().value(QStringLiteral("player")).toString(), QStringLiteral("player2"));
    QCOMPARE(startedSpy.count(), 0);

    Q_EMIT m_runner->m_instances.at(1)->started();
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(m_runner->m_sessionStartupComplete);
    QCOMPARE(failedSpy.count(), 0);
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 2000);
}

void TestSessionRunner::testInstanceErrorAbortsStartup()
{
    twoPlayerPreset(QStringLiteral("Instance error"));
    m_log.autoStart = false;
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    Q_EMIT m_runner->m_instances.at(0)->started();
    QCOMPARE(m_log.configs.size(), 2);
    Q_EMIT m_runner->m_instances.at(1)->errorOccurred(QStringLiteral("sandbox exploded"));

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3000);
    QVERIFY(failedSpy.first().constFirst().toString().contains(QStringLiteral("sandbox exploded")));
    QVERIFY(m_log.stops.contains(0));
    QTRY_VERIFY_WITH_TIMEOUT(!m_runner->isActive(), 3000);
}

void TestSessionRunner::testCancelWhilePreparing()
{
    onePlayerPreset(QStringLiteral("Cancel preparation"));
    m_log.autoStart = false;
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    QCOMPARE(m_log.configs.size(), 1);
    QVERIFY(m_runner->isActive());
    QVERIFY(!m_runner->isRunning());
    m_runner->stop();

    QCOMPARE(m_log.stops, QList<int>{0});
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(!m_runner->isActive());
}

void TestSessionRunner::testStopStopsRunningInstances()
{
    twoPlayerPreset(QStringLiteral("Stop all"));
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());

    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 3000);
    QCOMPARE(QSet<int>(m_log.stops.begin(), m_log.stops.end()), (QSet<int>{0, 1}));
    QVERIFY(!m_runner->isActive());
}

void TestSessionRunner::testStoppedInstanceCancelsItsPositionListener()
{
    twoPlayerPreset(QStringLiteral("Listener lifetime"));
    WindowManager windowManager;
    m_runner->m_windowManager = &windowManager;
    const auto detach = qScopeGuard([&] {
        if (m_runner->isActive()) m_runner->stop();
        m_runner->m_windowManager = nullptr;
    });
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 2000);

    // A PID-scoped listener for each instance; only the exited instance's one must go.
    windowManager.m_positionScripts.insert(0, QStringLiteral("listener-0"));
    windowManager.m_positionScripts.insert(1, QStringLiteral("listener-1"));
    fake(1)->exitNaturally();
    QVERIFY(m_runner->isActive());
    QVERIFY(!windowManager.m_positionScripts.contains(1));
    QVERIFY(windowManager.m_positionScripts.contains(0));
}

void TestSessionRunner::testSynchronousLaunchFailureRunsPostHookOnce()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString markerPath = scriptDir.filePath(QStringLiteral("post.log"));
    const QString scriptPath = scriptDir.filePath(QStringLiteral("post.sh"));
    QVERIFY(writeExecutable(scriptPath, "sleep 0.2\nprintf post >> '" + markerPath.toUtf8() + "'"));

    onePlayerPreset(QStringLiteral("Launch failure"));
    m_sessionManager->setPostSessionExecutable(scriptPath);
    m_log.startFails = true;
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3000);
    QCOMPARE(m_log.configs.size(), 1);
    QVERIFY(errorSpy.first().constFirst().toString().contains(QStringLiteral("simulated Gamescope launch failure")));
    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(marker.readAll(), QByteArray("post"));
}

void TestSessionRunner::testInvalidPostSessionReportsError()
{
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setPostSessionExecutable(QStringLiteral("relative/post.sh"));
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    m_runner->stop();
    QTRY_VERIFY_WITH_TIMEOUT(!errorSpy.isEmpty(), 2000);
    QVERIFY(errorSpy.first().at(0).toString().contains(QStringLiteral("Post-session script is not executable")));
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 2000);
}

void TestSessionRunner::testPostSessionRunsOnceAfterStop()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString markerPath = scriptDir.filePath(QStringLiteral("post.log"));
    const QString scriptPath = scriptDir.filePath(QStringLiteral("post.sh"));
    QVERIFY(writeExecutable(scriptPath, "echo post >> \"" + markerPath.toUtf8() + "\""));

    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setPostSessionExecutable(scriptPath);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    QVERIFY(m_runner->isActive());
    QCOMPARE(m_log.configs.size(), 2);
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 2000);
    QVERIFY(!m_runner->isActive());

    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(marker.readAll(), QByteArray("post\n"));
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void TestSessionRunner::testVirtualDisplayFailureAbortsStartup()
{
    twoPlayerPreset(QStringLiteral("Vdisplay failure"));
    m_sessionManager->setInstanceConfig(1, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    StreamingFakes fakes(m_runner, /*vdisplayWorks=*/false);

    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3000);
    QVERIFY(failedSpy.first().constFirst().toString().contains(
        QStringLiteral("Player 2: could not create virtual display")));
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(m_log.configs.size(), 0);
    QVERIFY(!m_runner->isActive());
    QVERIFY(!m_runner->m_virtualDisplays->hasDisplays());
}

void TestSessionRunner::testStreamingInstancesGetDistinctSunshinePorts()
{
    twoPlayerPreset(QStringLiteral("Streaming ports"));
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceConfig(i, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    }
    StreamingFakes fakes(m_runner);
    m_log.autoStart = false;

    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(m_runner->m_pendingInstanceConfigs.size(), 2, 3000);
    for (int i = 0; i < 2; ++i) {
        const QVariantMap config = m_runner->m_pendingInstanceConfigs.value(i);
        QCOMPARE(config.value(QStringLiteral("sunshinePort")).toInt(),
                 SunshineConfig::userPort(QStringLiteral("player%1").arg(i + 1)));
        QCOMPARE(config.value(QStringLiteral("monitorName")).toString(),
                 QStringLiteral("Virtual-couchplay-p%1").arg(i + 1));
        QCOMPARE(config.value(QStringLiteral("sink")).toString(), QStringLiteral("couchplay-stream-p%1").arg(i + 1));
        QCOMPARE(config.value(QStringLiteral("positionX")).toInt(), 5000);
    }

    m_runner->stop();
}

void TestSessionRunner::testSteamInstancesGetPrivateIpcAddress()
{
    // Launcher detection matches the executable's basename; the command must also resolve
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    const QString steamStub = bin.filePath(QStringLiteral("steam"));
    QVERIFY(QFile::copy(QStringLiteral("/usr/bin/true"), steamStub));
    const QString steamPreset =
        m_presetManager->addCustomPreset(QStringLiteral("Steam IPC"), steamStub + QStringLiteral(" -bigpicture"));
    // No Steam install here; the IPC port follows the launcher, not the integration check
    QVERIFY(m_presetManager->setRequiredIntegrations(steamPreset, {}));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, steamPreset);
    }

    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);
    QVERIFY2(m_runner->start(), errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constLast().constFirst().toString()));
    QTRY_COMPARE_WITH_TIMEOUT(m_log.configs.size(), 2, 3000);

    // Each Steam player gets its own loopback IPC address through the environment only: the flag is forwarded
    // to the game's argv and breaks Halo Infinite.
    QSet<QString> addresses;
    for (const QVariantMap &config : m_log.configs) {
        const QString address = config.value(QStringLiteral("steamIpcAddress")).toString();
        QVERIFY2(address.startsWith(QStringLiteral("127.0.0.1:")), qPrintable(address));
        QVERIFY(address != QStringLiteral("127.0.0.1:57343"));
        addresses.insert(address);
        QVERIFY(!config.value(QStringLiteral("gameCommand"))
                     .toStringList()
                     .contains(QStringLiteral("-master_ipc_name_override")));
    }
    QCOMPARE(addresses.size(), 2);
    m_runner->stop();
}

void TestSessionRunner::testGameOverrideAppliesToMatchingPlayersOnly()
{
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    const QString steamStub = bin.filePath(QStringLiteral("steam"));
    QVERIFY(QFile::copy(QStringLiteral("/usr/bin/true"), steamStub));
    const QString steamPreset =
        m_presetManager->addCustomPreset(QStringLiteral("Steam game"), steamStub + QStringLiteral(" -bigpicture"));
    QVERIFY(m_presetManager->setRequiredIntegrations(steamPreset, {}));
    const QString otherPreset = m_presetManager->addCustomPreset(QStringLiteral("Other"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, steamPreset);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, otherPreset);

    m_runner->setGameOverride({{QStringLiteral("launcherId"), QStringLiteral("steam")},
                               {QStringLiteral("backend"), QStringLiteral("native")},
                               {QStringLiteral("gameId"), QStringLiteral("440")}});
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);
    QVERIFY2(m_runner->start(), errorSpy.isEmpty() ? "" : qPrintable(errorSpy.constLast().constFirst().toString()));
    QTRY_COMPARE_WITH_TIMEOUT(m_log.configs.size(), 2, 3000);

    const QStringList first = m_log.configs.at(0).value(QStringLiteral("gameCommand")).toStringList();
    QVERIFY(first.contains(QStringLiteral("-applaunch")));
    QVERIFY(first.contains(QStringLiteral("440")));
    QVERIFY(!first.contains(QStringLiteral("-bigpicture")));
    QCOMPARE(m_log.configs.at(1).value(QStringLiteral("gameCommand")).toStringList(),
             QStringList{QStringLiteral("/usr/bin/true")});
    // One-shot: the saved profile keeps its own selection
    QVERIFY(m_sessionManager->currentProfile().instances.at(0).gameSelection.gameId.isEmpty());
    m_runner->stop();
}

void TestSessionRunner::testGameOverrideWithoutMatchingLauncherFails()
{
    twoPlayerPreset(QStringLiteral("No heroic"));
    m_runner->setGameOverride({{QStringLiteral("launcherId"), QStringLiteral("heroic")},
                               {QStringLiteral("backend"), QStringLiteral("gog")},
                               {QStringLiteral("gameId"), QStringLiteral("x")}});
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);
    QVERIFY(!m_runner->start());
    QVERIFY(!errorSpy.isEmpty());
    QVERIFY(errorSpy.constLast().constFirst().toString().contains(QStringLiteral("required by --game")));
    QVERIFY(m_log.configs.isEmpty());
}

void TestSessionRunner::testApplyLaunchOverrides()
{
    const auto profileOf = [](const QStringList &presetIds) {
        SessionProfile profile;
        for (const QString &id : presetIds) {
            InstanceConfig instance;
            instance.presetId = id;
            profile.instances.append(instance);
        }
        return profile;
    };
    GameSelection steamGame;
    steamGame.launcherId = QStringLiteral("steam");
    steamGame.backend = QStringLiteral("native");
    steamGame.gameId = QStringLiteral("440");

    // The game's launcher fills empty players; players with another launcher are untouched
    SessionProfile profile = profileOf({QString(), QStringLiteral("heroic")});
    QCOMPARE(SessionRunner::applyLaunchOverrides(profile, QString(), steamGame, *m_presetManager), QString());
    QCOMPARE(profile.instances.at(0).presetId, QStringLiteral("steam"));
    QCOMPARE(profile.instances.at(0).gameSelection.gameId, QStringLiteral("440"));
    QCOMPARE(profile.instances.at(1).presetId, QStringLiteral("heroic"));
    QVERIFY(profile.instances.at(1).gameSelection.isEmpty());

    // A launcher matches by id or by name, ignoring case
    const QString emuId = m_presetManager->addCustomPreset(QStringLiteral("Emu"), QStringLiteral("/usr/bin/true"));
    for (const QString &launcher : {QStringLiteral("emu"), emuId}) {
        profile = profileOf({QString(), QString()});
        QCOMPARE(SessionRunner::applyLaunchOverrides(profile, launcher, GameSelection(), *m_presetManager), QString());
        QCOMPARE(profile.instances.at(0).presetId, emuId);
        QCOMPARE(profile.instances.at(1).presetId, emuId);
    }

    profile = profileOf({QString()});
    QCOMPARE(SessionRunner::applyLaunchOverrides(profile, QStringLiteral("nope"), GameSelection(), *m_presetManager),
             QStringLiteral("Unknown launcher: nope"));

    m_presetManager->addCustomPreset(QStringLiteral("Dup"), QStringLiteral("/usr/bin/true"));
    m_presetManager->addCustomPreset(QStringLiteral("Dup"), QStringLiteral("/usr/bin/false"));
    QVERIFY(SessionRunner::applyLaunchOverrides(profile, QStringLiteral("dup"), GameSelection(), *m_presetManager)
                .contains(QStringLiteral("ambiguous")));

    profile = profileOf({QString(), QStringLiteral("heroic")});
    QVERIFY(SessionRunner::applyLaunchOverrides(profile, QString(), GameSelection(), *m_presetManager)
                .startsWith(QStringLiteral("No launcher set for player 1")));
}

void TestSessionRunner::testLauncherOverrideFillsEmptyPlayers()
{
    twoPlayerPreset(QStringLiteral("Emu"));
    m_sessionManager->setInstancePreset(0, QString());
    m_sessionManager->setInstancePreset(1, QString());
    m_runner->setLauncherOverride(QStringLiteral("emu"));
    QVERIFY(m_runner->start());
    QTRY_COMPARE(m_log.configs.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(m_log.configs.at(i).value(QStringLiteral("gameCommand")).toStringList(),
                 QStringList{QStringLiteral("/usr/bin/true")});
    }
    QVERIFY(m_sessionManager->currentProfile().instances.at(0).presetId.isEmpty());
    m_runner->stop();
}

void TestSessionRunner::testEmptyLauncherWithoutOverrideFails()
{
    twoPlayerPreset(QStringLiteral("Emu"));
    m_sessionManager->setInstancePreset(0, QString());
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);
    QVERIFY(!m_runner->start());
    QVERIFY(!errorSpy.isEmpty());
    QVERIFY(errorSpy.constLast().constFirst().toString().startsWith(QStringLiteral("No launcher set for player 1")));
    QVERIFY(m_log.configs.isEmpty());
}

void TestSessionRunner::testStreamingStartupWaitsForSunshineReady()
{
    twoPlayerPreset(QStringLiteral("Streaming startup"));
    m_sessionManager->setInstanceConfig(0, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    StreamingFakes fakes(m_runner);
    m_log.autoStart = false;

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy streamStartedSpy(m_runner->m_streamManager, &StreamManager::streamStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(m_log.configs.size(), 1, 3000);
    QCOMPARE(m_runner->m_streamManager->streamState(0), StreamManager::NotStarted);
    Q_EMIT m_runner->m_instances.at(0)->started();

    // The next player waits until Sunshine for this one reports ready.
    QTRY_COMPARE_WITH_TIMEOUT(streamStartedSpy.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(m_log.configs.size(), 2, 3000);
    QCOMPARE(startedSpy.count(), 0);
    Q_EMIT m_runner->m_instances.at(1)->started();
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 3000);
    QCOMPARE(failedSpy.count(), 0);

    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 10000);
    QVERIFY(!m_runner->m_streamAudio.isActive());
    QVERIFY(!m_runner->m_virtualDisplays->hasDisplays());
    QVERIFY(!m_runner->m_streamManager->hasOwnedPids());
}

void TestSessionRunner::testStreamingLaunchFailureAbortsStartup()
{
    twoPlayerPreset(QStringLiteral("Streaming failure"));
    m_sessionManager->setInstanceConfig(0, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    StreamingFakes fakes(m_runner, /*vdisplayWorks=*/true, /*sunshineExists=*/false);

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(m_log.configs.size(), 1);
    QVERIFY(failedSpy.first().constFirst().toString().contains(QStringLiteral("Sunshine")));
    QTRY_VERIFY_WITH_TIMEOUT(!m_runner->isActive(), 3000);
    QVERIFY(!m_runner->m_streamAudio.isActive());
    QVERIFY(!m_runner->m_virtualDisplays->hasDisplays());
}

// ---------------------------------------------------------------------------
// Controllers
// ---------------------------------------------------------------------------

void TestSessionRunner::testDeviceReconnectIgnoredDuringPostHook()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString scriptPath = scriptDir.filePath(QStringLiteral("post.sh"));
    QVERIFY(writeExecutable(scriptPath, "sleep 0.4"));

    twoPlayerPreset(QStringLiteral("Device reconnect"));
    m_sessionManager->setPostSessionExecutable(scriptPath);
    auto *deviceManager = new DeviceManager(m_runner);
    m_runner->setDeviceManager(deviceManager);
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QCOMPARE(startedSpy.count(), 1);
    deviceManager->m_devices = {makeInputDevice(99, QStringLiteral("Test pad"), QStringLiteral("controller"))};

    m_runner->stop();
    QTRY_VERIFY_WITH_TIMEOUT(m_runner->m_hookProcess && m_runner->m_hookIsPost, 1000);
    m_runner->onDeviceReconnected(QStringLiteral("test-device"), 99, 1);
    QTest::qWait(150);
    QVERIFY(attachLines().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 2000);
}

void TestSessionRunner::testDeviceReconnectIgnoredDuringPreHook()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString scriptPath = scriptDir.filePath(QStringLiteral("pre.sh"));
    QVERIFY(writeExecutable(scriptPath, "sleep 0.4"));

    twoPlayerPreset(QStringLiteral("Prehook reconnect"));
    m_sessionManager->setPreSessionExecutable(scriptPath);
    auto *deviceManager = new DeviceManager(m_runner);
    m_runner->setDeviceManager(deviceManager);
    deviceManager->m_devices = {makeInputDevice(99, QStringLiteral("Test pad"), QStringLiteral("controller"))};
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QVERIFY(m_runner->start());
    QVERIFY(m_runner->m_hookProcess && !m_runner->m_hookIsPost);

    // A reconnect before the hook finishes must not attach anything.
    m_runner->onDeviceReconnected(QStringLiteral("test-device"), 99, 0);
    QTest::qWait(100);
    QVERIFY(attachLines().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 3000);

    // Positive control: once started, the same reconnect attaches the pad to player 1's virtual pad.
    deviceManager->m_devices = {makeInputDevice(99, QStringLiteral("Test pad"), QStringLiteral("controller"))};
    m_runner->onDeviceReconnected(QStringLiteral("test-device"), 99, 0);
    QTRY_VERIFY_WITH_TIMEOUT(attached(1, 99), 2000);
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadClaimsAreFifo()
{
    const QString pad = QStringLiteral("Sunshine PS5 (virtual) pad");
    QVERIFY(startSessionWithDevices({0, 1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller")),
                                     makeInputDevice(902, pad, QStringLiteral("controller"))}));

    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(0);
    QCOMPARE(m_runner->m_gamepadClaims.size(), 2);

    m_runner->onDeviceAdded(901, pad);
    m_runner->onDeviceAdded(902, pad);

    QTRY_VERIFY_WITH_TIMEOUT(attached(2, 901), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(attached(1, 902), 2000);
    QVERIFY(m_runner->m_gamepadClaims.isEmpty());

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadUniqGroupsDevices()
{
    const QString pad = QStringLiteral("Sunshine PS5 (virtual) pad");
    QVERIFY(startSessionWithDevices({0, 1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller"), QStringLiteral("sunshine-pad-1")),
                                     makeInputDevice(902, pad + QStringLiteral(" motion"), QStringLiteral("other"),
                                                     QStringLiteral("sunshine-pad-1"))}));
    m_runner->m_clockOverrideMs = 10000;
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);

    m_runner->onDeviceAdded(901, pad);
    // Within 3 s the motion node shares the uniq and is grouped with the first node without a claim,
    // but only the controller node is forwarded: motion axes would otherwise drive the virtual pad's sticks.
    m_runner->m_clockOverrideMs = 12999;
    m_runner->onDeviceAdded(902, pad + QStringLiteral(" motion"));

    QTRY_VERIFY_WITH_TIMEOUT(attached(2, 901), 2000);
    QVERIFY(!attached(2, 902));

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadSingleStreamerFallback()
{
    const QString pad = QStringLiteral("Sunshine X-Box One (virtual) pad");
    QVERIFY(startSessionWithDevices({1}, {makeInputDevice(901, pad, QStringLiteral("controller"))}));
    QVERIFY(m_runner->m_gamepadClaims.isEmpty());

    m_runner->onDeviceAdded(901, pad);

    QTRY_VERIFY_WITH_TIMEOUT(attached(2, 901), 2000);

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadClaimExpires()
{
    const QString pad = QStringLiteral("Sunshine X-Box One (virtual) pad");
    QVERIFY(startSessionWithDevices({0, 1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller")),
                                     makeInputDevice(902, pad, QStringLiteral("controller"))}));

    m_runner->m_clockOverrideMs = 1000;
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(0);
    m_runner->m_clockOverrideMs = 1000 + m_runner->m_claimTtlMs - 1;
    m_runner->onDeviceAdded(901, pad);
    QTRY_VERIFY_WITH_TIMEOUT(attached(1, 901), 2000);

    m_runner->m_clockOverrideMs = 20000;
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);
    m_runner->m_clockOverrideMs = 20000 + m_runner->m_claimTtlMs + 1;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot attribute Moonlight gamepad")));
    m_runner->onDeviceAdded(902, pad);
    QTest::qWait(150);
    QVERIFY(!attached(1, 902));
    QVERIFY(!attached(2, 902));
    QVERIFY(m_runner->m_gamepadClaims.isEmpty());

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineKeyboardMouseNotAttached()
{
    QVERIFY(startSessionWithDevices({1},
                                    {makeInputDevice(901, QStringLiteral("Sunshine Keyboard"), QStringLiteral("keyboard")),
                                     makeInputDevice(902, QStringLiteral("Sunshine Mouse"), QStringLiteral("mouse")),
                                     makeInputDevice(903, QStringLiteral("Logitech Gamepad"), QStringLiteral("controller"))}));
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);

    m_runner->onDeviceAdded(901, QStringLiteral("Sunshine Keyboard"));
    m_runner->onDeviceAdded(902, QStringLiteral("Sunshine Mouse"));
    m_runner->onDeviceAdded(903, QStringLiteral("Logitech Gamepad"));

    QTest::qWait(150);
    QVERIFY(attachLines().isEmpty());
    QCOMPARE(m_runner->m_gamepadClaims.size(), 1); // claim not consumed

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadUnownedWithoutClaim()
{
    const QString pad = QStringLiteral("Sunshine PS5 (virtual) pad");
    QVERIFY(startSessionWithDevices({0, 1}, {makeInputDevice(901, pad, QStringLiteral("controller"))}));

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot attribute Moonlight gamepad")));
    m_runner->onDeviceAdded(901, pad);

    QTest::qWait(150);
    QVERIFY(attachLines().isEmpty());

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testParseRenderResolution()
{
    QCOMPARE(SessionRunner::parseRenderResolution(QStringLiteral("1280x720")), QSize(1280, 720));
    QCOMPARE(SessionRunner::parseRenderResolution(QStringLiteral(" 2560x1440 ")), QSize(2560, 1440));
    // Empty means "match the window".
    QVERIFY(!SessionRunner::parseRenderResolution(QString()).isValid());
    QVERIFY(!SessionRunner::parseRenderResolution(QStringLiteral("1280")).isValid());
    QVERIFY(!SessionRunner::parseRenderResolution(QStringLiteral("1280x")).isValid());
    QVERIFY(!SessionRunner::parseRenderResolution(QStringLiteral("axb")).isValid());
    QVERIFY(!SessionRunner::parseRenderResolution(QStringLiteral("1280x720x60")).isValid());
    // Out-of-range values must not reach gamescope.
    QVERIFY(!SessionRunner::parseRenderResolution(QStringLiteral("0x0")).isValid());
    QVERIFY(!SessionRunner::parseRenderResolution(QStringLiteral("-1280x720")).isValid());
    QVERIFY(!SessionRunner::parseRenderResolution(QStringLiteral("99999x720")).isValid());
}

QTEST_MAIN(TestSessionRunner)
#include "test_sessionrunner.moc"
