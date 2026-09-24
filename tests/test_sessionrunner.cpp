// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <pwd.h>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <functional>
#include <unistd.h>

#define private public
#include "SessionRunner.h"
#include "PresetManager.h"
#undef private
#include "HeroicConfigManager.h"
#include "SessionManager.h"
#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"
#define private public
#include "CouchPlayHelperClient.h"

#define private public
#include "GamescopeInstance.h"
#undef private
#include "UserLookup.h"

class MockCouchPlayHelperClient : public CouchPlayHelperClient
{
    Q_OBJECT

public:
    using CouchPlayHelperClient::CouchPlayHelperClient;

    struct AclCall {
        QString path;
        QString username;
    };

    struct DirectoryAclCall {
        QString path;
        QString username;
        bool recursive;
    };
    struct OverlayCall {
        QString username;

        QString sourceDir;
        QString targetAlias;
    };

    struct CopyDirCall {
        QString username;
        QString sourceDir;
        QString targetRelativePath;
    };

    struct MountCall {
        QString username;

        QStringList directories;
    };

    struct MirrorCall {
        QString username;
        QString sourceDir;
        QString targetRelativePath;
    };

    QList<AclCall> aclCalls;
    QList<DirectoryAclCall> directoryAclCalls;
    QList<MountCall> mountCalls;
    struct DeviceOwnerCall { QString path; int uid; };
    QList<DeviceOwnerCall> deviceOwnerCalls;
    QList<OverlayCall> overlayCalls;
    QList<CopyDirCall> copyDirCalls;
    QList<MirrorCall> mirrorCalls;
    int shortcutReadCalls = 0;
    int shortcutWriteCalls = 0;
    std::function<void()> onShortcutRead;
    QStringList mountedOverlayAliases;
    std::function<void()> onOverlayMount;
    explicit MockCouchPlayHelperClient(QObject *parent = nullptr)
        : CouchPlayHelperClient(parent)
    {
        m_available = true;
    }

    QList<QStringList> launchCommands;
    std::function<void()> onLaunchInstance;

    qint64 nextPid = 1000;
    qint64 launchInstance(const QString &username,
                          const QString &displayContext,
                          const QStringList &gamescopeArgs,
                          const QStringList &gameCommand,
                          const QString &workingDirectory,
                          const QStringList &sharedRoots,
                          const QStringList &environment,
                          const QStringList &bindPaths) override
    {
        Q_UNUSED(username)
        Q_UNUSED(displayContext)
        Q_UNUSED(gamescopeArgs)
        Q_UNUSED(workingDirectory)
        Q_UNUSED(sharedRoots)
        Q_UNUSED(environment)
        Q_UNUSED(bindPaths)
        launchCommands.append(gameCommand);
        if (onLaunchInstance) {
            auto callback = onLaunchInstance;
            onLaunchInstance = {};
            callback();
        }
        return nextPid++;
    }

    int stopInstanceCalls = 0;
    bool stopInstance(qint64 pid) override
    {
        Q_UNUSED(pid)
        ++stopInstanceCalls;
        return true;
    }

    bool restoreAllDevicesResult = true;
    int restoreAllDevicesCalls = 0;
    bool restoreAllDevices() override
    {
        ++restoreAllDevicesCalls;
        return restoreAllDevicesResult;
    }
    bool destroyVirtualOutputResult = true;
    bool destroyNullSinkResult = true;
    int destroyVirtualOutputCalls = 0;
    int destroyNullSinkCalls = 0;
    bool destroyVirtualOutput(const QString &, const QString &) override
    {
        ++destroyVirtualOutputCalls;
        return destroyVirtualOutputResult;
    }
    bool destroyNullSink(const QString &, const QString &) override
    {
        ++destroyNullSinkCalls;
        return destroyNullSinkResult;
    }

    bool killInstance(qint64 pid) override
    {
        Q_UNUSED(pid)
        return true;
    }

    bool setPathAclWithParents(const QString &path, const QString &username) override
    {
        aclCalls.append({path, username});
        return true;
    }
    bool setDirectoryAcl(const QString &path, const QString &username, bool recursive) override
    {
        directoryAclCalls.append({path, username, recursive});
        return true;
    }

    bool setupOverlayMount(const QString &username, const QString &sourceDir, const QString &targetAlias) override
    {
        overlayCalls.append({username, sourceDir, targetAlias});
        if (onOverlayMount) {
            auto callback = onOverlayMount;
            onOverlayMount = {};
            callback();
        }
        mountedOverlayAliases.append(targetAlias);
        return true;
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

    int mountSharedDirectories(const QString &username, const QStringList &directories) override
    {
        mountCalls.append({username, directories});
        return directories.size();
    }

    int unmountAllCalls = 0;
    int unmountAllResult = 0;
    int unmountAllSharedDirectories() override
    {
        ++unmountAllCalls;
        if (unmountAllResult >= 0) {
            mountedOverlayAliases.clear();
        }
        return unmountAllResult;
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
        }
        return info;
    }

    QString player1Home = QStringLiteral("/home/player1");

    // Mirrors the process-local getpwuid view by default so existing slug
    // expectations stay identical; tests can override to simulate the
    // helper-resolved host home
    QString uidHomeOverride;
    QString getUserHomeByUid(uint uid) override
    {
        if (!uidHomeOverride.isEmpty()) {
            return uidHomeOverride;
        }
        struct passwd *pw = getpwuid(uid);
        return pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
    }

    QString getUserSteamId(const QString &username) override
    {
        return username == QStringLiteral("player1") ? QStringLiteral("12345") : QString();
    }
    QString player1SteamRoot;
    QString getUserSteamRoot(const QString &username) override
    {
        return username == QStringLiteral("player1") ? player1SteamRoot : QString();
    }
    bool isSteamBootstrapped(const QString &username) override
    {
        return username == QStringLiteral("player1");
    }

