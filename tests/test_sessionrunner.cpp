// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <fcntl.h>
#include <functional>
#include <pwd.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <unistd.h>

#define private public
#include "DeviceManager.h"
#undef private
#define private public
#include "WindowManager.h"
#include "SessionRunner.h"
#include "PresetManager.h"
#undef private
#include "HeroicConfigManager.h"
#include "SessionManager.h"
#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"
#include "../helper/DirectoryPlan.h"
#include "../helper/SecureFs.h"
#define private public
#include "CouchPlayHelperClient.h"

#define private public
#include "GamescopeInstance.h"
#undef private
#include "DeviceManager.h"
#include "StreamManager.h"
#include "SunshineConfig.h"
#include "VirtualDisplayManager.h"
#include "UserLookup.h"

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

class MockCouchPlayHelperClient : public CouchPlayHelperClient
{
    Q_OBJECT

public:
    using CouchPlayHelperClient::CouchPlayHelperClient;

    struct AclCall {
        QString path;
        QString username;
    };

    struct CopyDirCall {
        QString username;
        QString sourceDir;
        QString targetRelativePath;
    };
    struct MirrorCall {
        QString username;
        QString sourceDir;
        QString targetRelativePath;
    };
    struct DeviceOwnerCall { QString path; int uid; };
    QList<AclCall> aclCalls;
    QList<DeviceOwnerCall> deviceOwnerCalls;
    QList<CopyDirCall> copyDirCalls;
    QList<MirrorCall> mirrorCalls;
    bool rejectMissingAclPaths = false;
    bool rejectUnsupportedAclPaths = false;

    explicit MockCouchPlayHelperClient(QObject *parent = nullptr)
        : CouchPlayHelperClient(parent)
    {
        m_available = true;
    }

    QList<QStringList> launchCommands;
    QList<QStringList> launchEnvironments;
    QStringList launchUsernames;
    QList<QVariantList> launchPreparations;
    QList<qint64> stopPids;
    QList<qint64> killPids;
    QStringList copiedFileUsers;
    bool autoReady = true;
    bool launchFails = false;
    bool stopSucceeds = true;
    bool killSucceeds = true;
    std::function<void(qint64)> onStopInstance;
    qint64 nextPid = 1000;
    qint64 launchInstance(const QString &username,
                          const QStringList &gamescopeArgs,
                          const QStringList &gameCommand,
                          const QString &workingDirectory,
                          const QStringList &sharedRoots,
                          const QStringList &environment,
                          bool privateTemporaryDirectories,
                          const QVariantList &directoryPreparations) override
    {
        Q_UNUSED(gamescopeArgs)
        Q_UNUSED(workingDirectory)
        Q_UNUSED(sharedRoots)
        launchEnvironments.append(environment);
        Q_UNUSED(privateTemporaryDirectories)
        launchCommands.append(gameCommand);
        launchUsernames.append(username);
        launchPreparations.append(directoryPreparations);
        if (launchFails) {
            Q_EMIT errorOccurred(QStringLiteral("simulated Gamescope launch failure"));
            return 0;
        }
        const qint64 pid = nextPid++;
        if (autoReady) Q_EMIT instanceReady(username, pid);
        return pid;
    }

    bool stopInstance(qint64 pid) override
    {
        stopPids.append(pid);
        if (onStopInstance) onStopInstance(pid);
        return stopSucceeds;
    }

    bool killInstance(qint64 pid) override
    {
        killPids.append(pid);
        return killSucceeds;
    }

    bool setPathAclWithParents(const QString &path, const QString &username) override
    {
        aclCalls.append({path, username});
        const bool pathExists = !rejectMissingAclPaths || QDir(path).exists();
        const bool rootAllowed = !rejectUnsupportedAclPaths || (path != QStringLiteral("/usr")
                                                                  && !path.startsWith(QStringLiteral("/usr/")));
        return pathExists && rootAllowed;
    }


    bool copyDirectoryToUser(const QString &username, const QString &sourceDir, const QString &targetRelativePath) override
    {
        copyDirCalls.append({username, sourceDir, targetRelativePath});
        return true;
    }

    bool mirrorDirectoryContents(const QString &username, const QString &sourceDir, const QString &targetRelativePath) override
    {
        mirrorCalls.append({username, sourceDir, targetRelativePath});
        return true;
    }
    bool copyFileToUser(const QString &, const QString &, const QString &username) override
    {
        copiedFileUsers.append(username);
        return true;
    }

    bool setDeviceOwner(const QString &devicePath, int uid) override
    {
        deviceOwnerCalls.append({devicePath, uid});
        return true;
    }
    QVariantMap getUserInfo(const QString &username) override
    {
        QVariantMap info;
        if (username == QStringLiteral("player1")) {
            info.insert(QStringLiteral("uid"), 1001u);
            info.insert(QStringLiteral("gid"), 1001u);
            info.insert(QStringLiteral("home"), player1Home);
        } else if (username == QStringLiteral("player2")) {
            info.insert(QStringLiteral("uid"), 1002u);
            info.insert(QStringLiteral("gid"), 1002u);
            info.insert(QStringLiteral("home"), player2Home);
        }
        return info;
    }

    QString player1Home = QStringLiteral("/home/player1");
    QString player2Home = QStringLiteral("/home/player2");

    // Mirrors the process-local getpwuid view by default so existing slug
    // expectations stay identical; tests can override to simulate the
    // helper-resolved host home
    QString uidHomeOverride;
    QString canonicalHomeOverride;
    QVariantMap getUserHomeInfoByUid(uint uid) override
    {
        struct passwd *pw = getpwuid(uid);
        const QString home = !uidHomeOverride.isEmpty() ? uidHomeOverride
            : pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
        if (home.isEmpty()) {
            return {};
        }
        QString canonical = canonicalHomeOverride.isEmpty()
            ? QFileInfo(home).canonicalFilePath() : canonicalHomeOverride;
        if (canonical.isEmpty()) {
            canonical = home;
        }
        return {{QStringLiteral("home"), home}, {QStringLiteral("canonicalHome"), canonical}};
    }

    // When set, "player2" looks like a user whose Steam has never been started.
    bool player2SteamNeverRun = false;
    QString getUserSteamId(const QString &username) override
    {
        return username == QStringLiteral("player1") ? QStringLiteral("12345")
             : username == QStringLiteral("player2") && !player2SteamNeverRun ? QStringLiteral("12346") : QString();
    }
    bool isSteamBootstrapped(const QString &username) override
    {
        return username == QStringLiteral("player1") || (username == QStringLiteral("player2") && !player2SteamNeverRun);
    }
    QString player1SteamRoot;
    QString player2SteamRoot;
    QString getUserSteamRoot(const QString &username) override
    {
        return username == QStringLiteral("player1") ? player1SteamRoot
             : username == QStringLiteral("player2") ? player2SteamRoot : QString();
    }

    bool writeFileToUser(const QByteArray &content, const QString &targetPath, const QString &username) override
    {
        Q_UNUSED(username)
        if (!QDir().mkpath(QFileInfo(targetPath).absolutePath())) {
            return false;
        }
        QFile f(targetPath);
        if (!f.open(QIODevice::WriteOnly)) {
            return false;
        }
        f.write(content);
        return true;
    }


    bool isInCouchPlayGroup(const QString &username) override
    {
        return username == QStringLiteral("player1") || username == QStringLiteral("player2");
    }
};

class TestSessionRunner : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Steam config tests
    void testSetupSteamConfigWithSteamLauncher();
    void testSetupSteamConfigWithNonSteamLauncher();
    void testSetupSteamConfigSteamIntegrationDisabled();
    void testHeroicStaleAndUnsupportedPathsDoNotAbortSetup();
    void testSetupSteamConfigAppliesHeroicAcls();
    void testSetupResourcesSyncsSelectedSteamAccount();
    void testStartSessionHeroicPresetUsesAclsAndSharedConfig();
    void testSteamNativeSessionRefreshesWithoutShortcutSync();
    void testOptionalShortcutSyncSkippedForUnbootstrappedPlayer();
    void testSetupDataDirectoriesUseApplicationSettings();
    void testSetupDataDirectoriesFallsBackToPresetDirs();
    void testHeroicPresetAclDirectoriesBecomeBootstrapPreparationsWithoutInstanceSnapshot();
    void testAutomaticHeroicGameRootsCreateAclPreparation();
    void testSetupDataDirectoriesFollowCurrentPreset();
    void testParseRenderResolution();
    void testNativeApplicationAccessDoesNotMutateApplicationSettings();
    void testNativeApplicationAclUsesConfiguredRootCoverage();
    void testSetupDataDirectoriesCreatesBindPlan();
    void testCopyDataDirectoryUsesCanonicalHome();
    void testDataDirectorySetupRejectsMissingHostHome();
    void testSetupDataDirectoriesBindPlanPreservesDelimiterPaths();
    void testSetupDataDirectoriesMirrorsStagedData();
    void testSetupDataDirectoriesLibrarySharingGate();
    void testSetupDataDirectoriesSecondaryLibrariesPrepared();
    void testSetupDataDirectoriesHeroicNoConfigBulkCopy();
    void testResolveUserIdentityViaHelper();
    void testResolveUserIdentityFallback();
    void testNaturalExitTearsDownSharingState();
    void testFinalizeDataDirResolvesIdentityViaHelper();
    void testResolveCompositorHomeViaHelper();
    void testPreSessionFailurePreventsLaunch();
    void testPreHookUsesStartingProfileSnapshot();
    void testPreHookUsesCapturedApplicationRevision();
    void testReadinessAdvancesSequentialStartup();
    void testPhysicalInstanceAudioDeviceRoutesOnlyThatPlayer();
    void testPreparationFailureAbortsStartup();
    void testCancelWhilePreparing();
    void testStopFailureRetainsPreparingInstanceForRetry();
    void testStopDefersCleanupUntilStopReturns_data();
    void testStopDefersCleanupUntilStopReturns();
    void testStoppedInstanceCancelsItsPositionListener();
    void testSynchronousLaunchFailureRunsPostHookOnce();
    void testVirtualDisplayFailureAbortsStartup();
    void testStreamingStartupWaitsForSunshineReady();
    void testStreamingInstancesGetDistinctSunshinePorts();
    void testStreamingLaunchFailureAbortsStartup();
    void testValidateDisplaysMonitorNotConnected();
    void testValidateDisplaysSharedMonitor();
    void testValidateDisplaysMonitorIsMainScreen();
    void testValidateDisplaysStreamWithMain();
    void testValidateDisplaysValidProfile();
    void testStartRejectsInvalidDisplay();
    void testValidatePairingGate();
    void testValidatePairingSkipsUnpairedNonStreamingAndNoUser();
    void testStartRejectsUnpairedStreamingPlayer();
    void testStartSkipPairingCheck();
    void testStartAllowsPairedStreamingPlayer();
    void testDeviceReconnectIgnoredDuringPostHook();
    void testDeviceReconnectIgnoredDuringPreHook();
    void testSunshineGamepadClaimsAreFifo();
    void testSunshineGamepadUniqGroupsDevices();
    void testSunshineGamepadSingleStreamerFallback();
    void testSunshineGamepadClaimExpires();
    void testSunshineKeyboardMouseNotChowned();
    void testSunshineGamepadUnownedWithoutClaim();
    void testInvalidPostSessionReportsError();
    void testPostSessionRunsOnceAfterStop();

private:
    void createMockHeroicConfig(const QString &basePath);
    void createMockLegendaryConfig(const QString &basePath);

    QByteArray m_originalHome;
    SessionRunner *m_runner = nullptr;
    SessionManager *m_sessionManager = nullptr;
    PresetManager *m_presetManager = nullptr;
    SteamConfigManager *m_steamConfigManager = nullptr;
    MockCouchPlayHelperClient *m_helperClient = nullptr;
};

