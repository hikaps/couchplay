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

    QList<AclCall> aclCalls;
    QList<MountCall> mountCalls;
    struct DeviceOwnerCall { QString path; int uid; };
    QList<DeviceOwnerCall> deviceOwnerCalls;
    QList<OverlayCall> overlayCalls;
    QList<CopyDirCall> copyDirCalls;

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

    int mountSharedDirectories(const QString &username, uint compositorUid, const QStringList &directories) override
    {
        Q_UNUSED(username)
        Q_UNUSED(compositorUid)
        Q_UNUSED(directories)
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
            info.insert(QStringLiteral("home"), QStringLiteral("/home/player1"));
        }
        return info;
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
    void testSetupDataDirectoriesLibrarySharingGate();
    void testSetupDataDirectoriesSecondaryLibrariesMounted();
    void testSetupDataDirectoriesHeroicNoConfigBulkCopy();
    void testResolveUserIdentityViaHelper();
    void testResolveUserIdentityFallback();

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

    // Library sharing disabled: the steamRoot overlay must NOT be mounted
    m_runner->setupDataDirectories();
    QCOMPARE(m_helperClient->overlayCalls.size(), 0);

    // Library sharing enabled: the steamRoot overlay is mounted. (The overall
    // result may be false — this fixture has no parseable libraries, so the
    // prepare/finalize steps log failures; the gate is what is under test.)
    steamManager->setShareLibraryEnabled(true);
    m_runner->setupDataDirectories();
    QCOMPARE(m_helperClient->overlayCalls.size(), 1);
    QCOMPARE(m_helperClient->overlayCalls[0].sourceDir, steamRoot);
    QCOMPARE(m_helperClient->overlayCalls[0].username, QStringLiteral("player1"));
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

    // Primary root via the dir list + secondary library via prepareDataDir's
    // alias mount, at the path libraryfolders.vdf advertises for the player
    QCOMPARE(m_helperClient->overlayCalls.size(), 2);
    QCOMPARE(m_helperClient->overlayCalls[0].sourceDir, steamRoot);
    QCOMPARE(m_helperClient->overlayCalls[1].sourceDir, QStringLiteral("/mnt/steamlibrary"));
    QCOMPARE(m_helperClient->overlayCalls[1].targetAlias, QStringLiteral(".couchplay/steam-libs/1"));
    QCOMPARE(m_helperClient->overlayCalls[1].username, QStringLiteral("player1"));
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

QTEST_MAIN(TestSessionRunner)
#include "test_sessionrunner.moc"