    bool readSteamShortcutsForUser(const QString &username,
                                   const QString &steamId,
                                   QByteArray *content,
                                   std::function<bool()> shouldContinue,
                                   QString *) override
    {
        if (username != QStringLiteral("player1") || steamId != QStringLiteral("12345") || !content
            || (shouldContinue && !shouldContinue())) {
            return false;
        }
        ++shortcutReadCalls;
        if (onShortcutRead) {
            QEventLoop waitForRead;
            QTimer::singleShot(0, &waitForRead, [this, &waitForRead] {
                onShortcutRead();
                waitForRead.quit();
            });
            waitForRead.exec();
        }
        if (shouldContinue && !shouldContinue()) {
            return false;
        }
        *content = SteamShortcutsVdf::emptyDocument();
        return true;
    }
    bool writeSteamShortcutsForUser(const QString &username,
                                    const QString &steamId,
                                    const QByteArray &expectedDigest,
                                    const QByteArray &content,
                                    QString *errorMessage) override
    {
        Q_UNUSED(expectedDigest)
        Q_UNUSED(content)
        if (username != QStringLiteral("player1") || steamId != QStringLiteral("12345")) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("unexpected Steam account");
            }
            return false;
        }
        ++shortcutWriteCalls;
        return true;
    }
    bool writeFileToUser(const QByteArray &content, const QString &targetPath, const QString &username) override
    {
        ++shortcutWriteCalls;
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
    bool readSteamLibraryFoldersForUser(const QString &username, QByteArray *content, bool *exists) override
    {
        if (username != QStringLiteral("player1") || !content || !exists || player1SteamRoot.isEmpty()) {
            return false;
        }
        QFile file(player1SteamRoot + QStringLiteral("/config/libraryfolders.vdf"));
        *exists = file.exists();
        if (!*exists) {
            content->clear();
            return true;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        *content = file.readAll();
        return true;
    }

    bool restoreSteamLibraryFoldersForUser(const QString &username, bool existed, const QByteArray &content) override
    {
        if (username != QStringLiteral("player1") || player1SteamRoot.isEmpty()) {
            return false;
        }
        const QString path = player1SteamRoot + QStringLiteral("/config/libraryfolders.vdf");
        if (!existed) {
            return !QFile::exists(path) || QFile::remove(path);
        }
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
    }

    bool isInCouchPlayGroup(const QString &username) override
    {
        return username == QStringLiteral("player1");
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
    void testSetupSteamConfigAppliesHeroicAcls();
    void testStartSessionHeroicPresetUsesAclsAndSharedConfig();
    void testSetupDataDirectoriesUsesInstanceDirs();
    void testSetupDataDirectoriesFallsBackToPresetDirs();
    void testSetupDataDirectoriesEmptySnapshotStaysEmpty();
    void testSetupDataDirectoriesBindModeMounts();
    void testSetupDataDirectoriesBindModeEscapesPipePath();
    void testSetupDataDirectoriesMirrorsStagedData();
    void testSetupDataDirectoriesLibrarySharingGate();
    void testSetupDataDirectoriesSecondaryLibrariesMounted();
    void testCancelSteamLibraryPreparationRollsBackMounts();
    void testSetupDataDirectoriesHeroicNoConfigBulkCopy();
    void testResolveUserIdentityViaHelper();
    void testResolveUserIdentityFallback();
    void testNaturalExitTearsDownSharingState();
    void testFinalizeDataDirResolvesIdentityViaHelper();
    void testResolveCompositorHomeViaHelper();
    void testPreSessionFailurePreventsLaunch();
    void testPreHookUsesStartingProfileSnapshot();
    void testInvalidPostSessionReportsError();
    void testPostSessionRunsOnceAfterStop();
    void testSessionStoppedHandlerCanStartNewSession();
    void testActiveChangedStartRestartDoesNotRunObsoleteSetup();
    void testActiveChangedRestartDoesNotEmitStaleFinalizationSignals();
    void testStaleHookEventsCannotAffectReplacementHook();
    void testInstanceStoppedReentrancyDoesNotFinalizeReplacementSession();
    void testStartNextInstanceReentrancyDoesNotUseStaleConfig();
    void testStopInstanceReentrancyDoesNotTouchReplacement();
    void testStaleWindowCallbacksCannotAffectReplacementSession();
    void testTeardownRetainsResourcesForRetry();
    void testStreamingSetupFailureDoesNotFinalizeReplacementSession();
    void testStopDuringSteamShortcutSyncDoesNotWriteOrLaunch();
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

void TestSessionRunner::testSetupDataDirectoriesUsesInstanceDirs()
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

    QVariantMap instanceDir;
    instanceDir[QStringLiteral("path")] = QStringLiteral("/instance/dir");
    instanceDir[QStringLiteral("mode")] = QStringLiteral("acl");
    QVariantList instanceDirs;
    instanceDirs.append(instanceDir);
    m_sessionManager->setInstanceDataDirectories(0, instanceDirs);

    QVERIFY(m_runner->setupSessionResources());

    QCOMPARE(m_helperClient->aclCalls.size(), 1);
    QCOMPARE(m_helperClient->aclCalls[0].path, QStringLiteral("/instance/dir"));
    QCOMPARE(m_helperClient->aclCalls[0].username, QStringLiteral("player1"));
    QCOMPARE(m_helperClient->directoryAclCalls.size(), 1);
    QCOMPARE(m_helperClient->directoryAclCalls[0].path, QStringLiteral("/instance/dir"));
    QCOMPARE(m_helperClient->directoryAclCalls[0].username, QStringLiteral("player1"));
    QVERIFY(m_helperClient->directoryAclCalls[0].recursive);
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

void TestSessionRunner::testSetupDataDirectoriesEmptySnapshotStaysEmpty()
{
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Empty Snapshot Game"),
                                                        QStringLiteral("/usr/bin/game4"));

    QVariantMap presetDir;
    presetDir[QStringLiteral("path")] = QStringLiteral("/preset/dir");
    presetDir[QStringLiteral("mode")] = QStringLiteral("acl");
    QVariantList presetDirs;
    presetDirs.append(presetDir);
    QVERIFY(m_presetManager->setDataDirectories(presetId, presetDirs));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    // Explicitly empty snapshot (preset selected while it had no dirs)
    m_sessionManager->setInstanceDataDirectories(0, QVariantList());

    QVERIFY(m_runner->setupSessionResources());

    // Later preset additions must not leak into the intentionally-empty snapshot
    QCOMPARE(m_helperClient->aclCalls.size(), 0);
    QCOMPARE(m_helperClient->overlayCalls.size(), 0);
    QCOMPARE(m_helperClient->copyDirCalls.size(), 0);
    QCOMPARE(m_helperClient->mountCalls.size(), 0);
}

void TestSessionRunner::testSetupDataDirectoriesBindModeMounts()
{
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Bind Game"), QStringLiteral("/usr/bin/game3"));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);

    QVariantMap bindDir;
    bindDir[QStringLiteral("path")] = QStringLiteral("/home/compositor/.config/game");
    bindDir[QStringLiteral("mode")] = QStringLiteral("bind");
    QVariantList dirs;
    dirs.append(bindDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    QVERIFY(m_runner->setupSessionResources());

    // Escaped mount spec — identical to the legacy form for plain paths
    // (empty alias => home-relative target)
    QCOMPARE(m_helperClient->mountCalls.size(), 1);
    QCOMPARE(m_helperClient->mountCalls[0].username, QStringLiteral("player1"));
    QCOMPARE(m_helperClient->mountCalls[0].directories,
             (QStringList{QStringLiteral("/home/compositor/.config/game|")}));
}

void TestSessionRunner::testSetupDataDirectoriesBindModeEscapesPipePath()
{
    // Legal paths containing '|' must survive the source|alias wire format
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Pipe Bind"), QStringLiteral("/usr/bin/game7"));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);

    QVariantMap bindDir;
    bindDir[QStringLiteral("path")] = QStringLiteral("/mnt/Game|Saves");
    bindDir[QStringLiteral("mode")] = QStringLiteral("bind");
    QVariantList dirs;
    dirs.append(bindDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    QVERIFY(m_runner->setupSessionResources());

    QCOMPARE(m_helperClient->mountCalls.size(), 1);
    // Escaped pipe + trailing separator (empty alias)
    QCOMPARE(m_helperClient->mountCalls[0].directories, (QStringList{QStringLiteral("/mnt/Game\\|Saves|")}));
}

void TestSessionRunner::testSetupDataDirectoriesMirrorsStagedData()
{
    QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Staged Game"), QStringLiteral("/usr/bin/game5"));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);

    // External overlay dir with staged files, a bind dir WITH staged files
    // (must be ignored: bind has no private layer — seeding would mutate the
    // shared source), and an acl dir (staging never applies to acl)
    QVariantList dirs;
    QVariantMap overlayDir;
    overlayDir[QStringLiteral("path")] = QStringLiteral("/opt/games/game");
    overlayDir[QStringLiteral("mode")] = QStringLiteral("overlay");
    QVariantMap bindDir;
    bindDir[QStringLiteral("path")] = QStringLiteral("/opt/other/lib");
    bindDir[QStringLiteral("mode")] = QStringLiteral("bind");
    QVariantMap aclDir;
    aclDir[QStringLiteral("path")] = QStringLiteral("/opt/readonly");
    aclDir[QStringLiteral("mode")] = QStringLiteral("acl");
    dirs.append(overlayDir);
    dirs.append(bindDir);
    dirs.append(aclDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    struct passwd *pw = getpwuid(getuid());
    QString compositorHome = pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
    QString stagingRoot = playerDataStagingRoot(presetId, QStringLiteral("player1"));
    QString overlayStaging = stagingRoot + QLatin1Char('/')
        + dataDirectoryStagingSlug(QStringLiteral("/opt/games/game"), compositorHome);
    QVERIFY(QDir().mkpath(overlayStaging));
    QFile seed(overlayStaging + QStringLiteral("/config.ini"));
    QVERIFY(seed.open(QIODevice::WriteOnly));
    seed.write("player=1\n");
    seed.close();
    QString bindStaging = stagingRoot + QLatin1Char('/')
        + dataDirectoryStagingSlug(QStringLiteral("/opt/other/lib"), compositorHome);
    QVERIFY(QDir().mkpath(bindStaging));
    QFile bindSeed(bindStaging + QStringLiteral("/seed.ini"));
    QVERIFY(bindSeed.open(QIODevice::WriteOnly));
    bindSeed.write("must-not-mirror\n");
    bindSeed.close();

    QVERIFY(m_runner->setupSessionResources());

    // Only the overlay dir is mirrored, into the player's view of that dir
    // (external path -> .couchplay/mounts mapping); the bind staging content
    // must be ignored entirely
    QCOMPARE(m_helperClient->mirrorCalls.size(), 1);
    QCOMPARE(m_helperClient->mirrorCalls[0].username, QStringLiteral("player1"));
    QCOMPARE(m_helperClient->mirrorCalls[0].sourceDir, overlayStaging);
    QCOMPARE(m_helperClient->mirrorCalls[0].targetRelativePath, QStringLiteral(".couchplay/mounts/opt/games/game"));

    QDir(stagingRoot).removeRecursively();
}

void TestSessionRunner::testSetupDataDirectoriesLibrarySharingGate()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    // Mock a detected Steam installation
    QString steamRoot = homeDir.path() + QStringLiteral("/.steam/steam");
    QDir().mkpath(steamRoot + QStringLiteral("/config"));
    QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
    libraryVdf.write("\"libraryfolders\"\n{\n}\n");
    libraryVdf.close();

    auto *steamManager = new SteamConfigManager(this);
    m_runner->setSteamConfigManager(steamManager);
    QVERIFY(steamManager->isSteamDetected());
    QCOMPARE(steamManager->steamPaths().steamRoot, steamRoot);
    QVERIFY(!steamManager->shareLibraryEnabled()); // default off

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));

    QVariantMap overlayDir;
    overlayDir[QStringLiteral("path")] = steamRoot;
    overlayDir[QStringLiteral("mode")] = QStringLiteral("overlay");
    QVariantList dirs;
    dirs.append(overlayDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    // Library sharing disabled: the steamRoot entry must NOT be mounted — the
    // player's own Steam root is never overlaid
    m_runner->setupSessionResources();
    QCOMPARE(m_helperClient->overlayCalls.size(), 0);

    // Library sharing enabled with no parseable libraries: still no mounts
    // (nothing to share), and crucially no home-relative overlay of the
    // player's Steam root
    steamManager->setShareLibraryEnabled(true);
    m_runner->setupSessionResources();
    QCOMPARE(m_helperClient->overlayCalls.size(), 0);
    for (const auto &call : m_helperClient->overlayCalls) {
        QVERIFY(!call.targetAlias.isEmpty()); // everything alias-mounted, never at the player's Steam root path
        QVERIFY(call.sourceDir != steamRoot || call.targetAlias == QStringLiteral(".couchplay/steam-libs/0"));
    }
}

void TestSessionRunner::testSetupDataDirectoriesSecondaryLibrariesMounted()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    QString steamRoot = homeDir.path() + QStringLiteral("/.steam/steam");
    QDir().mkpath(steamRoot + QStringLiteral("/config"));
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
                     "    \"path\"\t\t\"/mnt/steamlibrary\"\n"
                     "  }\n"
                     "}\n");
    libraryVdf.close();

    // Finalization snapshots the target user's libraryfolders.vdf through the
    // helper. Keep that target root in the isolated fixture so the test does
    // not depend on a real /home/player1 installation.
    const QString player1Home = homeDir.path() + QStringLiteral("/player1");
    const QString player1SteamRoot = player1Home + QStringLiteral("/.steam/steam");
    QVERIFY(QDir().mkpath(player1SteamRoot + QStringLiteral("/config")));
    m_helperClient->player1Home = player1Home;
    m_helperClient->player1SteamRoot = player1SteamRoot;

    auto *steamManager = new SteamConfigManager(this);
    steamManager->setHelperClient(m_helperClient); // prepareDataDir ACLs/mounts via the manager's own client
    m_runner->setSteamConfigManager(steamManager);
    QVERIFY(steamManager->isSteamDetected());
    steamManager->setShareLibraryEnabled(true);

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));

    QVariantMap overlayDir;
    overlayDir[QStringLiteral("path")] = steamRoot;
    overlayDir[QStringLiteral("mode")] = QStringLiteral("overlay");
    QVariantList dirs;
    dirs.append(overlayDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {QStringLiteral("steam")}));
    QVERIFY(m_runner->setupSessionResources());

    // Both libraries expose only steamapps/common under their player aliases;
    // the player's Steam root and the compositor's account state stay private.
    QCOMPARE(m_helperClient->overlayCalls.size(), 2);
    bool foundPrimary = false;
    bool foundSecondary = false;
    for (const auto &call : m_helperClient->overlayCalls) {
        QVERIFY(!call.targetAlias.isEmpty());
        if (call.sourceDir == steamRoot + QStringLiteral("/steamapps/common")) {
            foundPrimary = true;
            QCOMPARE(call.targetAlias, QStringLiteral(".couchplay/steam-libs/0/steamapps/common"));
        }
        if (call.sourceDir == QStringLiteral("/mnt/steamlibrary/steamapps/common")) {
            foundSecondary = true;
            QCOMPARE(call.targetAlias, QStringLiteral(".couchplay/steam-libs/1/steamapps/common"));
        }
        QCOMPARE(call.username, QStringLiteral("player1"));
    }
    QVERIFY(foundPrimary);
    QVERIFY(foundSecondary);
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

    // Resolver defaults still apply: install-path overlay + game-dir ACL
    QCOMPARE(m_helperClient->overlayCalls.size(), 1);
    QCOMPARE(m_helperClient->overlayCalls[0].sourceDir, heroicManager.defaultInstallPath());
    QCOMPARE(m_helperClient->overlayCalls[0].username, QStringLiteral("ghostuser"));
    QCOMPARE(m_helperClient->aclCalls.size(), 1);
    QCOMPARE(m_helperClient->aclCalls[0].path, homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame"));
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
    // A session whose games exit on their own must release the privileged
    // sharing state; a later stop() must neither bypass nor double-run it
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));

    QVariantList dirs;
    QVariantMap overlayDir;
    overlayDir[QStringLiteral("path")] = QStringLiteral("/home/compositor/Games/MyGame");
    overlayDir[QStringLiteral("mode")] = QStringLiteral("overlay");
    dirs.append(overlayDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    QVERIFY(m_runner->setupSessionResources());
    QVERIFY(m_runner->m_sharedStateActive);
    QCOMPARE(m_helperClient->overlayCalls.size(), 1);
    QCOMPARE(m_helperClient->unmountAllCalls, 0);

    // Last instance exits naturally (wired the way startNextInstance does)
    auto *instance = new GamescopeInstance(m_runner);
    instance->m_index = 0;
    connect(instance, &GamescopeInstance::stopped, m_runner, &SessionRunner::onInstanceStopped);
    m_runner->m_instances.append(instance);
    QMetaObject::invokeMethod(instance, "stopped");

    QVERIFY(!m_runner->m_sharedStateActive);
    QCOMPARE(m_helperClient->unmountAllCalls, 1);

    // stop() after natural exit takes the early return without re-tearing down
    m_runner->stop();
    QCOMPARE(m_helperClient->unmountAllCalls, 1);

    // A new session re-arms the tracker and stop() cleans it up again
    QVERIFY(m_runner->setupSessionResources());
    QVERIFY(m_runner->m_sharedStateActive);
    m_runner->stop();
    QCOMPARE(m_helperClient->unmountAllCalls, 2);
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

void TestSessionRunner::testSessionStoppedHandlerCanStartNewSession()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString postPath = scriptDir.filePath(QStringLiteral("post-new-session.sh"));
    const QString markerPath = scriptDir.filePath(QStringLiteral("post-new-session.log"));
    QFile postScript(postPath);
    QVERIFY(postScript.open(QIODevice::WriteOnly | QIODevice::Text));
    postScript.write("#!/bin/sh\necho new-session >> \"" + markerPath.toUtf8() + "\"\n");
    postScript.close();
    QVERIFY(postScript.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString presetId = m_presetManager->addCustomPreset(QStringLiteral("Reentrant restart game"),
                                                              QStringLiteral("/bin/true"));
    QVERIFY(!presetId.isEmpty());
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    bool restarted = false;
    bool restartAccepted = false;
    bool restartStateInitialized = false;
    connect(m_runner, &SessionRunner::sessionStopped, m_runner,
            [this, postPath, &restarted, &restartAccepted, &restartStateInitialized] {
        if (restarted) {
            return;
        }
        restarted = true;
        m_sessionManager->setPostSessionExecutable(postPath);
        restartAccepted = m_runner->start();
        restartStateInitialized = m_runner->isActive() && m_runner->m_hasStartingProfile
            && m_runner->m_startingProfile.postSessionExecutable == postPath && m_runner->m_postHookArmed;
    });

    QVERIFY(m_runner->start());
    m_runner->stop();

    QVERIFY(restarted);
    QVERIFY(restartAccepted);
    QVERIFY(restartStateInitialized);
    QVERIFY(m_runner->isActive());
    QVERIFY(m_runner->m_hasStartingProfile);
    QCOMPARE(m_runner->m_startingProfile.postSessionExecutable, postPath);
    QVERIFY(m_runner->m_postHookArmed);
    QVERIFY(!m_runner->m_finalizing);
    QCOMPARE(stoppedSpy.count(), 1);

    m_runner->stop();
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 2, 2000);
    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(marker.readAll(), QByteArray("new-session\n"));
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

    const QByteArray originalLibraryFolders =
        QByteArrayLiteral("\"libraryfolders\"\n"
                          "{\n"
                          "  \"0\"\n"
                          "  {\n"
                          "    \"path\"\t\t\"" )
        + steamRoot.toUtf8()
        + QByteArrayLiteral("\"\n"
                            "  }\n"
                            "  \"1\"\n"
                            "  {\n"
                            "    \"path\"\t\t\"")
        + externalLib.toUtf8()
        + QByteArrayLiteral("\"\n"
                            "  }\n"
                            "}\n");
    QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
    QCOMPARE(libraryVdf.write(originalLibraryFolders), originalLibraryFolders.size());
    libraryVdf.close();

    // The helper-resolved home points at the temp dir; no passwd entry for
    // player1 exists in the test environment
    m_helperClient->player1Home = homeDir.path();
    m_helperClient->player1SteamRoot = steamRoot;

    auto *steamManager = new SteamConfigManager(this);
    steamManager->setHelperClient(m_helperClient);
    m_runner->setSteamConfigManager(steamManager);
    QVERIFY(steamManager->isSteamDetected());
    steamManager->setShareLibraryEnabled(true);

    DataDirectory dir;
    dir.path = steamRoot;
    dir.mode = QStringLiteral("overlay");

    // Production sequence: prepare mounts the alias libraries (and loads
    // them); finalize then writes manifests and libraryfolders.vdf
    QVERIFY(steamManager->prepareDataDir(dir, QStringLiteral("player1")));
    QVERIFY(steamManager->finalizeDataDir(dir, QStringLiteral("player1")));

    // Manifests and libraryfolders.vdf landed under the helper-resolved home
    // (each alias mount presents its library, so the manifest sits in the
    // library's own steamapps/)
    QVERIFY(QFile::exists(
        homeDir.path() + QStringLiteral("/.couchplay/steam-libs/1/steamapps/appmanifest_730.acf")));
    QVERIFY(QFile::exists(steamRoot + QStringLiteral("/config/libraryfolders.vdf")));
    QFile vdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(vdf.open(QIODevice::ReadOnly));
    const QByteArray vdfContent = vdf.readAll();
    QVERIFY(vdfContent.contains(".couchplay/steam-libs"));
    QVERIFY(!vdfContent.contains("extlib")); // only alias paths + the player's own root
    QVERIFY(steamManager->cleanupLibrarySharing(QStringLiteral("player1")));
    QFile restoredVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(restoredVdf.open(QIODevice::ReadOnly));
    QCOMPARE(restoredVdf.readAll(), originalLibraryFolders);
}
void TestSessionRunner::testActiveChangedRestartDoesNotEmitStaleFinalizationSignals()
{
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));

    QStringList events;
    bool restarted = false;
    bool restartAccepted = false;
    connect(m_runner, &SessionRunner::sessionStopped, this, [&events] { events.append(QStringLiteral("stopped")); });
    connect(m_runner, &SessionRunner::activeChanged, this, [this, &events, &restarted, &restartAccepted] {
        events.append(m_runner->isActive() ? QStringLiteral("active") : QStringLiteral("inactive"));
        if (!m_runner->isActive() && !restarted) {
            restarted = true;
            events.append(QStringLiteral("restart-entry"));
            restartAccepted = m_runner->start();
            events.append(QStringLiteral("restart-returned"));
        }
    });
    connect(m_runner, &SessionRunner::statusChanged, this, [&events] { events.append(QStringLiteral("status")); });
    connect(m_runner, &SessionRunner::runningChanged, this, [&events] { events.append(QStringLiteral("running")); });
    connect(m_runner, &SessionRunner::instancesChanged, this, [&events] { events.append(QStringLiteral("instances")); });

    QVERIFY(m_runner->start());
    m_runner->stop();

    QVERIFY(restarted);
    QVERIFY(restartAccepted);
    QVERIFY(m_runner->isActive());
    QCOMPARE(events.count(QStringLiteral("stopped")), 1);
    QVERIFY(events.indexOf(QStringLiteral("stopped")) < events.indexOf(QStringLiteral("restart-entry")));
    QCOMPARE(events.last(), QStringLiteral("restart-returned"));

    m_runner->stop();
}