void TestSessionRunner::initTestCase()
{
    m_originalHome = qgetenv("HOME");
}

void TestSessionRunner::cleanupTestCase()
{
    if (!m_originalHome.isEmpty()) {
        qputenv("HOME", m_originalHome);
    }
}

void TestSessionRunner::init()
{
    m_sessionManager = new SessionManager(this);
    m_presetManager = new PresetManager(this);
    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {}));
    m_steamConfigManager = new SteamConfigManager(this);
    m_helperClient = new MockCouchPlayHelperClient(this);

    m_runner = new SessionRunner(this);

    m_runner->setSessionManager(m_sessionManager);
    m_runner->setHelperClient(m_helperClient);
    m_runner->setSteamConfigManager(m_steamConfigManager);
    m_runner->setPresetManager(m_presetManager);
}

void TestSessionRunner::cleanup()
{
    delete m_runner;
    m_runner = nullptr;
    delete m_sessionManager;
    m_sessionManager = nullptr;
    delete m_helperClient;
    m_helperClient = nullptr;
    delete m_steamConfigManager;
    m_steamConfigManager = nullptr;
    delete m_presetManager;
    m_presetManager = nullptr;
}

void TestSessionRunner::createMockHeroicConfig(const QString &basePath)
{
    QString heroicRoot = basePath + QStringLiteral("/.config/heroic");

    QDir().mkpath(heroicRoot + QStringLiteral("/GamesConfig"));

    QFile configFile(heroicRoot + QStringLiteral("/config.json"));
    if (configFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;
        QJsonObject defaultSettings;
        defaultSettings[QStringLiteral("defaultInstallPath")] = QString(basePath + QStringLiteral("/Games/Heroic"));
        root[QStringLiteral("defaultSettings")] = defaultSettings;
        configFile.write(QJsonDocument(root).toJson());
        configFile.close();
    }

    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic"));
}

void TestSessionRunner::createMockLegendaryConfig(const QString &basePath)
{
    QDir().mkpath(basePath + QStringLiteral("/.config/legendary"));
    QFile legendaryFile(basePath + QStringLiteral("/.config/legendary/installed.json"));
    if (legendaryFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;
        QJsonObject game;
        game[QStringLiteral("title")] = QStringLiteral("Test Game Epic");
        game[QStringLiteral("install_path")] = QString(basePath + QStringLiteral("/Games/Heroic/EpicGame"));
        game[QStringLiteral("executable")] = QStringLiteral("Binaries/Win64/Game.exe");
        game[QStringLiteral("install_size")] = 1024;
        root[QStringLiteral("EpicGameApp")] = game;
        legendaryFile.write(QJsonDocument(root).toJson());
        legendaryFile.close();
    }
    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic/EpicGame"));
    const QString executableDir = basePath + QStringLiteral("/Games/Heroic/EpicGame/Binaries/Win64");
    QDir().mkpath(executableDir);
    QFile executable(executableDir + QStringLiteral("/Game.exe"));
    if (executable.open(QIODevice::WriteOnly)) {
        executable.write("windows-game");
        executable.close();
    }
}

void TestSessionRunner::testSetupSteamConfigWithSteamLauncher()
{
    LaunchPreset steamPreset = m_presetManager->getPreset(QStringLiteral("steam"));

    QVERIFY(steamPreset.launcherId == QStringLiteral("steam"));

    bool needsSteamSync = steamPreset.launcherId == QStringLiteral("steam");
    QVERIFY(needsSteamSync);
}

void TestSessionRunner::testSetupSteamConfigWithNonSteamLauncher()
{
    LaunchPreset heroicPreset = m_presetManager->getPreset(QStringLiteral("heroic"));

    QVERIFY(heroicPreset.launcherId == QStringLiteral("heroic"));

    bool needsSteamSync = heroicPreset.launcherId == QStringLiteral("steam");
    QVERIFY(!needsSteamSync);
}

void TestSessionRunner::testSetupSteamConfigSteamIntegrationDisabled()
{
    m_steamConfigManager->setSyncShortcutsEnabled(false);

    QVERIFY(!m_steamConfigManager->syncShortcutsEnabled());
}
void TestSessionRunner::testSetupResourcesSyncsSelectedSteamAccount()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    TestEnvironmentGuard configGuard("XDG_CONFIG_HOME");
    qputenv("HOME", home.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

    const QString rootA = home.path() + QStringLiteral("/.local/share/Steam");
    const QString rootB = home.path() + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam");
    const QString accountA = rootA + QStringLiteral("/userdata/111/config/shortcuts.vdf");
    const QString accountB = rootB + QStringLiteral("/userdata/222/config/shortcuts.vdf");
    QVERIFY(QDir().mkpath(QFileInfo(accountA).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(accountB).absolutePath()));

    SteamShortcut shortcutA;
    shortcutA.appId = 31;
    shortcutA.appName = QStringLiteral("Wrong Source Game");
    shortcutA.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('a'));
    SteamShortcut shortcutB;
    shortcutB.appId = 47;
    shortcutB.appName = QStringLiteral("Selected Source Game");
    shortcutB.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('b'));
    QByteArray dataA;
    QByteArray dataB;
    QString vdfError;
    QVERIFY(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), shortcutA, &dataA, &vdfError));
    QVERIFY(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), shortcutB, &dataB, &vdfError));
    QFile fileA(accountA);
    QVERIFY(fileA.open(QIODevice::WriteOnly));
    QCOMPARE(fileA.write(dataA), qint64(dataA.size()));
    fileA.close();
    QFile fileB(accountB);
    QVERIFY(fileB.open(QIODevice::WriteOnly));
    QCOMPARE(fileB.write(dataB), qint64(dataB.size()));
    fileB.close();

    m_presetManager->setSteamConfigManager(nullptr);
    m_runner->setSteamConfigManager(nullptr);
    delete m_steamConfigManager;
    m_steamConfigManager = new SteamConfigManager(this);
    m_steamConfigManager->setHelperClient(m_helperClient);
    m_presetManager->setSteamConfigManager(m_steamConfigManager);
    m_runner->setSteamConfigManager(m_steamConfigManager);
    m_helperClient->player1SteamRoot = home.path() + QStringLiteral("/players/player1/.local/share/Steam");

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));
    const QVariantMap selection{{QStringLiteral("launcherId"), QStringLiteral("steam")},
                                {QStringLiteral("backend"), QStringLiteral("shortcut")},
                                {QStringLiteral("gameId"), QString::number((quint64(47) << 32) | 0x02000000ULL)},
                                {QStringLiteral("title"), QStringLiteral("Selected Source Game")}};
    m_sessionManager->setInstanceGame(0, selection);

    QVERIFY(!m_runner->start());
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_helperClient->aclCalls.isEmpty());
    QVERIFY(m_helperClient->launchCommands.isEmpty());
    QVERIFY(m_steamConfigManager->selectSourceAccount(QFileInfo(rootB).canonicalFilePath(), QStringLiteral("222")));

    QVariantMap wrongSourceSelection = selection;
    wrongSourceSelection[QStringLiteral("gameId")] = QString::number((quint64(31) << 32) | 0x02000000ULL);
    wrongSourceSelection[QStringLiteral("title")] = QStringLiteral("Wrong Source Game");
    m_sessionManager->setInstanceGame(0, wrongSourceSelection);
    QVERIFY(!m_runner->start());
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_helperClient->aclCalls.isEmpty());
    QVERIFY(m_helperClient->launchCommands.isEmpty());
    m_sessionManager->setInstanceGame(0, selection);

    QVERIFY(m_runner->setupSessionResources());
    QFile playerShortcuts(m_helperClient->player1SteamRoot
                          + QStringLiteral("/userdata/12345/config/shortcuts.vdf"));
    QVERIFY(playerShortcuts.open(QIODevice::ReadOnly));
    QCOMPARE(playerShortcuts.readAll(), dataB);
    QFile untouchedSource(accountA);
    QVERIFY(untouchedSource.open(QIODevice::ReadOnly));
    QCOMPARE(untouchedSource.readAll(), dataA);
}
void TestSessionRunner::testOptionalShortcutSyncSkippedForUnbootstrappedPlayer()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    TestEnvironmentGuard configGuard("XDG_CONFIG_HOME");
    qputenv("HOME", home.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

    const QString shortcutsPath = home.path() + QStringLiteral("/.local/share/Steam/userdata/123/config/shortcuts.vdf");
    QVERIFY(QDir().mkpath(QFileInfo(shortcutsPath).absolutePath()));
    QFile shortcuts(shortcutsPath);
    QVERIFY(shortcuts.open(QIODevice::WriteOnly));
    QVERIFY(shortcuts.write(SteamShortcutsVdf::emptyDocument()) > 0);
    shortcuts.close();

    m_presetManager->setSteamConfigManager(nullptr);
    m_runner->setSteamConfigManager(nullptr);
    delete m_steamConfigManager;
    m_steamConfigManager = new SteamConfigManager(this);
    m_steamConfigManager->setHelperClient(m_helperClient);
    m_presetManager->setSteamConfigManager(m_steamConfigManager);
    m_runner->setSteamConfigManager(m_steamConfigManager);
    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {QStringLiteral("steam")}));
    m_steamConfigManager->setSyncShortcutsEnabled(true);

    m_helperClient->player2SteamNeverRun = true;
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));

    QSignalSpy failed(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy errors(m_runner, &SessionRunner::errorOccurred);
    QVERIFY2(m_runner->start(), qPrintable(errors.value(0).value(0).toString()));
    QVERIFY(failed.isEmpty());
    m_runner->stop();
}
void TestSessionRunner::testSteamNativeSessionRefreshesWithoutShortcutSync()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    TestEnvironmentGuard configGuard("XDG_CONFIG_HOME");
    qputenv("HOME", home.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

    const QString root = home.path() + QStringLiteral("/.local/share/Steam");
    const QString shortcutsPath = root + QStringLiteral("/userdata/123/config/shortcuts.vdf");
    QVERIFY(QDir().mkpath(QFileInfo(shortcutsPath).absolutePath()));
    auto writeShortcut = [&](const QString &title) {
        SteamShortcut shortcut;
        shortcut.appId = 47;
        shortcut.appName = title;
        shortcut.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('c'));
        QByteArray bytes;
        QString error;
        if (!SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), shortcut, &bytes, &error)) {
            qWarning() << error;
            return false;
        }
        QFile file(shortcutsPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false;
        return true;
    };
    QVERIFY(writeShortcut(QStringLiteral("Before Refresh")));

    m_presetManager->setSteamConfigManager(nullptr);
    m_runner->setSteamConfigManager(nullptr);
    delete m_steamConfigManager;
    m_steamConfigManager = new SteamConfigManager(this);
    m_presetManager->setSteamConfigManager(m_steamConfigManager);
    m_runner->setSteamConfigManager(m_steamConfigManager);
    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {QStringLiteral("steam")}));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));

    m_steamConfigManager->loadGames();
    QCOMPARE(m_steamConfigManager->gamesAsVariant().constFirst().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Before Refresh"));
    QVERIFY(writeShortcut(QStringLiteral("After Refresh")));

    QVERIFY(!m_steamConfigManager->syncShortcutsEnabled());
    QVERIFY(m_runner->start());
    QCOMPARE(m_steamConfigManager->gamesAsVariant().constFirst().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("After Refresh"));
    m_runner->stop();
}


