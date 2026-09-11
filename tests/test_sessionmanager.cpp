// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <KConfig>
#include <KConfigGroup>

#include "PresetManager.h"
#include "SessionManager.h"
#include "SteamConfigManager.h"

#define KEY(x) QStringLiteral(x)

class TestSessionManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality tests
    void testInitialization();
    void testNewSession();
    void testInstanceCount();
    void testCurrentLayout();

    // Instance configuration tests
    void testGetInstanceConfig();
    void testSetInstanceConfig();
    void testSetInstanceResolution();
    void testRecalculateOutputResolutionsHorizontal();
    void testRecalculateOutputResolutionsVertical();
    void testSetInstanceUser();

    // Profile management tests
    void testSaveProfile();
    void testLoadProfile();
    void testLoadProfileLegacySharedDirectories();
    void testLoadSaveDataDirectoriesRoundtrip();
    void testUnsnapshottedStaysUnsnapshottedAfterSaveLoad();
    void testPlayerDataFolderPath();
    void testPlayerDataFolderPathMarkerPredicate();
    void testSetInstanceConfigPreservesDirectorySnapshot();
    void testDeleteProfile();
    void testSavedProfiles();
    void testRefreshProfiles();

    // Signal tests
    void testInstanceCountChangedSignal();
    void testCurrentLayoutChangedSignal();
    void testProfilesChangedSignal();

    // User assignment tests
    void testGetAssignedUsers();

private:
    SessionManager *m_sessionManager = nullptr;
    QTemporaryDir *m_tempDir = nullptr;
};

void TestSessionManager::initTestCase()
{
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
}

void TestSessionManager::cleanupTestCase()
{
    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestSessionManager::init()
{
    m_sessionManager = new SessionManager(this);
}

void TestSessionManager::cleanup()
{
    delete m_sessionManager;
    m_sessionManager = nullptr;
}

void TestSessionManager::testInitialization()
{
    QVERIFY(m_sessionManager != nullptr);
    QCOMPARE(m_sessionManager->instanceCount(), 2);
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("horizontal"));
    QVERIFY(m_sessionManager->currentProfileName().isEmpty());
}

void TestSessionManager::testNewSession()
{
    m_sessionManager->setInstanceCount(3);
    m_sessionManager->setCurrentLayout(QStringLiteral("vertical"));

    m_sessionManager->newSession();

    QCOMPARE(m_sessionManager->instanceCount(), 2);
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("horizontal"));
    QVERIFY(m_sessionManager->currentProfileName().isEmpty());
}

void TestSessionManager::testInstanceCount()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::instanceCountChanged);

    m_sessionManager->setInstanceCount(3);
    QCOMPARE(m_sessionManager->instanceCount(), 3);
    QCOMPARE(spy.count(), 1);

    m_sessionManager->setInstanceCount(4);
    QCOMPARE(m_sessionManager->instanceCount(), 4);

    m_sessionManager->setInstanceCount(1);
    QVERIFY(m_sessionManager->instanceCount() >= 2);
}

void TestSessionManager::testCurrentLayout()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::currentLayoutChanged);

    m_sessionManager->setCurrentLayout(QStringLiteral("vertical"));
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("vertical"));
    QCOMPARE(spy.count(), 1);

    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("grid"));

    m_sessionManager->setCurrentLayout(QStringLiteral("multi-monitor"));
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("multi-monitor"));
}

void TestSessionManager::testGetInstanceConfig()
{
    QVariantMap config = m_sessionManager->getInstanceConfig(0);
    QVERIFY(config.contains(KEY("internalWidth")));
    QVERIFY(config.contains(KEY("internalHeight")));
    QVERIFY(config.contains(KEY("refreshRate")));

    QVariantMap invalidConfig = m_sessionManager->getInstanceConfig(-1);
    QVERIFY(invalidConfig.isEmpty());

    QVariantMap outOfBoundsConfig = m_sessionManager->getInstanceConfig(10);
    QVERIFY(outOfBoundsConfig.isEmpty());
}

void TestSessionManager::testSetInstanceConfig()
{
    QVariantMap config;
    config.insert(KEY("internalWidth"), 1280);
    config.insert(KEY("internalHeight"), 720);
    config.insert(KEY("refreshRate"), 120);

    m_sessionManager->setInstanceConfig(0, config);

    QVariantMap retrieved = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(retrieved.value(KEY("internalWidth")).toInt(), 1280);
    QCOMPARE(retrieved.value(KEY("internalHeight")).toInt(), 720);
    QCOMPARE(retrieved.value(KEY("refreshRate")).toInt(), 120);
}