void TestSessionRunner::testActiveChangedStartRestartDoesNotRunObsoleteSetup()
{
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    const int configuredInstances = m_sessionManager->currentProfile().instances.size();
    const qsizetype initialLaunchCount = m_helperClient->launchCommands.size();
    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    bool restarted = false;
    bool restartAccepted = false;
    connect(m_runner, &SessionRunner::activeChanged, this, [this, &restarted, &restartAccepted] {
        if (!m_runner->isActive() || restarted) {
            return;
        }
        restarted = true;
        m_runner->stop();
        restartAccepted = m_runner->start();
    });

    QVERIFY(m_runner->start());

    QVERIFY(restarted);
    QVERIFY(restartAccepted);
    QVERIFY(m_runner->isActive());
    QCOMPARE(m_helperClient->launchCommands.size() - initialLaunchCount, configuredInstances);
    QCOMPARE(startedSpy.count(), 1);

    m_runner->stop();
}

void TestSessionRunner::testStreamingSetupFailureDoesNotFinalizeReplacementSession()
{
    QVariantMap streamingConfig;
    streamingConfig.insert(QStringLiteral("outputMode"), QStringLiteral("streaming"));
    m_sessionManager->setInstanceConfig(0, streamingConfig);
    m_helperClient->m_available = false;

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    bool restarted = false;
    bool restartAccepted = false;
    connect(m_runner, &SessionRunner::errorOccurred, m_runner,
            [this, &restarted, &restartAccepted] {
        if (restarted) {
            return;
        }
        restarted = true;
        m_runner->stop();
        m_helperClient->m_available = true;
        QVariantMap physicalConfig;
        physicalConfig.insert(QStringLiteral("outputMode"), QStringLiteral("physical"));
        m_sessionManager->setInstanceConfig(0, physicalConfig);
        restartAccepted = m_runner->start();
    });

    QVERIFY(m_runner->start());

    QVERIFY(restarted);
    QVERIFY(restartAccepted);
    QVERIFY(m_runner->isActive());
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);

    m_runner->stop();
    QCOMPARE(stoppedSpy.count(), 2);
}
void TestSessionRunner::testStopDuringSteamShortcutSyncDoesNotWriteOrLaunch()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourceRoot = tempDir.path() + QStringLiteral("/source-steam");
    const QString sourceShortcuts = sourceRoot + QStringLiteral("/userdata/12345/config/shortcuts.vdf");
    QVERIFY(QDir().mkpath(QFileInfo(sourceShortcuts).absolutePath()));
    QFile sourceFile(sourceShortcuts);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.write(SteamShortcutsVdf::emptyDocument());
    sourceFile.close();

    const QString targetHome = tempDir.path() + QStringLiteral("/player-home");
    QVERIFY(QDir().mkpath(targetHome));
    m_helperClient->player1Home = targetHome;
    m_helperClient->player1SteamRoot = tempDir.path() + QStringLiteral("/target-steam");

    SteamPaths paths;
    paths.valid = true;
    paths.steamRoot = sourceRoot;
    paths.configDir = sourceRoot + QStringLiteral("/config");
    paths.userDataDir = sourceRoot + QStringLiteral("/userdata/12345");
    paths.libraryFoldersVdf = paths.configDir + QStringLiteral("/libraryfolders.vdf");
    paths.shortcutsVdf = sourceShortcuts;
    m_steamConfigManager->m_steamPaths = paths;
    m_steamConfigManager->setHelperClient(m_helperClient);
    m_steamConfigManager->setSyncShortcutsEnabled(true);

    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {QStringLiteral("steam")}));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));

    m_runner->m_startingProfile = m_sessionManager->currentProfile();
    m_runner->m_hasStartingProfile = true;
    m_runner->m_startupGeneration = 7;
    m_runner->m_active = true;
    LaunchCommand launchCommand;
    launchCommand.program = QStringLiteral("/bin/true");
    m_runner->m_launchCommands.append(launchCommand);
    m_helperClient->onShortcutRead = [this] { m_runner->stop(); };

    m_runner->continueStart();

    QCOMPARE(m_helperClient->shortcutReadCalls, 1);
    QCOMPARE(m_helperClient->shortcutWriteCalls, 0);
    QVERIFY(m_helperClient->launchCommands.isEmpty());
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_runner->m_startupGeneration > 7);
}