void TestSessionRunner::testSetupSteamConfigAppliesHeroicAcls()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    createMockHeroicConfig(homeDir.path());
    createMockLegendaryConfig(homeDir.path());

    HeroicConfigManager heroicManager;
    m_presetManager->setHeroicConfigManager(&heroicManager);

    LaunchPreset heroicPreset = m_presetManager->getPreset(QStringLiteral("heroic"));
    QVERIFY(heroicPreset.launcherId == QStringLiteral("heroic"));

    QList<DataDirectory> dataDirs = heroicPreset.dataDirectories;

    QString expectedGamePath = homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
    bool foundAclGameDir = false;
    for (const DataDirectory &dir : dataDirs) {
        if (dir.mode == QStringLiteral("acl") && dir.path == expectedGamePath) {
            foundAclGameDir = true;
            break;
        }
    }
    QVERIFY2(foundAclGameDir, "Heroic preset should contain acl-mode DataDirectory for game path");

    // Config sync is dispatched via syncConfigToUser at session start, not via
    // a copy-mode DataDirectory that would bulk-copy the whole config root
    bool foundCopyConfigDir = false;
    for (const DataDirectory &dir : dataDirs) {
        if (dir.mode == QStringLiteral("copy") && dir.path.contains(QStringLiteral("heroic"))) {
            foundCopyConfigDir = true;
            break;
        }
    }
    QVERIFY2(!foundCopyConfigDir, "Heroic preset must not carry copy-mode DataDirectory for the config path");
}

void TestSessionRunner::testStartSessionHeroicPresetUsesAclsAndSharedConfig()
{
    QSKIP("Requires D-Bus (m_runner->start()).");
}

void TestSessionRunner::testSetupDataDirectoriesUseApplicationSettings()
{
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Instance Dirs Game"),
                                                        QStringLiteral("/usr/bin/game"));
    QVariantMap presetDir;
    presetDir[QStringLiteral("path")] = QStringLiteral("/preset/dir");
    presetDir[QStringLiteral("mode")] = QStringLiteral("acl");
    QVariantList presetDirs;
    presetDirs.append(presetDir);
    QVERIFY(m_presetManager->setDataDirectories(presetId, presetDirs));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("Application Directories")));
    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("Application Directories")));

    QVERIFY(m_runner->setupSessionResources());

    QCOMPARE(m_helperClient->aclCalls.size(), 1);
    QCOMPARE(m_helperClient->aclCalls.constLast().path, QStringLiteral("/preset/dir"));
    const QVariantList firstPlan = m_runner->m_instanceDirectoryPreparations.value(0);
    QCOMPARE(firstPlan.size(), 1);
    QCOMPARE(firstPlan.constFirst().toMap().value(QStringLiteral("source")).toString(), QStringLiteral("/preset/dir"));
    QCOMPARE(firstPlan.constFirst().toMap().value(QStringLiteral("mode")).toString(), QStringLiteral("acl"));

    presetDir[QStringLiteral("path")] = QStringLiteral("/preset/dir-updated");
    QVERIFY(m_presetManager->setDataDirectories(presetId, {presetDir}));
    QVERIFY(m_runner->setupSessionResources());
    QCOMPARE(m_helperClient->aclCalls.size(), 2);
    QCOMPARE(m_helperClient->aclCalls.constLast().path, QStringLiteral("/preset/dir-updated"));
    const QVariantList updatedPlan = m_runner->m_instanceDirectoryPreparations.value(0);
    QCOMPARE(updatedPlan.size(), 1);
    QCOMPARE(updatedPlan.constFirst().toMap().value(QStringLiteral("source")).toString(), QStringLiteral("/preset/dir-updated"));
    QCOMPARE(updatedPlan.constFirst().toMap().value(QStringLiteral("mode")).toString(), QStringLiteral("acl"));
}
void TestSessionRunner::testSetupDataDirectoriesFallsBackToPresetDirs()
{
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Fallback Dirs Game"),
                                                        QStringLiteral("/usr/bin/game2"));

    QVariantMap presetDir;
    presetDir[QStringLiteral("path")] = QStringLiteral("/preset/dir");
    presetDir[QStringLiteral("mode")] = QStringLiteral("acl");
    QVariantList presetDirs;
    presetDirs.append(presetDir);
    QVERIFY(m_presetManager->setDataDirectories(presetId, presetDirs));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    // No instance directories set — must fall back to the preset's defaults

    QVERIFY(m_runner->setupSessionResources());

    QCOMPARE(m_helperClient->aclCalls.size(), 1);
    QCOMPARE(m_helperClient->aclCalls[0].path, QStringLiteral("/preset/dir"));
    QCOMPARE(m_helperClient->aclCalls[0].username, QStringLiteral("player1"));
}

void TestSessionRunner::testHeroicPresetAclDirectoriesBecomeBootstrapPreparationsWithoutInstanceSnapshot()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    qputenv("HOME", homeDir.path().toLocal8Bit());
    createMockHeroicConfig(homeDir.path());
    createMockLegendaryConfig(homeDir.path());

    HeroicConfigManager heroicManager;
    QVERIFY(heroicManager.isHeroicDetected());
    heroicManager.setHelperClient(m_helperClient);
    m_presetManager->setHeroicConfigManager(&heroicManager);
    m_runner->setHeroicConfigManager(&heroicManager);

    const QString gamePath = homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
    QVERIFY(QDir(gamePath).exists());
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Heroic custom preset"),
                                                               QStringLiteral("/usr/bin/true"));
    QVERIFY(!presetId.isEmpty());
    QVERIFY(m_presetManager->setRequiredIntegrations(presetId, {QStringLiteral("heroic")}));
    QVariantMap presetAcl;
    presetAcl.insert(QStringLiteral("path"), gamePath);
    presetAcl.insert(QStringLiteral("mode"), QStringLiteral("acl"));
    QVERIFY(m_presetManager->setDataDirectories(presetId, {presetAcl}));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    // No instance-level directory snapshot: setup falls back to this explicit preset list.
    QVERIFY(m_runner->setupSessionResources());

    bool aclPreparationFound = false;
    for (const QVariant &entry : m_runner->m_instanceDirectoryPreparations.value(0)) {
        const QVariantMap plan = entry.toMap();
        aclPreparationFound |= plan.value(QStringLiteral("source")).toString() == gamePath
            && plan.value(QStringLiteral("mode")).toString() == QStringLiteral("acl");
    }
    QVERIFY(aclPreparationFound);
}
void TestSessionRunner::testAutomaticHeroicGameRootsCreateAclPreparation()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    qputenv("HOME", homeDir.path().toLocal8Bit());
    createMockHeroicConfig(homeDir.path());
    createMockLegendaryConfig(homeDir.path());

    HeroicConfigManager heroicManager;
    QVERIFY(heroicManager.isHeroicDetected());
    heroicManager.setHelperClient(m_helperClient);
    m_presetManager->setHeroicConfigManager(&heroicManager);
    m_runner->setHeroicConfigManager(&heroicManager);

    const QString gamePath = homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
    QFile privateExecutable(gamePath + QStringLiteral("/Binaries/Win64/Game.exe"));
    QVERIFY(privateExecutable.exists());
    QVERIFY(privateExecutable.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("heroic"));
    QVERIFY(m_runner->setupSessionResources());

    bool aclPreparationFound = false;
    for (const QVariant &entry : m_runner->m_instanceDirectoryPreparations.value(0)) {
        const QVariantMap plan = entry.toMap();
        aclPreparationFound |= plan.value(QStringLiteral("source")).toString() == gamePath
            && plan.value(QStringLiteral("mode")).toString() == QStringLiteral("acl");
    }
    QVERIFY2(aclPreparationFound, "Automatic Heroic roots must retain ACL bootstrap preparations");
}

void TestSessionRunner::testSetupDataDirectoriesFollowCurrentPreset()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Editable Directories"),
                                                              QStringLiteral("/usr/bin/game4"));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    const QString profileName = QStringLiteral("Directory inheritance");
    QVERIFY(m_sessionManager->saveProfile(profileName));

    const QVariantMap firstDirectory{{QStringLiteral("path"), QStringLiteral("/preset/dir")},
                                     {QStringLiteral("mode"), QStringLiteral("acl")}};
    QVERIFY(m_presetManager->setDataDirectories(presetId, {firstDirectory}));
    QVERIFY(m_sessionManager->loadProfile(profileName));
    QVERIFY(m_runner->setupSessionResources());
    QCOMPARE(m_runner->m_instanceDirectoryPreparations.value(0).constFirst().toMap()
                 .value(QStringLiteral("source")).toString(), QStringLiteral("/preset/dir"));

    const QVariantMap updatedDirectory{{QStringLiteral("path"), QStringLiteral("/preset/dir-updated")},
                                       {QStringLiteral("mode"), QStringLiteral("acl")}};
    QVERIFY(m_presetManager->setDataDirectories(presetId, {updatedDirectory}));
    QVERIFY(m_runner->setupSessionResources());
    QCOMPARE(m_runner->m_instanceDirectoryPreparations.value(0).constFirst().toMap()
                 .value(QStringLiteral("source")).toString(), QStringLiteral("/preset/dir-updated"));
}
void TestSessionRunner::testNativeApplicationAccessDoesNotMutateApplicationSettings()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_helperClient->uidHomeOverride = homeDir.path();

    const QString appDir = homeDir.path() + QStringLiteral("/.local/share/Example Launcher");
    QVERIFY(QDir().mkpath(appDir));
    const QString executablePath = appDir + QStringLiteral("/example-launcher");
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write("#!/bin/sh\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString presetId = m_presetManager->addCustomPreset(
        QStringLiteral("Example Launcher"), QStringLiteral("\"%1\"").arg(executablePath), appDir);
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    const LaunchCommand command = m_presetManager->buildLaunchCommand(presetId, {});
    QVERIFY(command.isValid());
    QCOMPARE(command.program, executablePath);
    QCOMPARE(command.workingDirectory, appDir);
    m_runner->m_startingSnapshot.launchCommands = {command, command};

    QVERIFY(m_runner->setupSessionResources());

    QVERIFY(m_presetManager->getPreset(presetId).dataDirectories.isEmpty());
    for (int index = 0; index < 2; ++index) {
        const QVariantList preparations = m_runner->m_instanceDirectoryPreparations.value(index);
        QCOMPARE(preparations.size(), 1);
        const QVariantMap acl = preparations.constFirst().toMap();
        QCOMPARE(acl.value(QStringLiteral("source")).toString(), appDir);
        QCOMPARE(acl.value(QStringLiteral("mode")).toString(), QStringLiteral("acl"));
        QVERIFY(acl.value(QStringLiteral("targetAlias")).toString().isEmpty());
        QVERIFY(acl.value(QStringLiteral("stagingPath")).toString().isEmpty());
    }
}
void TestSessionRunner::testNativeApplicationAclUsesConfiguredRootCoverage()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_helperClient->uidHomeOverride = homeDir.path();

    const QString configuredRoot = homeDir.path() + QStringLiteral("/Games/ConfiguredRoot");
    const QString applicationDirectory = configuredRoot + QStringLiteral("/bin");
    QVERIFY(QDir().mkpath(applicationDirectory));
    const QString executablePath = applicationDirectory + QStringLiteral("/game");
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write("#!/bin/sh\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString presetId = m_presetManager->addCustomPreset(
        QStringLiteral("Configured Root"), QStringLiteral("\"%1\"").arg(executablePath), applicationDirectory);
    QVERIFY(m_presetManager->setDataDirectories(presetId, {
        QVariantMap{{QStringLiteral("path"), configuredRoot}, {QStringLiteral("mode"), QStringLiteral("acl")}}
    }));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);

    const LaunchCommand command = m_presetManager->buildLaunchCommand(presetId, {});
    QVERIFY(command.isValid());
    m_runner->m_startingSnapshot.launchCommands = {command};
    QVERIFY(m_runner->setupSessionResources());

    const QVariantList plans = m_runner->m_instanceDirectoryPreparations.value(0);
    QCOMPARE(plans.size(), 1);
    const QVariantMap acl = plans.constFirst().toMap();
    QCOMPARE(acl.value(QStringLiteral("source")).toString(), configuredRoot);
    QCOMPARE(acl.value(QStringLiteral("mode")).toString(), QStringLiteral("acl"));
}

