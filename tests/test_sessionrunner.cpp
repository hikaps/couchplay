// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <pwd.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>

#define private public
#include "SessionRunner.h"
#include "PresetManager.h"
#undef private
#include "HeroicConfigManager.h"
#include "SessionManager.h"
#include "SteamConfigManager.h"
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

    struct OverlayCall {
        QString username;
        uint compositorUid;
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
        uint compositorUid;
        QStringList directories;
    };

    struct MirrorCall {
        QString username;
        QString sourceDir;
        QString targetRelativePath;
    };

    QList<AclCall> aclCalls;
    QList<MountCall> mountCalls;
    struct DeviceOwnerCall { QString path; int uid; };
    QList<DeviceOwnerCall> deviceOwnerCalls;
    QList<OverlayCall> overlayCalls;
    QList<CopyDirCall> copyDirCalls;
    QList<MirrorCall> mirrorCalls;

    explicit MockCouchPlayHelperClient(QObject *parent = nullptr)
        : CouchPlayHelperClient(parent)
    {
        m_available = true;
    }

    bool setPathAclWithParents(const QString &path, const QString &username) override
    {
        aclCalls.append({path, username});
        return true;
    }

    bool setupOverlayMount(const QString &username, uint compositorUid, const QString &sourceDir, const QString &targetAlias) override
    {
        overlayCalls.append({username, compositorUid, sourceDir, targetAlias});
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

    int mountSharedDirectories(const QString &username, uint compositorUid, const QStringList &directories) override
    {
        mountCalls.append({username, compositorUid, directories});
        return directories.size();
    }

    int unmountAllCalls = 0;
    int unmountAllSharedDirectories() override
    {
        unmountAllCalls++;
        return 0;
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
    void testSetupDataDirectoriesMirrorsStagedData();
    void testSetupDataDirectoriesLibrarySharingGate();
    void testSetupDataDirectoriesSecondaryLibrariesMounted();
    void testSetupDataDirectoriesHeroicNoConfigBulkCopy();
    void testResolveUserIdentityViaHelper();
    void testResolveUserIdentityFallback();
    void testNaturalExitTearsDownSharingState();
    void testFinalizeDataDirResolvesIdentityViaHelper();
    void testResolveCompositorHomeViaHelper();

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
    QSKIP("Requires D-Bus (m_runner->start()). Will be rewritten in Commit 11 when setupDataDirectories() is implemented.");
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

    QVERIFY(m_runner->setupDataDirectories());

    QCOMPARE(m_helperClient->aclCalls.size(), 1);
    QCOMPARE(m_helperClient->aclCalls[0].path, QStringLiteral("/instance/dir"));
    QCOMPARE(m_helperClient->aclCalls[0].username, QStringLiteral("player1"));
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

    QVERIFY(m_runner->setupDataDirectories());

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

    QVERIFY(m_runner->setupDataDirectories());

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

    QVERIFY(m_runner->setupDataDirectories());

    // Legacy mount spec: "path|" (empty alias => home-relative target)
    QCOMPARE(m_helperClient->mountCalls.size(), 1);
    QCOMPARE(m_helperClient->mountCalls[0].username, QStringLiteral("player1"));
    QCOMPARE(m_helperClient->mountCalls[0].directories,
             (QStringList{QStringLiteral("/home/compositor/.config/game|")}));
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

    QVERIFY(m_runner->setupDataDirectories());

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
    m_runner->setupDataDirectories();
    QCOMPARE(m_helperClient->overlayCalls.size(), 0);

    // Library sharing enabled with no parseable libraries: still no mounts
    // (nothing to share), and crucially no home-relative overlay of the
    // player's Steam root
    steamManager->setShareLibraryEnabled(true);
    m_runner->setupDataDirectories();
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

    m_runner->setupDataDirectories();

    // Both libraries are alias-mounted under ~/.couchplay/steam-libs/<i>;
    // the player's Steam root path is never a mount target
    QCOMPARE(m_helperClient->overlayCalls.size(), 2);
    bool foundPrimary = false;
    bool foundSecondary = false;
    for (const auto &call : m_helperClient->overlayCalls) {
        QVERIFY(!call.targetAlias.isEmpty());
        if (call.sourceDir == steamRoot) {
            foundPrimary = true;
            QCOMPARE(call.targetAlias, QStringLiteral(".couchplay/steam-libs/0"));
        }
        if (call.sourceDir == QStringLiteral("/mnt/steamlibrary")) {
            foundSecondary = true;
            QCOMPARE(call.targetAlias, QStringLiteral(".couchplay/steam-libs/1"));
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
    QVERIFY(!m_runner->setupDataDirectories());

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

    QVERIFY(m_runner->setupDataDirectories());
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
    QVERIFY(m_runner->setupDataDirectories());
    QVERIFY(m_runner->m_sharedStateActive);
    m_runner->stop();
    QCOMPARE(m_helperClient->unmountAllCalls, 2);
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

    // Production sequence: prepare mounts the alias libraries (and loads
    // them); finalize then writes manifests and libraryfolders.vdf
    QVERIFY(steamManager->prepareDataDir(dir, QStringLiteral("player1")));
    QVERIFY(steamManager->finalizeDataDir(dir, QStringLiteral("player1")));

    // Manifests and libraryfolders.vdf landed under the helper-resolved home
    QVERIFY(QFile::exists(homeDir.path() + QStringLiteral("/.couchplay/steam-libs/1/appmanifest_730.acf")));
    QVERIFY(QFile::exists(steamRoot + QStringLiteral("/config/libraryfolders.vdf")));
    QFile vdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QVERIFY(vdf.open(QIODevice::ReadOnly));
    const QByteArray vdfContent = vdf.readAll();
    QVERIFY(vdfContent.contains(".couchplay/steam-libs"));
    QVERIFY(!vdfContent.contains("extlib")); // only alias paths + the player's own root
}

QTEST_MAIN(TestSessionRunner)
#include "test_sessionrunner.moc"