void TestSessionRunner::testCancelSteamLibraryPreparationRollsBackMounts()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    const QString steamRoot = homeDir.path() + QStringLiteral("/.steam/steam");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
    const QString secondaryLibrary = homeDir.path() + QStringLiteral("/secondary-library");
    QVERIFY(QDir().mkpath(secondaryLibrary + QStringLiteral("/steamapps")));
    QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
    libraryVdf.write("\"libraryfolders\"\n{\n"
                     "  \"0\"\n  {\n"
                     "    \"path\"\t\t\"" + steamRoot.toUtf8() + "\"\n"
                     "  }\n  \"1\"\n  {\n"
                     "    \"path\"\t\t\"" + secondaryLibrary.toUtf8() + "\"\n"
                     "  }\n}\n");
    libraryVdf.close();
    const QString player1Home = homeDir.path() + QStringLiteral("/player1");
    const QString player1SteamRoot = player1Home + QStringLiteral("/.steam/steam");
    const QString player1Config = player1SteamRoot + QStringLiteral("/config");
    QVERIFY(QDir().mkpath(player1Config));
    QVERIFY(QDir().mkpath(player1SteamRoot + QStringLiteral("/userdata/12345/config")));
    const QByteArray originalPlayerLibraryFolders =
        QByteArrayLiteral("\"libraryfolders\" { \"1\" { \"path\" \"/mnt/player-library\" } }\n");
    QFile playerLibraryFolders(player1Config + QStringLiteral("/libraryfolders.vdf"));
    QVERIFY(playerLibraryFolders.open(QIODevice::WriteOnly));
    QCOMPARE(playerLibraryFolders.write(originalPlayerLibraryFolders), originalPlayerLibraryFolders.size());
    playerLibraryFolders.close();

    m_helperClient->player1Home = player1Home;
    m_helperClient->player1SteamRoot = player1SteamRoot;

    delete m_steamConfigManager;
    m_steamConfigManager = new SteamConfigManager(this);
    auto *steamManager = m_steamConfigManager;
    steamManager->setHelperClient(m_helperClient);
    steamManager->setShareLibraryEnabled(true);
    m_runner->setSteamConfigManager(steamManager);
    QVERIFY(steamManager->isSteamDetected());
    QVERIFY(m_presetManager->setRequiredIntegrations(QStringLiteral("steam"), {QStringLiteral("steam")}));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));
    QVariantMap steamRootDir;
    steamRootDir.insert(QStringLiteral("path"), steamRoot);
    steamRootDir.insert(QStringLiteral("mode"), QStringLiteral("overlay"));
    QVariantList dataDirectories;
    dataDirectories.append(steamRootDir);
    m_sessionManager->setInstanceDataDirectories(0, dataDirectories);

    QSignalSpy startedSpy(m_runner, &SessionRunner::sessionStarted);
    bool restarted = false;
    bool restartAccepted = false;
    connect(m_runner, &SessionRunner::sessionStopped, m_runner, [this, &restarted, &restartAccepted] {
        if (!restarted) {
            restarted = true;
            restartAccepted = m_runner->start();
        }
    });
    m_helperClient->onOverlayMount = [this] { m_runner->stop(); };
    QVERIFY(m_runner->start());

    QVERIFY(restarted);
    QVERIFY(restartAccepted);
    QCOMPARE(m_helperClient->overlayCalls.size(), 3);
    QCOMPARE(m_helperClient->mountedOverlayAliases.size(), 2);
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(m_runner->isActive());
    QVERIFY(!m_runner->m_finalizing);
    m_runner->stop();
    QFile restoredLibraryFolders(player1Config + QStringLiteral("/libraryfolders.vdf"));
    QVERIFY(restoredLibraryFolders.open(QIODevice::ReadOnly));
    QCOMPARE(restoredLibraryFolders.readAll(), originalPlayerLibraryFolders);
}