void TestSessionRunner::testCopyDataDirectoryUsesCanonicalHome()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString canonicalHome = root.path() + QStringLiteral("/accounts/account");
    const QString declaredHome = root.path() + QStringLiteral("/declared-home");
    const QString source = canonicalHome + QStringLiteral("/.config/launcher");
    QVERIFY(QDir().mkpath(source));
    auto *helper = new MockCouchPlayHelperClient(m_runner);
    helper->uidHomeOverride = declaredHome;
    helper->canonicalHomeOverride = canonicalHome;
    helper->player1Home = root.path() + QStringLiteral("/players/player1");
    m_runner->setHelperClient(helper);
    const QString preset = m_presetManager->addCustomPreset(QStringLiteral("Home Mapping"), QStringLiteral("/usr/bin/game"));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, preset);
    QVERIFY(m_presetManager->setDataDirectories(preset, {
        QVariantMap{{QStringLiteral("path"), source}, {QStringLiteral("mode"), QStringLiteral("copy")}}
    }));

    QVERIFY(m_runner->setupSessionResources());
    const QVariantList preparations = m_runner->m_instanceDirectoryPreparations.value(0);
    QCOMPARE(preparations.size(), 1);
    const QVariantMap copy = preparations.constFirst().toMap();
    QCOMPARE(copy.value(QStringLiteral("source")).toString(), source);
    QCOMPARE(copy.value(QStringLiteral("mode")).toString(), QStringLiteral("copy"));
    QCOMPARE(copy.value(QStringLiteral("targetAlias")).toString(), QStringLiteral(".config/launcher"));
    QCOMPARE(helper->copyDirCalls.size(), 0);
}

void TestSessionRunner::testDataDirectorySetupRejectsMissingHostHome()
{
    class MissingHomeHelper : public MockCouchPlayHelperClient
    {
    public:
        using MockCouchPlayHelperClient::MockCouchPlayHelperClient;
        QVariantMap getUserHomeInfoByUid(uint) override
        {
            return {{QStringLiteral("home"), QStringLiteral("/home/account")}};
        }
    };
    auto *helper = new MissingHomeHelper(m_runner);
    m_runner->setHelperClient(helper);
    QSignalSpy errors(m_runner, &SessionRunner::errorOccurred);
    QVERIFY(!m_runner->setupSessionResources());
    QCOMPARE(errors.count(), 1);
}

void TestSessionRunner::testSetupDataDirectoriesCreatesBindPlan()
{
    // Pin the host home used by the production mapper so the declared-home
    // source is tested as a home-relative target on every test host.
    m_helperClient->uidHomeOverride = QStringLiteral("/home/compositor");
    m_helperClient->canonicalHomeOverride = QStringLiteral("/home/compositor");
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Bind Game"), QStringLiteral("/usr/bin/game3"));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);

    QVariantMap bindDir;
    bindDir[QStringLiteral("path")] = QStringLiteral("/home/compositor/.config/game");
    bindDir[QStringLiteral("mode")] = QStringLiteral("bind");
    QVariantList dirs;
    dirs.append(bindDir);
    QVERIFY(m_presetManager->setDataDirectories(presetId, dirs));

    QVERIFY(m_runner->setupSessionResources());

    const QVariantList plans = m_runner->m_instanceDirectoryPreparations.value(0);
    QCOMPARE(plans.size(), 1);
    const QVariantMap plan = plans.constFirst().toMap();
    QCOMPARE(plan.value(QStringLiteral("source")).toString(), QStringLiteral("/home/compositor/.config/game"));
    QCOMPARE(plan.value(QStringLiteral("mode")).toString(), QStringLiteral("bind"));
    QCOMPARE(plan.value(QStringLiteral("targetAlias")).toString(), QStringLiteral(".config/game"));
}

void TestSessionRunner::testSetupDataDirectoriesBindPlanPreservesDelimiterPaths()
{
    // Delimiter characters are carried as ordinary path data in the structured plan.
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Pipe Bind"), QStringLiteral("/usr/bin/game7"));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);

    QVariantMap bindDir;
    bindDir[QStringLiteral("path")] = QStringLiteral("/mnt/Game|Saves");
    bindDir[QStringLiteral("mode")] = QStringLiteral("bind");
    QVariantList dirs;
    dirs.append(bindDir);
    QVERIFY(m_presetManager->setDataDirectories(presetId, dirs));

    QVERIFY(m_runner->setupSessionResources());

    const QVariantList plans = m_runner->m_instanceDirectoryPreparations.value(0);
    QCOMPARE(plans.size(), 1);
    QCOMPARE(plans.constFirst().toMap().value(QStringLiteral("targetAlias")).toString(),
             QStringLiteral(".couchplay/mounts/mnt/Game|Saves"));
}

void TestSessionRunner::testSetupDataDirectoriesMirrorsStagedData()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    TestEnvironmentGuard flatpakGuard("FLATPAK_ID");
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("FLATPAK_ID");
    m_helperClient->uidHomeOverride = home.path();

    const QString storage = home.path() + QStringLiteral("/storage");
    const QString alias = home.path() + QStringLiteral("/.local/share/couchplay");
    QVERIFY(QDir().mkpath(storage));
    QVERIFY(QDir().mkpath(QFileInfo(alias).absolutePath()));
    QCOMPARE(::symlink(QFile::encodeName(storage).constData(), QFile::encodeName(alias).constData()), 0);
    const QString source = home.path() + QStringLiteral("/game-config");
    QVERIFY(QDir().mkpath(source));
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Staged Game"), QStringLiteral("/usr/bin/game5"));
    QVERIFY(m_presetManager->setDataDirectories(presetId, {
        QVariantMap{{QStringLiteral("path"), source}, {QStringLiteral("mode"), QStringLiteral("overlay")}}
    }));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    const QString staging = playerDataStagingRoot(presetId, QStringLiteral("player1")) + QLatin1Char('/')
        + DirectoryPlan::dataDirectoryStagingSlug(source, home.path());
    QVERIFY(QDir().mkpath(staging));
    const QByteArray contents("PLAYER-STAGING-OVERRIDE\n");
    QFile seed(staging + QStringLiteral("/settings.ini"));
    QVERIFY(seed.open(QIODevice::WriteOnly));
    QCOMPARE(seed.write(contents), qint64(contents.size()));
    seed.close();

    QVERIFY(m_runner->setupSessionResources());
    const QString providedPath = m_runner->m_instanceDirectoryPreparations.value(0).constFirst()
                                     .toMap().value(QStringLiteral("stagingPath")).toString();
    // Exercise the real helper consumer: the emitted path must be walkable
    // without following links and must expose player-specific staged contents.
    const int directoryFd = SecureFs::openExistingDirNoFollow(providedPath);
    QVERIFY2(directoryFd >= 0, qPrintable(QStringLiteral("Cannot securely open staging %1: %2")
                                             .arg(providedPath).arg(directoryFd)));
    const int fileFd = ::openat(directoryFd, "settings.ini", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ::close(directoryFd);
    QVERIFY(fileFd >= 0);
    char buffer[64];
    const ssize_t bytes = ::read(fileFd, buffer, sizeof(buffer));
    ::close(fileFd);
    QVERIFY(bytes >= 0);
    QCOMPARE(QByteArray(buffer, bytes), contents);
}

void TestSessionRunner::testSetupDataDirectoriesLibrarySharingGate()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    TestEnvironmentGuard configGuard("XDG_CONFIG_HOME");
    qputenv("HOME", homeDir.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (homeDir.path() + QStringLiteral("/config")).toLocal8Bit());

    // Mock a detected Steam installation
    QString steamRoot = homeDir.path() + QStringLiteral("/.steam/steam");
    const QString selectedSteamRoot = homeDir.path()
        + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam");
    QVERIFY(QDir().mkpath(selectedSteamRoot + QStringLiteral("/userdata/222/config")));
    QDir().mkpath(steamRoot + QStringLiteral("/config"));
    QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
    libraryVdf.write("\"libraryfolders\"\n{\n}\n");
    libraryVdf.close();

    auto *steamManager = new SteamConfigManager(this);
    m_runner->setSteamConfigManager(steamManager);
    QVERIFY(steamManager->isSteamDetected());
    QVERIFY(steamManager->selectSourceAccount(QFileInfo(selectedSteamRoot).canonicalFilePath(), QStringLiteral("222")));
    QCOMPARE(steamManager->steamPaths().steamRoot, QFileInfo(selectedSteamRoot).canonicalFilePath());
    QVERIFY(!steamManager->shareLibraryEnabled()); // default off

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));

    QVariantMap overlayDir;
    overlayDir[QStringLiteral("path")] = steamRoot;
    overlayDir[QStringLiteral("mode")] = QStringLiteral("overlay");
    QVariantList dirs;
    dirs.append(overlayDir);
    QVERIFY(m_presetManager->setDataDirectories(QStringLiteral("steam"), dirs));
    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {QStringLiteral("steam")}));
    // Library sharing disabled: no bootstrap preparations are generated.
    m_runner->m_startingSnapshot.shareSteamLibraries = false;
    QVERIFY(m_runner->setupSessionResources());
    QVERIFY(m_runner->m_instanceDirectoryPreparations.value(0).isEmpty());

    // Enabling sharing with no parseable libraries still produces no plans
    // and never projects the compositor's Steam root into the player home.
    steamManager->setShareLibraryEnabled(true);
    m_runner->m_startingSnapshot.shareSteamLibraries = true;
    QVERIFY(!m_runner->setupSessionResources());
    QVERIFY(m_runner->m_instanceDirectoryPreparations.value(0).isEmpty());
}

void TestSessionRunner::testSetupDataDirectoriesSecondaryLibrariesPrepared()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    const QString steamRoot = homeDir.path() + QStringLiteral("/.steam/steam");
    const QString secondaryLibrary = homeDir.path() + QStringLiteral("/secondary library");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/steamapps/common")));
    QVERIFY(QDir().mkpath(secondaryLibrary + QStringLiteral("/steamapps/common")));
    QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
    libraryVdf.write("\"libraryfolders\"\n"
                     "{\n"
                     "  \"0\"\n"
                     "  {\n"
                     "    \"path\"\t\t\"" + steamRoot.toUtf8() + "\"\n"
                     "  }\n"
                     "  \"1\"\n"
                     "  {\n"
                     "    \"path\"\t\t\"" + secondaryLibrary.toUtf8() + "\"\n"
                     "  }\n"
                     "}\n");
    libraryVdf.close();

    m_helperClient->player1Home = homeDir.path() + QStringLiteral("/player1");
    QVERIFY(QDir().mkpath(m_helperClient->player1Home + QStringLiteral("/.steam/steam/config")));
    auto *steamManager = new SteamConfigManager(this);
    steamManager->setHelperClient(m_helperClient);
    m_runner->setSteamConfigManager(steamManager);
    QVERIFY(steamManager->isSteamDetected());
    steamManager->setShareLibraryEnabled(true);

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));
    QVERIFY(m_presetManager->setDataDirectories(QStringLiteral("steam"), {
        QVariantMap{{QStringLiteral("path"), steamRoot}, {QStringLiteral("mode"), QStringLiteral("overlay")}}
    }));
    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {QStringLiteral("steam")}));
    m_runner->m_startingSnapshot.shareSteamLibraries = true;
    QVERIFY(m_runner->setupSessionResources());

    const QVariantList plans = m_runner->m_instanceDirectoryPreparations.value(0);
    QCOMPARE(plans.size(), 2);
    const QVariantMap primary = plans.at(0).toMap();
    QCOMPARE(primary.value(QStringLiteral("source")).toString(), steamRoot + QStringLiteral("/steamapps/common"));
    QCOMPARE(primary.value(QStringLiteral("mode")).toString(), QStringLiteral("overlay"));
    QCOMPARE(primary.value(QStringLiteral("targetAlias")).toString(),
             QStringLiteral(".couchplay/steam-libs/0/steamapps/common"));
    const QVariantMap secondary = plans.at(1).toMap();
    QCOMPARE(secondary.value(QStringLiteral("source")).toString(), secondaryLibrary + QStringLiteral("/steamapps/common"));
    QCOMPARE(secondary.value(QStringLiteral("targetAlias")).toString(),
             QStringLiteral(".couchplay/steam-libs/1/steamapps/common"));
    QCOMPARE(m_helperClient->mirrorCalls.size(), 0);
    QCOMPARE(m_helperClient->copyDirCalls.size(), 0);
}