void TestSessionManager::testSetInstanceConfigPreservesDirectorySnapshot()
{
    // QML round-trips the whole config map for unrelated edits (codec,
    // refresh rate…). The map always carries dataDirectories (empty while
    // unsnapshotted), so applying it would convert the instance into an
    // explicit empty snapshot and suppress the preset's directories.
    m_sessionManager->setInstanceCount(1);

    QVariantMap config = m_sessionManager->getInstanceConfig(0);
    config[KEY("streamCodec")] = QStringLiteral("h265");
    m_sessionManager->setInstanceConfig(0, config);
    QVariantMap after = m_sessionManager->getInstanceConfig(0);
    QVERIFY(!after.value(KEY("dataDirectoriesSnapshotted")).toBool());
    QCOMPARE(after.value(KEY("dataDirectories")).toList().size(), 0);
    QCOMPARE(after.value(KEY("streamCodec")).toString(), QStringLiteral("h265"));

    // An existing explicit snapshot must survive the same round-trip
    QVariantList dirs;
    QVariantMap overlayDir;
    overlayDir[KEY("path")] = QStringLiteral("/home/compositor/Games/MyGame");
    overlayDir[KEY("mode")] = QStringLiteral("overlay");
    dirs.append(overlayDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    config = m_sessionManager->getInstanceConfig(0);
    config[KEY("refreshRate")] = 144;
    m_sessionManager->setInstanceConfig(0, config);
    after = m_sessionManager->getInstanceConfig(0);
    QVERIFY(after.value(KEY("dataDirectoriesSnapshotted")).toBool());
    QCOMPARE(after.value(KEY("dataDirectories")).toList().size(), 1);
    QCOMPARE(after.value(KEY("refreshRate")).toInt(), 144);
}

void TestSessionManager::testSetInstanceResolution()
{
    m_sessionManager->setInstanceResolution(0, 2560, 1440, 1920, 1080);

    QVariantMap config = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(config.value(KEY("internalWidth")).toInt(), 2560);
    QCOMPARE(config.value(KEY("internalHeight")).toInt(), 1440);
    QCOMPARE(config.value(KEY("outputWidth")).toInt(), 1920);
    QCOMPARE(config.value(KEY("outputHeight")).toInt(), 1080);
}

void TestSessionManager::testRecalculateOutputResolutionsHorizontal()
{
    // Internal matches output so the game adapts its UI to the split-screen aspect ratio
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setCurrentLayout(QStringLiteral("horizontal"));

    QSignalSpy spy(m_sessionManager, &SessionManager::instancesChanged);
    m_sessionManager->recalculateOutputResolutions(1920, 1080);
    QVERIFY(spy.count() >= 1);

    QVariantMap config0 = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(config0.value(KEY("outputWidth")).toInt(), 960);
    QCOMPARE(config0.value(KEY("outputHeight")).toInt(), 1080);
    QCOMPARE(config0.value(KEY("internalWidth")).toInt(), 960);
    QCOMPARE(config0.value(KEY("internalHeight")).toInt(), 1080);
}

void TestSessionManager::testRecalculateOutputResolutionsVertical()
{
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setCurrentLayout(QStringLiteral("vertical"));

    m_sessionManager->recalculateOutputResolutions(1920, 1080);

    QVariantMap config0 = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(config0.value(KEY("outputWidth")).toInt(), 1920);
    QCOMPARE(config0.value(KEY("outputHeight")).toInt(), 540);
    QCOMPARE(config0.value(KEY("internalWidth")).toInt(), 1920);
    QCOMPARE(config0.value(KEY("internalHeight")).toInt(), 540);
}

void TestSessionManager::testSetInstanceUser()
{
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));

    QVariantMap config = m_sessionManager->getInstanceConfig(1);
    QCOMPARE(config.value(KEY("username")).toString(), QStringLiteral("player2"));

    m_sessionManager->setInstanceUser(1, QString());
    config = m_sessionManager->getInstanceConfig(1);
    QVERIFY(config.value(KEY("username")).toString().isEmpty());
}

void TestSessionManager::testSaveProfile()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);

    m_sessionManager->setInstanceCount(3);
    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));

    bool result = m_sessionManager->saveProfile(QStringLiteral("TestProfile"));
    QCOMPARE(result, true);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(m_sessionManager->currentProfileName(), QStringLiteral("TestProfile"));
}