void TestSessionRunner::testStaleHookEventsCannotAffectReplacementHook()
{
    QTemporaryDir scriptDir;
    QVERIFY(scriptDir.isValid());
    const QString scriptPath = scriptDir.filePath(QStringLiteral("blocking-hook.sh"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("#!/bin/sh\nexec sleep 20\n");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    QSignalSpy failedSpy(m_runner, &SessionRunner::sessionStartFailed);
    m_runner->runHook(scriptPath, false);
    QProcess *staleErrorProcess = m_runner->m_hookProcess;
    QVERIFY(staleErrorProcess);
    QTRY_COMPARE_WITH_TIMEOUT(staleErrorProcess->state(), QProcess::Running, 2000);
    auto *replacementHook = new QProcess(m_runner);
    m_runner->m_hookProcess = replacementHook;
    QVERIFY(QMetaObject::invokeMethod(staleErrorProcess,
                                      "errorOccurred",
                                      Qt::DirectConnection,
                                      Q_ARG(QProcess::ProcessError, QProcess::FailedToStart)));
    QCOMPARE(m_runner->m_hookProcess, replacementHook);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(QMetaObject::invokeMethod(staleErrorProcess,
                                      "finished",
                                      Qt::DirectConnection,
                                      Q_ARG(int, 0),
                                      Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)));
    QCOMPARE(m_runner->m_hookProcess, replacementHook);
    staleErrorProcess->kill();
    staleErrorProcess->waitForFinished(2000);

    m_runner->m_hookProcess = nullptr;
    m_runner->runHook(scriptPath, false);
    QProcess *staleGenerationProcess = m_runner->m_hookProcess;
    QVERIFY(staleGenerationProcess);
    QTRY_COMPARE_WITH_TIMEOUT(staleGenerationProcess->state(), QProcess::Running, 2000);
    ++m_runner->m_startupGeneration;
    QVERIFY(QMetaObject::invokeMethod(staleGenerationProcess,
                                      "finished",
                                      Qt::DirectConnection,
                                      Q_ARG(int, 0),
                                      Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)));
    QCOMPARE(m_runner->m_hookProcess, staleGenerationProcess);
    QVERIFY(!m_runner->m_preHookCompleted);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(QMetaObject::invokeMethod(staleGenerationProcess,
                                      "errorOccurred",
                                      Qt::DirectConnection,
                                      Q_ARG(QProcess::ProcessError, QProcess::FailedToStart)));
    QCOMPARE(m_runner->m_hookProcess, staleGenerationProcess);
    QVERIFY(!m_runner->m_preHookCompleted);
    staleGenerationProcess->kill();
    staleGenerationProcess->waitForFinished(2000);
    m_runner->m_hookProcess = nullptr;
}