void TestSessionRunner::testSetupDataDirectoriesHeroicNoConfigBulkCopy()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    createMockHeroicConfig(homeDir.path());
    createMockLegendaryConfig(homeDir.path());

    HeroicConfigManager heroicManager;
    m_presetManager->setHeroicConfigManager(&heroicManager);
    m_runner->setHeroicConfigManager(&heroicManager);
    QVERIFY(heroicManager.isHeroicDetected());

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("ghostuser")); // unresolvable: sync bails early
    m_sessionManager->setInstancePreset(0, QStringLiteral("heroic"));
    // No instance dirs — falls back to the preset's resolver defaults

    // False is expected: config sync fails for a nonexistent user. What
    // matters here is the effect on the generic data-directory path.
    QVERIFY(!m_runner->setupSessionResources());

    // The config root must NOT be bulk-copied (selective sync replaces it)
    QCOMPARE(m_helperClient->copyDirCalls.size(), 0);

    // Resolver defaults include the install overlay and may include ACL-only
    // records for registered game roots.
    const QVariantList plans = m_runner->m_instanceDirectoryPreparations.value(0);
    bool foundInstallOverlay = false;
    for (const QVariant &entry : plans) {
        const QVariantMap plan = entry.toMap();
        const QString source = plan.value(QStringLiteral("source")).toString();
        const QString mode = plan.value(QStringLiteral("mode")).toString();
        foundInstallOverlay |= source == heroicManager.defaultInstallPath()
            && mode == QStringLiteral("overlay");
        QVERIFY(!(source == heroicManager.configPath() && mode == QStringLiteral("copy")));
    }
    QVERIFY(foundInstallOverlay);
    bool foundGameDirAcl = false;
    bool foundExecutableParentAcl = false;
    for (const auto &call : m_helperClient->aclCalls) {
        QCOMPARE(call.username, QStringLiteral("ghostuser"));
        foundGameDirAcl |= call.path == homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
        foundExecutableParentAcl |=
            call.path == homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame/Binaries/Win64");
    }
    QVERIFY(foundGameDirAcl);
    QVERIFY(foundExecutableParentAcl);
}

void TestSessionRunner::testHeroicStaleAndUnsupportedPathsDoNotAbortSetup()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    qputenv("HOME", homeDir.path().toLocal8Bit());

    createMockHeroicConfig(homeDir.path());
    createMockLegendaryConfig(homeDir.path());

    const QString staleGamePath = homeDir.path() + QStringLiteral("/Games/Heroic/DisconnectedGame");
    const QString staleExecutableDirectory = staleGamePath + QStringLiteral("/bin");
    const QString installedPath = homeDir.path() + QStringLiteral("/.config/legendary/installed.json");
    QFile installedFile(installedPath);
    QVERIFY(installedFile.open(QIODevice::ReadOnly));
    QJsonObject installed = QJsonDocument::fromJson(installedFile.readAll()).object();
    installedFile.close();
    QJsonObject staleGame;
    staleGame[QStringLiteral("title")] = QStringLiteral("Disconnected Game");
    staleGame[QStringLiteral("install_path")] = staleGamePath;
    staleGame[QStringLiteral("executable")] = QStringLiteral("bin/Game.exe");
    installed[QStringLiteral("DisconnectedGameApp")] = staleGame;
    QVERIFY(installedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(installedFile.write(QJsonDocument(installed).toJson()), qint64(QJsonDocument(installed).toJson().size()));
    installedFile.close();

    HeroicConfigManager heroicManager;
    QVERIFY(heroicManager.isHeroicDetected());
    const QString sideloadDirectory = heroicManager.heroicPaths().heroicRoot + QStringLiteral("/sideload_apps");
    QVERIFY(QDir().mkpath(sideloadDirectory));
    const QString sideloadLibrary = sideloadDirectory + QStringLiteral("/library.json");
    QJsonObject systemSideload;
    systemSideload.insert(QStringLiteral("app_name"), QStringLiteral("SystemSideload"));
    systemSideload.insert(QStringLiteral("title"), QStringLiteral("System Sideload"));
    systemSideload.insert(QStringLiteral("folder_name"), QString());
    systemSideload.insert(QStringLiteral("is_installed"), true);
    QJsonObject systemSideloadInstall;
    systemSideloadInstall.insert(QStringLiteral("executable"), QStringLiteral("/usr/bin/true"));
    systemSideload.insert(QStringLiteral("install"), systemSideloadInstall);
    QJsonObject sideloadLibraryData;
    sideloadLibraryData.insert(QStringLiteral("games"), QJsonArray{systemSideload});
    QFile sideloadFile(sideloadLibrary);
    QVERIFY(sideloadFile.open(QIODevice::WriteOnly));
    QCOMPARE(sideloadFile.write(QJsonDocument(sideloadLibraryData).toJson()),
             qint64(QJsonDocument(sideloadLibraryData).toJson().size()));
    sideloadFile.close();
    heroicManager.loadGames();
    bool foundSystemSideload = false;
    for (const HeroicGame &game : heroicManager.installedGames()) {
        foundSystemSideload |= game.appName == QStringLiteral("SystemSideload") && game.installPath.isEmpty()
            && game.executable == QStringLiteral("/usr/bin/true");
    }
    QVERIFY(foundSystemSideload);
    m_presetManager->setHeroicConfigManager(&heroicManager);
    heroicManager.setHelperClient(m_helperClient);
    m_runner->setHeroicConfigManager(&heroicManager);

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("heroic"));
    const QString existingGamePath = homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
    QVariantMap configuredAcl;
    configuredAcl.insert(QStringLiteral("path"), existingGamePath);
    configuredAcl.insert(QStringLiteral("mode"), QStringLiteral("acl"));
    QVERIFY(m_presetManager->setDataDirectories(QStringLiteral("heroic"), {configuredAcl}));
    m_helperClient->rejectMissingAclPaths = true;
    m_helperClient->rejectUnsupportedAclPaths = true;

    QVERIFY(m_runner->setupSessionResources());
    bool existingGameAclApplied = false;
    for (const auto &call : m_helperClient->aclCalls) {
        QVERIFY(!call.path.startsWith(QStringLiteral("/usr/")));
        QVERIFY(call.path != staleGamePath);
        QVERIFY(call.path != staleExecutableDirectory);
        existingGameAclApplied |= call.path == homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
    }
    QVERIFY(existingGameAclApplied);
    bool recursiveGamePreparation = false;
    for (const QVariant &entry : m_runner->m_instanceDirectoryPreparations.value(0)) {
        const QVariantMap plan = entry.toMap();
        recursiveGamePreparation |= plan.value(QStringLiteral("source")).toString() == existingGamePath
            && plan.value(QStringLiteral("mode")).toString() == QStringLiteral("acl");
    }
    QVERIFY(recursiveGamePreparation);
}
void TestSessionRunner::testResolveUserIdentityViaHelper()
{
    const UserIdentity id = resolveUserIdentity(QStringLiteral("player1"), m_helperClient);
    QVERIFY(id.valid);
    QCOMPARE(id.uid, 1001u);
    QCOMPARE(id.gid, 1001u);
    QCOMPARE(id.home, QStringLiteral("/home/player1"));
}

void TestSessionRunner::testResolveUserIdentityFallback()
{
    const UserIdentity id = resolveUserIdentity(QStringLiteral("root"), nullptr);
    QVERIFY(id.valid);
}