void TestSessionManager::testLoadProfile()
{
    m_sessionManager->setInstanceCount(4);
    m_sessionManager->setCurrentLayout(QStringLiteral("vertical"));
    m_sessionManager->saveProfile(QStringLiteral("LoadTestProfile"));

    m_sessionManager->newSession();
    QCOMPARE(m_sessionManager->instanceCount(), 2);

    bool result = m_sessionManager->loadProfile(QStringLiteral("LoadTestProfile"));
    QCOMPARE(result, true);
    QCOMPARE(m_sessionManager->instanceCount(), 4);
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("vertical"));
    QCOMPARE(m_sessionManager->currentProfileName(), QStringLiteral("LoadTestProfile"));

    result = m_sessionManager->loadProfile(QStringLiteral("NonExistentProfile"));
    QCOMPARE(result, false);
}

void TestSessionManager::testLoadProfileLegacySharedDirectories()
{
    // Hand-write a profile in the legacy format: sharedDirectories as a plain
    // QStringList (paths only), no dataDirectories key
    QString profilesDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles");
    QDir().mkpath(profilesDir);
    QString path = profilesDir + QStringLiteral("/LegacySharedDirsProfile.conf");

    {
        KConfig config(path, KConfig::SimpleConfig);
        KConfigGroup general = config.group(QStringLiteral("General"));
        general.writeEntry("instanceCount", 1);
        KConfigGroup inst = config.group(QStringLiteral("Instance0"));
        inst.writeEntry("username", QStringLiteral("player1"));
        inst.writeEntry("presetId", QStringLiteral("steam"));
        inst.writeEntry("sharedDirectories",
                        QStringList{QStringLiteral("/home/compositor/Games"),
                                    QStringLiteral("/home/compositor/Saves")});
        config.sync();
    }

    bool result = m_sessionManager->loadProfile(QStringLiteral("LegacySharedDirsProfile"));
    QVERIFY(result);

    QVariantMap configMap = m_sessionManager->getInstanceConfig(0);
    QVariantList dirs = configMap[QStringLiteral("dataDirectories")].toList();
    QCOMPARE(dirs.size(), 2);
    QCOMPARE(dirs[0].toMap()[QStringLiteral("path")].toString(), QStringLiteral("/home/compositor/Games"));
    QCOMPARE(dirs[0].toMap()[QStringLiteral("mode")].toString(), QStringLiteral("bind"));
    QCOMPARE(dirs[1].toMap()[QStringLiteral("path")].toString(), QStringLiteral("/home/compositor/Saves"));
    QCOMPARE(dirs[1].toMap()[QStringLiteral("mode")].toString(), QStringLiteral("bind"));

    m_sessionManager->deleteProfile(QStringLiteral("LegacySharedDirsProfile"));
}

void TestSessionManager::testLoadSaveDataDirectoriesRoundtrip()
{
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));

    QVariantList dirs;
    QVariantMap dir1;
    dir1[QStringLiteral("path")] = QStringLiteral("/home/compositor/Games");
    dir1[QStringLiteral("mode")] = QStringLiteral("copy");
    QVariantMap dir2;
    dir2[QStringLiteral("path")] = QStringLiteral("/home/compositor/Steam");
    dir2[QStringLiteral("mode")] = QStringLiteral("overlay");
    dirs.append(dir1);
    dirs.append(dir2);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("DataDirRoundtripProfile")));

    m_sessionManager->newSession();
    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("DataDirRoundtripProfile")));

    QVariantList restored = m_sessionManager->getInstanceConfig(0)[QStringLiteral("dataDirectories")].toList();
    QCOMPARE(restored.size(), 2);
    QCOMPARE(restored[0].toMap()[QStringLiteral("path")].toString(), QStringLiteral("/home/compositor/Games"));
    QCOMPARE(restored[0].toMap()[QStringLiteral("mode")].toString(), QStringLiteral("copy"));
    QCOMPARE(restored[1].toMap()[QStringLiteral("path")].toString(), QStringLiteral("/home/compositor/Steam"));
    QCOMPARE(restored[1].toMap()[QStringLiteral("mode")].toString(), QStringLiteral("overlay"));

    m_sessionManager->deleteProfile(QStringLiteral("DataDirRoundtripProfile"));
}