void TestSessionRunner::testInstanceStoppedReentrancyDoesNotFinalizeReplacementSession()
{
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    int startedCount = 0;
    connect(m_runner, &SessionRunner::sessionStarted, m_runner, [this, &startedCount] {
        ++startedCount;
        if (startedCount == 2 && !m_runner->m_instances.isEmpty()) {
            m_runner->m_instances.first()->m_helperPid = 0;
        }
    });

    QSignalSpy stoppedSpy(m_runner, &SessionRunner::sessionStopped);
    bool restartFromStop = false;
    bool restartAccepted = false;
    connect(m_runner, &SessionRunner::sessionStopped, m_runner, [this, &restartFromStop, &restartAccepted] {
        if (!restartFromStop) {
            restartFromStop = true;
            restartAccepted = m_runner->start();
        }
    });

    bool stopFromInstanceSignal = false;
    connect(m_runner, &SessionRunner::instanceStopped, m_runner, [this, &stopFromInstanceSignal](int) {
        if (!stopFromInstanceSignal) {
            stopFromInstanceSignal = true;
            m_runner->stop();
        }
    });

    QVERIFY(m_runner->start());
    QCOMPARE(startedCount, 1);
    QVERIFY(!m_runner->m_instances.isEmpty());
    GamescopeInstance *stoppedInstance = m_runner->m_instances.first();
    stoppedInstance->m_helperPid = 0;
    const quint64 stoppedGeneration = m_runner->m_startupGeneration;
    QVERIFY(QMetaObject::invokeMethod(stoppedInstance, "stopped", Qt::DirectConnection));

    QVERIFY(stopFromInstanceSignal);
    QVERIFY(restartFromStop);
    QVERIFY(restartAccepted);
    QCOMPARE(startedCount, 2);
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(m_runner->isActive());
    QVERIFY(!m_runner->m_finalizing);
    QVERIFY(m_runner->m_startupGeneration > stoppedGeneration);

    m_runner->stop();
}