void TestSessionRunner::testNaturalExitTearsDownSharingState()
{
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Natural exit"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstancePreset(1, presetId);
    QVERIFY(m_presetManager->setDataDirectories(presetId, {
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/shared-game-data")},
                    {QStringLiteral("mode"), QStringLiteral("overlay")}}
    }));

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 2000);
    QVERIFY(m_runner->m_sharedStateActive);
    QCOMPARE(m_helperClient->launchPreparations.size(), 2);
    for (int i = 0; i < m_helperClient->launchPreparations.size(); ++i) {
        QCOMPARE(m_helperClient->launchUsernames.at(i), i == 0 ? QStringLiteral("player1") : QStringLiteral("player2"));
        bool foundOverlay = false;
        for (const QVariant &entry : m_helperClient->launchPreparations.at(i)) {
            foundOverlay |= entry.toMap().value(QStringLiteral("mode")).toString() == QStringLiteral("overlay");
        }
        QVERIFY(foundOverlay);
    }

    const qint64 firstOwnedPid = m_runner->m_instances.at(0)->pid();
    const qint64 secondOwnedPid = m_runner->m_instances.at(1)->pid();
    QVERIFY(firstOwnedPid > 0);
    QVERIFY(secondOwnedPid > 0);
    Q_EMIT m_helperClient->instanceStopped(QStringLiteral("player1"), firstOwnedPid, QStringLiteral("exited"));
    QVERIFY(m_runner->isActive());
    QVERIFY(m_runner->m_sharedStateActive);
    Q_EMIT m_helperClient->instanceStopped(QStringLiteral("player2"), secondOwnedPid, QStringLiteral("exited"));
    QVERIFY(!m_runner->m_sharedStateActive);
    QVERIFY(m_runner->m_instances.isEmpty());
    QVERIFY(m_runner->m_instanceDirectoryPreparations.isEmpty());

    // Natural exit owns cleanup; an additional stop is idempotent.
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
    QCOMPARE(m_helperClient->launchCommands.size(), 0);
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

    QFile preScript(prePath);
    QVERIFY(preScript.open(QIODevice::WriteOnly | QIODevice::Text));
    preScript.write("#!/bin/sh\nsleep 0.2\n");
    preScript.close();
    QVERIFY(preScript.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    QFile originalPost(originalPostPath);
    QVERIFY(originalPost.open(QIODevice::WriteOnly | QIODevice::Text));
    originalPost.write("#!/bin/sh\necho original >> \"" + originalMarker.toUtf8() + "\"\n");
    originalPost.close();
    QVERIFY(originalPost.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    QFile changedPost(changedPostPath);
    QVERIFY(changedPost.open(QIODevice::WriteOnly | QIODevice::Text));
    changedPost.write("#!/bin/sh\necho changed >> \"" + changedMarker.toUtf8() + "\"\n");
    changedPost.close();
    QVERIFY(changedPost.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

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
    QCOMPARE(m_helperClient->launchCommands.size(), 2);
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 3000);

    QFile originalMarkerFile(originalMarker);
    QVERIFY(originalMarkerFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(originalMarkerFile.readAll(), QByteArray("original\n"));
    QVERIFY(!QFile::exists(changedMarker));
}
void TestSessionRunner::testPreHookUsesCapturedApplicationRevision()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    TestEnvironmentGuard homeGuard("HOME");
    TestEnvironmentGuard configGuard("XDG_CONFIG_HOME");
    qputenv("HOME", root.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (root.path() + QStringLiteral("/config")).toLocal8Bit());
    const QString steamRoot = root.path() + QStringLiteral("/.steam/steam");
    const QString secondaryLibrary = root.path() + QStringLiteral("/secondary-library");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/steamapps/common")));
    QVERIFY(QDir().mkpath(secondaryLibrary + QStringLiteral("/steamapps/common")));
    QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
    libraryVdf.write("\"libraryfolders\"\n{\n"
                     "  \"0\"\n  {\n    \"path\"\t\"" + steamRoot.toUtf8() + "\"\n  }\n"
                     "  \"1\"\n  {\n    \"path\"\t\"" + secondaryLibrary.toUtf8() + "\"\n  }\n}\n");
    libraryVdf.close();
    m_helperClient->uidHomeOverride = root.path();
    m_helperClient->canonicalHomeOverride = root.path();
    m_helperClient->player1Home = root.path() + QStringLiteral("/players/player1");
    m_helperClient->player2Home = root.path() + QStringLiteral("/players/player2");
    m_helperClient->player1SteamRoot = m_helperClient->player1Home + QStringLiteral("/.steam/steam");
    m_helperClient->player2SteamRoot = m_helperClient->player2Home + QStringLiteral("/.steam/steam");
    SteamConfigManager steamManager;
    steamManager.setHelperClient(m_helperClient);
    steamManager.setSyncShortcutsEnabled(false);
    steamManager.setShareLibraryEnabled(false);
    m_runner->setSteamConfigManager(&steamManager);
    QVERIFY(steamManager.isSteamDetected());
    createMockHeroicConfig(root.path());
    createMockLegendaryConfig(root.path());
    HeroicConfigManager heroicManager;
    heroicManager.setHelperClient(m_helperClient);
    m_runner->setHeroicConfigManager(&heroicManager);
    QVERIFY(heroicManager.isHeroicDetected());

    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Snapshot app"), QStringLiteral("/usr/bin/true"));
    QVERIFY(m_presetManager->setRequiredIntegrations(presetId, {QStringLiteral("heroic"), QStringLiteral("steam")}));
    const QVariantMap originalDir{{QStringLiteral("path"), QStringLiteral("/snapshot/original")},
                                  {QStringLiteral("mode"), QStringLiteral("acl")}};
    const QVariantMap steamMarker{{QStringLiteral("path"), steamRoot},
                                  {QStringLiteral("mode"), QStringLiteral("overlay")}};
    QVERIFY(m_presetManager->setDataDirectories(presetId, {originalDir, steamMarker}));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    QVERIFY(!steamManager.shareLibraryEnabled());

    const QString preHook = root.filePath(QStringLiteral("pre.sh"));
    QFile hook(preHook);
    QVERIFY(hook.open(QIODevice::WriteOnly | QIODevice::Text));
    hook.write("#!/bin/sh\nsleep 0.2\n");
    hook.close();
    QVERIFY(hook.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    m_sessionManager->setPreSessionExecutable(preHook);

    bool editsApplied = false;
    QTimer::singleShot(50, this, [this, presetId, &editsApplied, &steamManager, steamMarker]() {
        const QVariantMap updatedDir{{QStringLiteral("path"), QStringLiteral("/snapshot/updated")},
                                     {QStringLiteral("mode"), QStringLiteral("copy")}};
        editsApplied = m_presetManager->updateCustomPreset(presetId,
                                                           QStringLiteral("Snapshot app"),
                                                           QStringLiteral("/usr/bin/false"),
                                                           QString(),
                                                           QString())
            && m_presetManager->setDataDirectories(presetId, {updatedDir, steamMarker})
            && m_presetManager->setRequiredIntegrations(presetId, {QStringLiteral("steam")});
        steamManager.setShareLibraryEnabled(true);
    });

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 3000);
    QVERIFY(editsApplied);
    QCOMPARE(m_helperClient->launchCommands.size(), 2);
    QCOMPARE(m_helperClient->launchCommands.at(0).constFirst(), QStringLiteral("/usr/bin/true"));
    QCOMPARE(m_helperClient->launchCommands.at(1).constFirst(), QStringLiteral("/usr/bin/true"));
    QCOMPARE(m_runner->m_startingSnapshot.presets.size(), 2);
    const QStringList capturedIntegrations{QStringLiteral("heroic"), QStringLiteral("steam")};
    QCOMPARE(m_runner->m_startingSnapshot.presets.at(0).requiredIntegrations, capturedIntegrations);
    QCOMPARE(m_runner->m_startingSnapshot.presets.at(1).requiredIntegrations, capturedIntegrations);
    QVERIFY(!m_runner->m_startingSnapshot.shareSteamLibraries);
    for (const QVariantList &preparations : m_helperClient->launchPreparations) {
        QCOMPARE(preparations.constFirst().toMap().value(QStringLiteral("mode")).toString(), QStringLiteral("acl"));
        QCOMPARE(preparations.constFirst().toMap().value(QStringLiteral("source")).toString(),
                 QStringLiteral("/snapshot/original"));
        for (const QVariant &preparation : preparations) {
            QVERIFY(!preparation.toMap().value(QStringLiteral("targetAlias")).toString().startsWith(
                QStringLiteral(".couchplay/steam-libs/")));
        }
    }
    QVERIFY(m_helperClient->copiedFileUsers.contains(QStringLiteral("player1")));
    QVERIFY(m_helperClient->copiedFileUsers.contains(QStringLiteral("player2")));

    const int copiesAfterFirstStart = m_helperClient->copiedFileUsers.size();
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 3000);

    m_sessionManager->setPreSessionExecutable(QString());
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 2, 3000);
    QCOMPARE(m_helperClient->launchCommands.size(), 4);
    QCOMPARE(m_helperClient->launchCommands.at(2).constFirst(), QStringLiteral("/usr/bin/false"));
    QCOMPARE(m_helperClient->launchCommands.at(3).constFirst(), QStringLiteral("/usr/bin/false"));
    QCOMPARE(m_runner->m_startingSnapshot.presets.at(0).requiredIntegrations, QStringList{QStringLiteral("steam")});
    QCOMPARE(m_runner->m_startingSnapshot.presets.at(1).requiredIntegrations, QStringList{QStringLiteral("steam")});
    QVERIFY(m_runner->m_startingSnapshot.shareSteamLibraries);
    QCOMPARE(m_helperClient->launchPreparations.at(2).constFirst().toMap().value(QStringLiteral("mode")).toString(),
             QStringLiteral("copy"));
    QCOMPARE(m_helperClient->launchPreparations.at(2).constFirst().toMap().value(QStringLiteral("source")).toString(),
             QStringLiteral("/snapshot/updated"));
    QCOMPARE(m_helperClient->launchPreparations.at(3).constFirst().toMap()
                 .value(QStringLiteral("source")).toString(), QStringLiteral("/snapshot/updated"));
    const QVariantList secondPlayerPlans = m_helperClient->launchPreparations.at(2);
    QCOMPARE(secondPlayerPlans.size(), 3);
    QCOMPARE(secondPlayerPlans.at(1).toMap().value(QStringLiteral("mode")).toString(), QStringLiteral("overlay"));
    QCOMPARE(secondPlayerPlans.at(1).toMap().value(QStringLiteral("targetAlias")).toString(),
             QStringLiteral(".couchplay/steam-libs/0/steamapps/common"));
    QCOMPARE(secondPlayerPlans.at(2).toMap().value(QStringLiteral("targetAlias")).toString(),
             QStringLiteral(".couchplay/steam-libs/1/steamapps/common"));
    QCOMPARE(m_helperClient->launchPreparations.at(2), m_helperClient->launchPreparations.at(3));
    QCOMPARE(m_helperClient->copiedFileUsers.size(), copiesAfterFirstStart);
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 2, 3000);
    m_runner->setSteamConfigManager(m_steamConfigManager);
}

void TestSessionRunner::testReadinessAdvancesSequentialStartup()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Serial startup"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    m_helperClient->autoReady = false;
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    QVERIFY(m_runner->isActive());
    QVERIFY(!m_runner->isRunning());
    QCOMPARE(m_helperClient->launchCommands.size(), 1);
    QCOMPARE(m_helperClient->launchUsernames.constFirst(), QStringLiteral("player1"));
    const qint64 firstPid = m_helperClient->nextPid - 1;
    QVERIFY(firstPid > 0);
    QCOMPARE(m_runner->m_instances.size(), 1);
    QCOMPARE(m_runner->m_instances.constFirst()->pid(), firstPid);
    QCOMPARE(m_runner->m_instances.constFirst()->gamescopePid(), qint64(0));

    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player2"), firstPid);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player1"), firstPid + 99);
    QCOMPARE(m_helperClient->launchCommands.size(), 1);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player1"), firstPid);
    QCOMPARE(m_helperClient->launchCommands.size(), 2);
    QCOMPARE(m_helperClient->launchUsernames.constLast(), QStringLiteral("player2"));
    const qint64 secondPid = m_helperClient->nextPid - 1;
    QVERIFY(secondPid > firstPid);
    QVERIFY(m_runner->isRunning());
    QCOMPARE(startedSpy.count(), 0);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player1"), firstPid);
    QCOMPARE(m_helperClient->launchCommands.size(), 2);
    QCOMPARE(startedSpy.count(), 0);

    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player2"), secondPid);
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(m_runner->m_sessionStartupComplete);
    QCOMPARE(failedSpy.count(), 0);
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 2000);
}

void TestSessionRunner::testPreparationFailureAbortsStartup()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Preparation failure"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    m_helperClient->autoReady = false;
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);

    QVERIFY(m_runner->start());
    const qint64 firstPid = m_helperClient->nextPid - 1;
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player1"), firstPid);
    QCOMPARE(m_helperClient->launchCommands.size(), 2);
    const qint64 secondPid = m_helperClient->nextPid - 1;
    Q_EMIT m_helperClient->instanceStopped(QStringLiteral("player1"), firstPid,
                                           QStringLiteral("failed: directory preparation failed"));

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 2000);
    QCOMPARE(startedSpy.count(), 0);
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_helperClient->stopPids.contains(secondPid));
    QVERIFY(failedSpy.constFirst().constFirst().toString().contains(QStringLiteral("directory preparation failed")));
}

void TestSessionRunner::testCancelWhilePreparing()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Cancel preparation"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_helperClient->autoReady = false;
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    const qint64 preparingPid = m_helperClient->nextPid - 1;
    QVERIFY(preparingPid > 0);
    QVERIFY(m_runner->isActive());
    QVERIFY(!m_runner->isRunning());
    m_runner->stop();

    QCOMPARE(m_helperClient->stopPids, QList<qint64>{preparingPid});
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(!m_runner->isActive());
}
void TestSessionRunner::testStopFailureRetainsPreparingInstanceForRetry()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Retry stop"), QStringLiteral("/usr/bin/true"));
    QVERIFY(m_presetManager->setDataDirectories(presetId, {
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/couchplay-stop-retry")},
                    {QStringLiteral("mode"), QStringLiteral("overlay")}}
    }));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    m_helperClient->autoReady = false;

    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    const qint64 preparingPid = m_helperClient->nextPid - 1;
    QVERIFY(preparingPid > 0);
    QVERIFY(!m_runner->isRunning());
    QVERIFY(m_runner->m_sharedStateActive);

    m_helperClient->stopSucceeds = false;
    m_helperClient->killSucceeds = false;
    m_runner->stop();

    QVERIFY(m_runner->isActive());
    QVERIFY(!m_runner->isRunning());
    QCOMPARE(m_runner->m_instances.size(), 1);
    QCOMPARE(m_runner->m_instances.constFirst()->pid(), preparingPid);
    QVERIFY(m_runner->m_sharedStateActive);
    QVERIFY(!m_runner->m_instanceDirectoryPreparations.value(0).isEmpty());
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(stoppedSpy.count(), 0);

    m_helperClient->stopSucceeds = true;
    m_runner->stop();
    QCOMPARE(m_helperClient->stopPids.count(), 2);
    QCOMPARE(m_helperClient->stopPids.constLast(), preparingPid);
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(!m_runner->isActive());
    QVERIFY(!m_runner->m_sharedStateActive);
}

void TestSessionRunner::testStopDefersCleanupUntilStopReturns_data()
{
    QTest::addColumn<bool>("retry");
    QTest::newRow("initial-stop") << false;
    QTest::newRow("retry-stop") << true;
}