void TestSessionManager::testUnsnapshottedStaysUnsnapshottedAfterSaveLoad()
{
    // An instance that never had a snapshot taken must survive a save/load
    // round-trip without being converted into an explicit empty snapshot
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("steam"));
    // No setInstanceDataDirectories call: unsnapshotted, uses preset defaults

    QCOMPARE(m_sessionManager->getInstanceConfig(0)[QStringLiteral("dataDirectoriesSnapshotted")].toBool(), false);
    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("UnsnapshottedProfile")));

    m_sessionManager->newSession();
    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("UnsnapshottedProfile")));

    QVariantMap config = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(config[QStringLiteral("dataDirectoriesSnapshotted")].toBool(), false);
    QVERIFY(config[QStringLiteral("dataDirectories")].toList().isEmpty());

    m_sessionManager->deleteProfile(QStringLiteral("UnsnapshottedProfile"));
}

void TestSessionManager::testPlayerDataFolderPath()
{
    // Invalid index and missing user yield an empty path
    QCOMPARE(m_sessionManager->playerDataFolderPath(99), QString());
    m_sessionManager->setInstanceCount(1);
    QCOMPARE(m_sessionManager->playerDataFolderPath(0), QString());

    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("custom-staging"));

    QVariantList dirs;
    QVariantMap overlayDir;
    overlayDir[QStringLiteral("path")] = QStringLiteral("/home/compositor/Games/MyGame");
    overlayDir[QStringLiteral("mode")] = QStringLiteral("overlay");
    QVariantMap aclDir;
    aclDir[QStringLiteral("path")] = QStringLiteral("/home/compositor/ReadOnly");
    aclDir[QStringLiteral("mode")] = QStringLiteral("acl");
    dirs.append(overlayDir);
    dirs.append(aclDir);
    m_sessionManager->setInstanceDataDirectories(0, dirs);

    QString expectedRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/player-data/custom-staging/player1");
    QCOMPARE(m_sessionManager->playerDataFolderPath(0), expectedRoot);
    QVERIFY(QDir(expectedRoot).exists());

    // A staging subfolder exists per writable dir; read-only dirs get none.
    // Slugs carry a hash suffix, so assert on the readable prefix.
    QDir rootDir(expectedRoot);
    const QStringList slugs = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(slugs.size(), 1);
    QVERIFY(slugs.first().contains(QStringLiteral("Games_MyGame-")));

    QDir(expectedRoot).removeRecursively();
}

void TestSessionManager::testPlayerDataFolderPathMarkerPredicate()
{
    // The Steam-root overlay marker is excluded from staging folders only
    // under the exact runtime predicate (Steam launcher + overlay mode +
    // detected Steam root); anything else keeps its folder
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    const QByteArray originalHome = qgetenv("HOME");
    qputenv("HOME", homeDir.path().toLocal8Bit());
    QStandardPaths::setTestModeEnabled(true);

    // Fake a detected Steam installation
    const QString steamRoot = homeDir.path() + QStringLiteral("/.steam/steam");
    QVERIFY(QDir(steamRoot).mkpath(QStringLiteral("config")));
    {
        QFile libraryVdf(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
        QVERIFY(libraryVdf.open(QIODevice::WriteOnly));
        libraryVdf.write("\"libraryfolders\"\n{\n}\n");
    }

    SteamConfigManager steamManager;
    QVERIFY(steamManager.isSteamDetected());
    QCOMPARE(steamManager.steamPaths().steamRoot, steamRoot);
    PresetManager presetManager;
    presetManager.setSteamConfigManager(&steamManager);
    m_sessionManager->setPresetManager(&presetManager);

    QVariantMap markerDir; // overlay at the detected Steam root: library-sharing marker
    markerDir[QStringLiteral("path")] = steamRoot;
    markerDir[QStringLiteral("mode")] = QStringLiteral("overlay");
    QVariantMap copyRootDir; // same path in copy mode: ordinary private directory
    copyRootDir[QStringLiteral("path")] = steamRoot;
    copyRootDir[QStringLiteral("mode")] = QStringLiteral("copy");

    auto stagingDirCount = [this](const QString &presetId, const QVariantList &dirs) -> int {
        m_sessionManager->setInstanceCount(1);
        m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
        m_sessionManager->setInstancePreset(0, presetId);
        m_sessionManager->setInstanceDataDirectories(0, dirs);
        const QString root = m_sessionManager->playerDataFolderPath(0);
        if (root.isEmpty()) {
            return -1;
        }
        const int count = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        QDir(root).removeRecursively();
        return count;
    };

    // Compute everything first so a failed assertion can't leak the patched env
    const int steamOverlayCount = stagingDirCount(QStringLiteral("steam"), QVariantList{markerDir});
    const int steamCopyCount = stagingDirCount(QStringLiteral("steam"), QVariantList{copyRootDir});
    const QString customId =
        presetManager.addCustomPreset(QStringLiteral("Marker Test"), QStringLiteral("/usr/bin/game"));
    const int customOverlayCount = stagingDirCount(customId, QVariantList{markerDir});

    QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/player-data"))
        .removeRecursively();
    QStandardPaths::setTestModeEnabled(false);
    if (!originalHome.isNull()) {
        qputenv("HOME", originalHome);
    } else {
        qunsetenv("HOME");
    }

    QCOMPARE(steamOverlayCount, 0); // marker: no staging folder
    QCOMPARE(steamCopyCount, 1); // copy mode at the same path: staged
    QCOMPARE(customOverlayCount, 1); // non-Steam preset: staged
}

void TestSessionManager::testDeleteProfile()
{
    m_sessionManager->saveProfile(QStringLiteral("DeleteTestProfile"));

    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);

    bool result = m_sessionManager->deleteProfile(QStringLiteral("DeleteTestProfile"));
    QCOMPARE(result, true);
    QCOMPARE(spy.count(), 1);
    QVERIFY(m_sessionManager->currentProfileName().isEmpty());

    result = m_sessionManager->deleteProfile(QStringLiteral("NonExistentProfile"));
    QCOMPARE(result, false);
}