void TestSessionRunner::testStartNextInstanceReentrancyDoesNotUseStaleConfig()
{
    QVariantMap config;
    config.insert(QStringLiteral("username"), QStringLiteral("player1"));
    config.insert(QStringLiteral("outputMode"), QStringLiteral("physical"));
    config.insert(QStringLiteral("outputWidth"), 1280);
    config.insert(QStringLiteral("outputHeight"), 720);
    config.insert(QStringLiteral("gameCommand"), QStringList{QStringLiteral("game")});

    m_runner->m_active = true;
    m_runner->m_startupGeneration = 1;
    m_runner->m_pendingInstanceConfigs.append(config);
    m_helperClient->onLaunchInstance = [this] { m_runner->stop(); };

    m_runner->startNextInstance();

    QCOMPARE(m_helperClient->launchCommands.size(), 1);
    // The launch callback stopped the session before start() returned. The
    // synchronous started signal is stale and must stop that launched helper
    // process instead of leaving it orphaned.
    QCOMPARE(m_helperClient->stopInstanceCalls, 1);
    QVERIFY(!m_runner->isActive());
    QVERIFY(m_runner->m_pendingInstanceConfigs.isEmpty());
    QVERIFY(m_runner->m_instances.isEmpty());
}
void TestSessionRunner::testStopInstanceReentrancyDoesNotTouchReplacement()
{
    auto *instance = new GamescopeInstance(m_runner);
    instance->m_index = 0;
    instance->m_helperPid = 100;
    m_runner->m_instances.append(instance);
    auto *replacement = new GamescopeInstance(m_runner);
    replacement->m_index = 0;

    // Run before SessionRunner's stopped handler to model synchronous
    // replacement during stop().
    connect(instance, &GamescopeInstance::stopped, m_runner, [this, replacement] {
        m_runner->m_instances[0] = replacement;
    });
    connect(instance, &GamescopeInstance::stopped, m_runner, &SessionRunner::onInstanceStopped);

    m_runner->stopInstance(0);
    QCOMPARE(m_runner->m_instances.value(0), replacement);
}