void TestSessionRunner::testStopDefersCleanupUntilStopReturns()
{
    QFETCH(bool, retry);
    const QString presetId = m_presetManager->addCustomPreset(
        QStringLiteral("Synchronous stop"), QStringLiteral("/usr/bin/true"));
    QVERIFY(m_presetManager->setDataDirectories(presetId, {
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/couchplay-stop-stack")},
                    {QStringLiteral("mode"), QStringLiteral("overlay")}}
    }));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, presetId);
    }
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 2000);

    const auto instances = m_runner->m_instances;
    const qint64 livePid = instances.at(0)->pid();
    Q_EMIT m_helperClient->instanceStopped(QStringLiteral("player2"), instances.at(1)->pid(),
                                         QStringLiteral("exited"));
    QVERIFY(m_runner->isActive());
    if (retry) {
        m_helperClient->stopSucceeds = false;
        m_helperClient->killSucceeds = false;
        m_runner->stop();
        QVERIFY(m_runner->isActive());
        QVERIFY(m_runner->m_sharedStateActive);
        QCOMPARE(instances.at(0)->pid(), livePid);
        QCOMPARE(stoppedSpy.count(), 0);
        m_helperClient->stopSucceeds = true;
    }

    bool callbackRan = false;
    bool retainedDuringStop = false;
    QList<GamescopeInstance *> stoppingInstances;
    m_helperClient->onStopInstance = [&](qint64 pid) {
        callbackRan = true;
        // Retain the active loop's storage even if cleanup clears m_instances;
        // baseline failure must observe early release, not invalid iterators.
        stoppingInstances = m_runner->m_instances;
        Q_EMIT m_helperClient->instanceStopped(QStringLiteral("player1"), pid,
                                             QStringLiteral("stopped"));
        retainedDuringStop = m_runner->isActive() && m_runner->m_sharedStateActive
            && m_runner->m_instances.size() == 2
            && !m_runner->m_instanceDirectoryPreparations.isEmpty()
            && stoppedSpy.count() == 0;
    };
    m_runner->stop();
    m_helperClient->onStopInstance = {};

    QVERIFY(callbackRan);
    QVERIFY(retainedDuringStop);
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(!m_runner->isActive());
    QVERIFY(!m_runner->m_sharedStateActive);
    QVERIFY(m_runner->m_instances.isEmpty());
    QVERIFY(m_runner->m_instanceDirectoryPreparations.isEmpty());
}

void TestSessionRunner::testStoppedInstanceCancelsItsPositionListener()
{
    const QString presetId = m_presetManager->addCustomPreset(
        QStringLiteral("Listener lifetime"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, presetId);
    }
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
    const auto instances = m_runner->m_instances;
    Q_EMIT m_helperClient->instanceStopped(QStringLiteral("player2"), instances.at(1)->pid(),
                                         QStringLiteral("exited"));
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
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    const QStringList scriptLines{QStringLiteral("#!/bin/sh"), QStringLiteral("sleep 0.2"),
                                  QStringLiteral("printf post >> ") + markerPath};
    script.write(scriptLines.join(QLatin1Char('\n')).toUtf8());
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Launch failure"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setPostSessionExecutable(scriptPath);
    m_helperClient->launchFails = true;
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3000);
    QCOMPARE(m_helperClient->launchCommands.size(), 1);
    QVERIFY(errorSpy.first().constFirst().toString().contains(QStringLiteral("simulated Gamescope launch failure")));
    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(marker.readAll(), QByteArray("post"));
}

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
}

void TestSessionRunner::testVirtualDisplayFailureAbortsStartup()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Vdisplay failure"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, presetId);
    }
    m_sessionManager->setInstanceConfig(1, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    StreamingFakes fakes(m_runner, /*vdisplayWorks=*/false);

    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3000);
    QVERIFY(failedSpy.first().constFirst().toString().contains(
        QStringLiteral("Player 2: could not create virtual display")));
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(m_helperClient->launchCommands.size(), 0);
    QVERIFY(!m_runner->isActive());
    QVERIFY(!m_runner->m_virtualDisplays->hasDisplays());
}

void TestSessionRunner::testStreamingInstancesGetDistinctSunshinePorts()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Streaming ports"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, presetId);
        m_sessionManager->setInstanceConfig(i, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    }
    StreamingFakes fakes(m_runner);
    m_helperClient->autoReady = false;

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

void TestSessionRunner::testStreamingStartupWaitsForSunshineReady()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Streaming startup"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceConfig(0, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    StreamingFakes fakes(m_runner);
    m_helperClient->autoReady = false;

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QSignalSpy streamStartedSpy(m_runner->m_streamManager, &StreamManager::streamStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(m_helperClient->launchCommands.size(), 1, 3000);
    QCOMPARE(m_runner->m_streamManager->streamState(0), StreamManager::NotStarted);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player1"), m_helperClient->nextPid - 1);

    // The next player waits until Sunshine for this one reports ready.
    QTRY_COMPARE_WITH_TIMEOUT(streamStartedSpy.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(m_helperClient->launchCommands.size(), 2, 3000);
    QCOMPARE(startedSpy.count(), 0);
    Q_EMIT m_helperClient->instanceReady(QStringLiteral("player2"), m_helperClient->nextPid - 1);
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
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Streaming failure"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceConfig(0, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    StreamingFakes fakes(m_runner, /*vdisplayWorks=*/true, /*sunshineExists=*/false);

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    QVERIFY(m_runner->start());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(m_helperClient->launchCommands.size(), 1);
    QVERIFY(failedSpy.first().constFirst().toString().contains(QStringLiteral("Sunshine")));
    QTRY_VERIFY_WITH_TIMEOUT(!m_runner->isActive(), 3000);
    QVERIFY(!m_runner->m_streamAudio.isActive());
    QVERIFY(!m_runner->m_virtualDisplays->hasDisplays());
}

static SessionProfile displayProfile(const QStringList &displays, const QString &mainScreen = QString())
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

    // Empty mainScreen means the primary screen
    SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:HDMI-A-1")});
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, primary), expected);

    // Explicit main screen naming the same connector
    profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:HDMI-A-1")}, QStringLiteral("HDMI-A-1"));
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, QStringLiteral("DP-1")), expected);

    // Choosing the other screen as main frees the monitor
    profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:HDMI-A-1")}, QStringLiteral("DP-1"));
    QCOMPARE(SessionRunner::validateDisplays(profile, screens, primary), QString());

    // Without any main-screen player the monitor is free to use
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
    // A main screen that is not connected falls back to the primary screen
    QCOMPARE(SessionRunner::validateDisplays(displayProfile({QStringLiteral("main"), QStringLiteral("virtual")},
                                                            QStringLiteral("DP-99"))),
             QString());
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
    QVERIFY(m_helperClient->launchCommands.isEmpty());
}

void TestSessionRunner::testValidatePairingGate()
{
    SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("virtual")});
    profile.instances[0].username = QStringLiteral("player1");
    profile.instances[1].username = QStringLiteral("player2");
    const auto none = [](const QString &) { return 0; };
    const auto onlyPlayer2 = [](const QString &username) { return username == QStringLiteral("player2") ? 1 : 0; };

    QCOMPARE(SessionRunner::validatePairing(profile, none),
             QStringLiteral("Player 2 (player2) has no paired Moonlight client. "
                            "Pair one for player2 on the Users page first."));
    QCOMPARE(SessionRunner::validatePairing(profile, onlyPlayer2), QString());

    // A real monitor only streams when the player opted in
    profile = displayProfile({QStringLiteral("main"), QStringLiteral("monitor:DP-1")});
    profile.instances[1].username = QStringLiteral("player2");
    QCOMPARE(SessionRunner::validatePairing(profile, none), QString());
    profile.instances[1].streamToMoonlight = true;
    QVERIFY(!SessionRunner::validatePairing(profile, none).isEmpty());
}

void TestSessionRunner::testValidatePairingSkipsUnpairedNonStreamingAndNoUser()
{
    SessionProfile profile = displayProfile({QStringLiteral("main"), QStringLiteral("virtual")});
    profile.instances[0].username = QStringLiteral("player1");
    profile.instances[1].username.clear();
    QCOMPARE(SessionRunner::validatePairing(profile, [](const QString &) { return 0; }), QString());
}

void TestSessionRunner::testStartRejectsUnpairedStreamingPlayer()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Unpaired"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, presetId);
    }
    m_sessionManager->setInstanceConfig(1, {{QStringLiteral("display"), QStringLiteral("virtual")}});
    StreamingFakes fakes(m_runner);
    m_runner->setSkipPairingCheck(false);
    QSignalSpy errorSpy(m_runner, &SessionRunner::errorOccurred);

    QVERIFY(!m_runner->start());
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.first().constFirst().toString().contains(QStringLiteral("has no paired Moonlight client")));
    QCOMPARE(m_runner->status(), QStringLiteral("Error"));
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_helperClient->launchCommands.isEmpty());
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
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Paired"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    for (int i = 0; i < 2; ++i) {
        m_sessionManager->setInstanceUser(i, QStringLiteral("player%1").arg(i + 1));
        m_sessionManager->setInstancePreset(i, presetId);
    }
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

void TestSessionRunner::testDeviceReconnectIgnoredDuringPostHook()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString scriptPath = scriptDir.filePath(QStringLiteral("post.sh"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("#!/bin/sh\nsleep 0.4\n");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Device reconnect"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    m_sessionManager->setPostSessionExecutable(scriptPath);
    m_runner->setDeviceManager(new DeviceManager(m_runner));
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QVERIFY(m_runner->start());
    QCOMPARE(startedSpy.count(), 1);

    m_runner->stop();
    QTRY_VERIFY_WITH_TIMEOUT(m_runner->m_hookProcess && m_runner->m_hookIsPost, 1000);
    const qsizetype ownerCalls = m_helperClient->deviceOwnerCalls.size();
    m_runner->onDeviceReconnected(QStringLiteral("test-device"), 99, 1);
    QCOMPARE(m_helperClient->deviceOwnerCalls.size(), ownerCalls);
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 2000);
}