void TestSessionManager::testSavedProfiles()
{
    QList<SessionProfile> profiles = m_sessionManager->savedProfiles();
    for (const SessionProfile &profile : profiles) {
        m_sessionManager->deleteProfile(profile.name);
    }

    profiles = m_sessionManager->savedProfiles();
    int initialCount = profiles.size();

    m_sessionManager->saveProfile(QStringLiteral("Profile1"));
    m_sessionManager->saveProfile(QStringLiteral("Profile2"));

    profiles = m_sessionManager->savedProfiles();
    QCOMPARE(profiles.size(), initialCount + 2);

    bool foundProfile1 = false;
    bool foundProfile2 = false;
    for (const SessionProfile &profile : profiles) {
        if (profile.name == QStringLiteral("Profile1")) {
            foundProfile1 = true;
        }
        if (profile.name == QStringLiteral("Profile2")) {
            foundProfile2 = true;
        }
    }
    QVERIFY(foundProfile1);
    QVERIFY(foundProfile2);
}

void TestSessionManager::testRefreshProfiles()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);

    m_sessionManager->refreshProfiles();
    QCOMPARE(spy.count(), 1);
}

void TestSessionManager::testInstanceCountChangedSignal()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::instanceCountChanged);

    m_sessionManager->setInstanceCount(3);
    QCOMPARE(spy.count(), 1);
    m_sessionManager->setInstanceCount(3);
    QCOMPARE(spy.count(), 1);
}

void TestSessionManager::testCurrentLayoutChangedSignal()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::currentLayoutChanged);

    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));
    QCOMPARE(spy.count(), 1);
    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));
    QCOMPARE(spy.count(), 1);
}

void TestSessionManager::testProfilesChangedSignal()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);

    m_sessionManager->saveProfile(QStringLiteral("SignalTestProfile"));
    QCOMPARE(spy.count(), 1);

    m_sessionManager->deleteProfile(QStringLiteral("SignalTestProfile"));
    QCOMPARE(spy.count(), 2);
}

void TestSessionManager::testGetAssignedUsers()
{
    m_sessionManager->newSession();
    m_sessionManager->setInstanceCount(3);

    m_sessionManager->setInstanceUser(0, QString());
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstanceUser(2, QStringLiteral("player3"));

    QStringList assigned = m_sessionManager->getAssignedUsers(0);
    QCOMPARE(assigned.size(), 2);
    QVERIFY(assigned.contains(QStringLiteral("player2")));
    QVERIFY(assigned.contains(QStringLiteral("player3")));

    assigned = m_sessionManager->getAssignedUsers(1);
    QCOMPARE(assigned.size(), 1);
    QVERIFY(assigned.contains(QStringLiteral("player3")));

    assigned = m_sessionManager->getAssignedUsers(2);
    QCOMPARE(assigned.size(), 1);
    QVERIFY(assigned.contains(QStringLiteral("player2")));

    m_sessionManager->setInstanceUser(1, QString());
    assigned = m_sessionManager->getAssignedUsers(0);
    QCOMPARE(assigned.size(), 1);
    QVERIFY(assigned.contains(QStringLiteral("player3")));
}

QTEST_MAIN(TestSessionManager)
#include "test_sessionmanager.moc"