void TestSessionRunner::testStaleWindowCallbacksCannotAffectReplacementSession()
{
    m_runner->m_active = true;
    m_runner->m_startupGeneration = 2;
    m_runner->m_nextInstanceToStart = 0;
    m_runner->m_pendingInstanceConfigs.append(QVariantMap{});
    m_runner->m_pendingWindowRequests.insert(41, SessionRunner::PendingWindowRequest{1, 0});

    m_runner->onWindowPositioned(41, QStringLiteral("old-window"));
    QCOMPARE(m_runner->m_nextInstanceToStart, 0);
    QVERIFY(m_runner->m_positionedWindowIds.isEmpty());

    m_runner->m_pendingWindowRequests.insert(42, SessionRunner::PendingWindowRequest{1, 0});
    m_runner->onWindowPositioningTimeout(42);
    QVERIFY(m_runner->isActive());
    QCOMPARE(m_runner->m_startupGeneration, quint64(2));

    m_runner->m_pendingWindowRequests.insert(43, SessionRunner::PendingWindowRequest{2, 0});
    m_runner->onWindowPositioned(43, QStringLiteral("current-window"));
    QCOMPARE(m_runner->m_nextInstanceToStart, 1);
    QCOMPARE(m_runner->m_positionedWindowIds, QStringList{QStringLiteral("current-window")});
}

void TestSessionRunner::testTeardownRetainsResourcesForRetry()
{
    SessionRunner::StreamingInstanceInfo streamingInfo;
    streamingInfo.username = QStringLiteral("player1");
    streamingInfo.displayContext = QStringLiteral("wayland-99");
    streamingInfo.sinkName = QStringLiteral("sink-99");
    streamingInfo.virtualDisplayCreated = true;
    streamingInfo.nullSinkCreated = true;
    m_runner->m_streamingInstances.insert(7, streamingInfo);
    m_runner->m_sharedStateActive = true;
    m_runner->m_steamSharedUsers.insert(QStringLiteral("player1"));
    m_runner->m_ownedDevicePaths.append(QStringLiteral("/dev/input/event-test"));
    m_steamConfigManager->setHelperClient(m_helperClient);
    m_helperClient->m_available = false;

    m_runner->cleanupStreamingInstance(7);
    m_runner->stop();

    QVERIFY(m_runner->m_streamingInstances.contains(7));
    QVERIFY(m_runner->m_sharedStateActive);
    QVERIFY(m_runner->m_steamSharedUsers.contains(QStringLiteral("player1")));
    QVERIFY(m_runner->m_ownedDevicePaths.contains(QStringLiteral("/dev/input/event-test")));
    QVERIFY(!m_runner->start());

    m_helperClient->m_available = true;
    m_helperClient->destroyVirtualOutputResult = false;
    m_helperClient->destroyNullSinkResult = false;
    m_helperClient->restoreAllDevicesResult = false;
    m_helperClient->unmountAllResult = -1;
    m_runner->stop();
    // A successful D-Bus connection does not imply ResetAllDevices succeeded.
    // Keep the path for a later retry when the helper reports failure.
    QVERIFY(m_runner->m_ownedDevicePaths.contains(QStringLiteral("/dev/input/event-test")));
    QVERIFY(m_runner->m_streamingInstances.contains(7));
    QCOMPARE(m_helperClient->destroyVirtualOutputCalls, 1);
    QCOMPARE(m_helperClient->destroyNullSinkCalls, 1);
    QVERIFY(m_runner->m_sharedStateActive);
    QCOMPARE(m_helperClient->unmountAllCalls, 1);

    m_helperClient->destroyVirtualOutputResult = true;
    m_helperClient->destroyNullSinkResult = true;
    m_helperClient->restoreAllDevicesResult = true;
    m_helperClient->unmountAllResult = 0;
    m_runner->stop();
    QCOMPARE(m_helperClient->restoreAllDevicesCalls, 2);
    QVERIFY(m_runner->m_ownedDevicePaths.isEmpty());
    QVERIFY(m_runner->m_streamingInstances.isEmpty());
    QCOMPARE(m_helperClient->destroyVirtualOutputCalls, 2);
    QCOMPARE(m_helperClient->destroyNullSinkCalls, 2);
    QVERIFY(!m_runner->m_sharedStateActive);
    QCOMPARE(m_helperClient->unmountAllCalls, 2);
    QVERIFY(m_runner->m_steamSharedUsers.isEmpty());
}

QTEST_MAIN(TestSessionRunner)
#include "test_sessionrunner.moc"