void TestSessionRunner::testDeviceReconnectIgnoredDuringPreHook()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString scriptPath = scriptDir.filePath(QStringLiteral("pre.sh"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("#!/bin/sh\nsleep 0.4\n");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Prehook reconnect"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    m_sessionManager->setPreSessionExecutable(scriptPath);
    m_runner->setDeviceManager(new DeviceManager(m_runner));
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QVERIFY(m_runner->start());
    QVERIFY(m_runner->m_hookProcess && !m_runner->m_hookIsPost);

    // A reconnect before the hook finishes must not take ownership: cancelling the hook skips device restore.
    const qsizetype ownerCalls = m_helperClient->deviceOwnerCalls.size();
    m_runner->onDeviceReconnected(QStringLiteral("test-device"), 99, 0);
    QCOMPARE(m_helperClient->deviceOwnerCalls.size(), ownerCalls);
    QVERIFY(m_runner->m_ownedDevicePaths.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 3000);

    // Positive control: once started, the same reconnect is handled.
    m_runner->onDeviceReconnected(QStringLiteral("test-device"), 99, 0);
    QVERIFY(m_helperClient->deviceOwnerCalls.size() > ownerCalls);
}

static InputDevice makeInputDevice(int eventNumber, const QString &name, const QString &type, const QString &uniq = QString())
{
    InputDevice device;
    device.eventNumber = eventNumber;
    device.name = name;
    device.type = type;
    device.uniq = uniq;
    return device;
}

// Starts a two-player session on the mock helper, then marks the given players as streaming and
// replaces the device list; everything stays synchronous so no timer can refresh the devices.
static bool startSessionWithDevices(SessionRunner *runner,
                                    SessionManager *sessionManager,
                                    PresetManager *presetManager,
                                    const QList<int> &streaming,
                                    const QList<InputDevice> &devices)
{
    const QString presetId = presetManager->addCustomPreset(QStringLiteral("Gamepad attribution"), QStringLiteral("/usr/bin/true"));
    sessionManager->setInstanceCount(2);
    sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    sessionManager->setInstancePreset(0, presetId);
    sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    sessionManager->setInstancePreset(1, presetId);
    auto *deviceManager = new DeviceManager(runner);
    runner->setDeviceManager(deviceManager);
    if (!runner->start() || !runner->m_sessionStartupComplete) {
        return false;
    }
    for (int index : streaming) {
        runner->m_streamingInstances[index] = SessionRunner::StreamingInstanceInfo{};
    }
    deviceManager->m_devices = devices;
    return true;
}

static int ownerUidOf(const MockCouchPlayHelperClient *helper, int eventNumber)
{
    const QString path = QStringLiteral("/dev/input/event%1").arg(eventNumber);
    for (const auto &call : helper->deviceOwnerCalls) {
        if (call.path == path) {
            return call.uid;
        }
    }
    return -1;
}

void TestSessionRunner::testSunshineGamepadClaimsAreFifo()
{
    const QString pad = QStringLiteral("Sunshine PS5 (virtual) pad");
    QVERIFY(startSessionWithDevices(m_runner, m_sessionManager, m_presetManager, {0, 1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller")),
                                     makeInputDevice(902, pad, QStringLiteral("controller"))}));

    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(0);
    QCOMPARE(m_runner->m_gamepadClaims.size(), 2);

    m_runner->onDeviceAdded(901, pad);
    m_runner->onDeviceAdded(902, pad);

    QCOMPARE(ownerUidOf(m_helperClient, 901), 1002);
    QCOMPARE(ownerUidOf(m_helperClient, 902), 1001);
    QVERIFY(m_runner->m_gamepadClaims.isEmpty());
    QVERIFY(m_runner->m_ownedDevicePaths.contains(QStringLiteral("/dev/input/event901")));
    QVERIFY(m_runner->m_ownedDevicePaths.contains(QStringLiteral("/dev/input/event902")));

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadUniqGroupsDevices()
{
    const QString pad = QStringLiteral("Sunshine PS5 (virtual) pad");
    QVERIFY(startSessionWithDevices(m_runner, m_sessionManager, m_presetManager, {0, 1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller"), QStringLiteral("sunshine-pad-1")),
                                     makeInputDevice(902, pad + QStringLiteral(" motion"), QStringLiteral("other"),
                                                     QStringLiteral("sunshine-pad-1"))}));
    m_runner->m_clockOverrideMs = 10000;
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);

    m_runner->onDeviceAdded(901, pad);
    // Within 3 s the motion node shares the uniq and follows the first node without a claim.
    m_runner->m_clockOverrideMs = 12999;
    m_runner->onDeviceAdded(902, pad + QStringLiteral(" motion"));

    QCOMPARE(ownerUidOf(m_helperClient, 901), 1002);
    QCOMPARE(ownerUidOf(m_helperClient, 902), 1002);

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadSingleStreamerFallback()
{
    const QString pad = QStringLiteral("Sunshine X-Box One (virtual) pad");
    QVERIFY(startSessionWithDevices(m_runner, m_sessionManager, m_presetManager, {1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller"))}));
    QVERIFY(m_runner->m_gamepadClaims.isEmpty());

    m_runner->onDeviceAdded(901, pad);

    QCOMPARE(ownerUidOf(m_helperClient, 901), 1002);

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadClaimExpires()
{
    const QString pad = QStringLiteral("Sunshine X-Box One (virtual) pad");
    QVERIFY(startSessionWithDevices(m_runner, m_sessionManager, m_presetManager, {0, 1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller")),
                                     makeInputDevice(902, pad, QStringLiteral("controller"))}));

    m_runner->m_clockOverrideMs = 1000;
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(0);
    m_runner->m_clockOverrideMs = 1000 + m_runner->m_claimTtlMs - 1;
    m_runner->onDeviceAdded(901, pad);
    QCOMPARE(ownerUidOf(m_helperClient, 901), 1001);

    m_runner->m_clockOverrideMs = 20000;
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);
    m_runner->m_clockOverrideMs = 20000 + m_runner->m_claimTtlMs + 1;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot attribute Moonlight gamepad")));
    m_runner->onDeviceAdded(902, pad);
    QCOMPARE(ownerUidOf(m_helperClient, 902), -1);
    QVERIFY(m_runner->m_gamepadClaims.isEmpty());

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineKeyboardMouseNotChowned()
{
    QVERIFY(startSessionWithDevices(m_runner, m_sessionManager, m_presetManager, {1},
                                    {makeInputDevice(901, QStringLiteral("Sunshine Keyboard"), QStringLiteral("keyboard")),
                                     makeInputDevice(902, QStringLiteral("Sunshine Mouse"), QStringLiteral("mouse")),
                                     makeInputDevice(903, QStringLiteral("Logitech Gamepad"), QStringLiteral("controller"))}));
    Q_EMIT m_runner->m_streamManager->gamepadAnnounced(1);
    const qsizetype ownerCalls = m_helperClient->deviceOwnerCalls.size();

    m_runner->onDeviceAdded(901, QStringLiteral("Sunshine Keyboard"));
    m_runner->onDeviceAdded(902, QStringLiteral("Sunshine Mouse"));
    m_runner->onDeviceAdded(903, QStringLiteral("Logitech Gamepad"));

    QCOMPARE(m_helperClient->deviceOwnerCalls.size(), ownerCalls);
    QCOMPARE(m_runner->m_gamepadClaims.size(), 1); // claim not consumed

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
}

void TestSessionRunner::testSunshineGamepadUnownedWithoutClaim()
{
    const QString pad = QStringLiteral("Sunshine PS5 (virtual) pad");
    QVERIFY(startSessionWithDevices(m_runner, m_sessionManager, m_presetManager, {0, 1},
                                    {makeInputDevice(901, pad, QStringLiteral("controller"))}));
    const qsizetype ownerCalls = m_helperClient->deviceOwnerCalls.size();

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot attribute Moonlight gamepad")));
    m_runner->onDeviceAdded(901, pad);

    QCOMPARE(m_helperClient->deviceOwnerCalls.size(), ownerCalls);

    m_runner->m_streamingInstances.clear();
    m_runner->stop();
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
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("#!/bin/sh\necho post >> \"" + markerPath.toUtf8() + "\"\n");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setPostSessionExecutable(scriptPath);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);

    QVERIFY(m_runner->start());
    QVERIFY(m_runner->isActive());
    QCOMPARE(m_helperClient->launchCommands.size(), 2);
    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 2000);
    QVERIFY(!m_runner->isActive());

    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(marker.readAll(), QByteArray("post\n"));
}

void TestSessionRunner::testResolveCompositorHomeViaHelper()
{
    // Under Flatpak the sandbox's getpwuid cannot see host accounts; the
    // helper's host-side answer must win when present
    m_helperClient->uidHomeOverride = QStringLiteral("/home/compositor-host");
    QCOMPARE(resolveCompositorHome(m_helperClient), QStringLiteral("/home/compositor-host"));

    // Fallback to the process-local view when the helper has no answer
    m_helperClient->uidHomeOverride = QString();
    QCOMPARE(resolveCompositorHome(m_helperClient), resolveCompositorHome(nullptr));
}

void TestSessionRunner::testFinalizeDataDirResolvesIdentityViaHelper()
{
    // finalizeDataDir must resolve the target home through the helper: a
    // process-local getpwnam() cannot see CouchPlay host accounts under
    // Flatpak and aborted finalization after preparation had mounted
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    const QString steamRoot = homeDir.path() + QStringLiteral("/.steam/steam");
    QVERIFY(QDir(steamRoot).mkpath(QStringLiteral("config")));

    const QString externalLib = homeDir.path() + QStringLiteral("/extlib");
    QVERIFY(QDir(externalLib).mkpath(QStringLiteral("steamapps")));
    {
        QFile manifest(externalLib + QStringLiteral("/steamapps/appmanifest_730.acf"));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write("\"AppState\"\n{\n\t\"appid\"\t\t\"730\"\n}\n");
    }

    QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
    libraryVdf.write("\"libraryfolders\"\n"
                     "{\n"
                     "  \"0\"\n"
                     "  {\n"
                     "    \"path\"\t\t\"" + steamRoot.toUtf8() + "\"\n"
                     "  }\n"
                     "  \"1\"\n"
                     "  {\n"
                     "    \"path\"\t\t\"" + externalLib.toUtf8() + "\"\n"
                     "  }\n"
                     "}\n");
    libraryVdf.close();

    // The helper-resolved home points at the temp dir; no passwd entry for
    // player1 exists in the test environment
    m_helperClient->player1Home = homeDir.path();

    auto *steamManager = new SteamConfigManager(this);
    steamManager->setHelperClient(m_helperClient);
    m_runner->setSteamConfigManager(steamManager);
    QVERIFY(steamManager->isSteamDetected());
    steamManager->setShareLibraryEnabled(true);

    DataDirectory dir;
    dir.path = steamRoot;
    dir.mode = QStringLiteral("overlay");

    // Preparation records future instance-local common-content overlays;
    // finalization writes manifests and libraryfolders.vdf to their aliases.
    QVariantList directoryPreparations;
    QVERIFY(steamManager->prepareDataDir(dir, QStringLiteral("player1"), directoryPreparations));
    QCOMPARE(directoryPreparations.size(), 2);
    QCOMPARE(directoryPreparations.at(1).toMap().value(QStringLiteral("targetAlias")).toString(),
             QStringLiteral(".couchplay/steam-libs/1/steamapps/common"));
    QVERIFY(steamManager->finalizeDataDir(dir, QStringLiteral("player1")));

    // Files are written at the physical player home locations the bootstrap
    // will project into the instance namespace.
    QVERIFY(QFile::exists(
        homeDir.path() + QStringLiteral("/.couchplay/steam-libs/1/steamapps/appmanifest_730.acf")));
    QVERIFY(QFile::exists(steamRoot + QStringLiteral("/config/libraryfolders.vdf")));
    QFile vdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(vdf.open(QIODevice::ReadOnly));
    const QByteArray vdfContent = vdf.readAll();
    QVERIFY(vdfContent.contains(".couchplay/steam-libs"));
    QVERIFY(!vdfContent.contains("extlib")); // only alias paths + the player's own root
}

void TestSessionRunner::testPhysicalInstanceAudioDeviceRoutesOnlyThatPlayer()
{
    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Audio routing"), QStringLiteral("/usr/bin/true"));
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstancePreset(1, presetId);
    QVariantMap playerTwo = m_sessionManager->getInstanceConfig(1);
    playerTwo[QStringLiteral("audioDevice")] = QStringLiteral("alsa_output.usb-soundbar.analog-stereo");
    m_sessionManager->setInstanceConfig(1, playerTwo);

    QVERIFY(m_runner->start());
    QCOMPARE(m_helperClient->launchEnvironments.size(), 2);

    const QString sinkPrefix = QStringLiteral("PULSE_SINK=");
    auto sinkOf = [&](const QStringList &environment) {
        for (const QString &entry : environment) {
            if (entry.startsWith(sinkPrefix)) return entry.mid(sinkPrefix.size());
        }
        return QString();
    };
    QCOMPARE(m_helperClient->launchUsernames.at(0), QStringLiteral("player1"));
    QVERIFY(sinkOf(m_helperClient->launchEnvironments.at(0)).isEmpty());
    QCOMPARE(m_helperClient->launchUsernames.at(1), QStringLiteral("player2"));
    QCOMPARE(sinkOf(m_helperClient->launchEnvironments.at(1)),
             QStringLiteral("alsa_output.usb-soundbar.analog-stereo"));
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
