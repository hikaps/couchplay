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

// First: the client header under the test-access macro, before any other
// project header can pull it in unguarded
#define private public
#include "CouchPlayHelperClient.h"
#undef private

#include "PresetManager.h"
#include "SessionManager.h"
#include "SteamConfigManager.h"

#define KEY(x) QStringLiteral(x)

class MockHomeHelperClient : public CouchPlayHelperClient
{
    Q_OBJECT
public:
    explicit MockHomeHelperClient(QObject *parent = nullptr)
        : CouchPlayHelperClient(parent)
    {
        m_available = true;
    }

    QString uidHome;
    QVariantMap getUserHomeInfoByUid(uint uid) override
    {
        Q_UNUSED(uid)
        return {{QStringLiteral("home"), uidHome}, {QStringLiteral("canonicalHome"), uidHome}};
    }
};

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
    void testPrivateTemporaryDirectoriesRoundtrip();
    void testLoadProfileLegacySharedDirectories();
    void testMigrationPreservesExistingBackup();
    void testMigrationUnsafeBackupLeavesStateUnchanged();
    void testLegacyMigrationOnSaveAndDuplicate_data();
    void testLegacyMigrationOnSaveAndDuplicate();
    void testLegacyOverrideSettingsAreDiscardedOnSave();
    void testApplicationDirectoryEditsAffectLoadedProfile();
    void testPlayerDataFolderPath();
    void testPlayerDataFolderEligibilityTransitions();
    void testPlayerDataFolderPathMarkerPredicate();
    void testInstanceConfigDoesNotOwnApplicationDirectories();
    void testPlayerDataFolderPathUsesHelperResolvedHome();
    void testDeleteProfile();
    void testSavedProfiles();
    void testRefreshProfiles();
    void testProfileHooksAndGameSelectionRoundtrip();
    void testProfileHookPathsPreserveWhitespace();
    void testDuplicateProfilePreservesData();
    void testProfileNameValidation();

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

void TestSessionManager::testPlayerDataFolderPathUsesHelperResolvedHome()
{
    // The staging slug must use the helper-resolved compositor home so the
    // folder names match what SessionRunner mirrors into at session start
    // (under Flatpak the sandbox resolves a different home)
    MockHomeHelperClient helper;
    helper.uidHome = QStringLiteral("/home/compositor");
    m_sessionManager->setHelperClient(&helper);
    PresetManager presetManager;
    m_sessionManager->setPresetManager(&presetManager);
    const QString presetId = presetManager.addCustomPreset(QStringLiteral("staging"), QStringLiteral("/usr/bin/game"));
    QVERIFY(!presetId.isEmpty());
    QVERIFY(presetManager.setDataDirectories(presetId, {
        QVariantMap{{KEY("path"), QStringLiteral("/home/compositor/Games/MyGame")},
                    {KEY("mode"), QStringLiteral("overlay")}}
    }));

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, presetId);

    const QString root = m_sessionManager->playerDataFolderPath(0);
    QVERIFY(!root.isEmpty());
    QDir rootDir(root);
    const QStringList slugs = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(slugs.size(), 1);
    QVERIFY(slugs.first().startsWith(QStringLiteral("Games_MyGame-"))); // home-relative slug

    rootDir.removeRecursively();
    QDir(QDir::homePath() + QStringLiteral("/.local/share/couchplay/player-data")).removeRecursively();
}

void TestSessionManager::testInstanceConfigDoesNotOwnApplicationDirectories()
{
    m_sessionManager->setInstanceCount(1);
    PresetManager presetManager;
    m_sessionManager->setPresetManager(&presetManager);
    const QString presetId = presetManager.addCustomPreset(QStringLiteral("authoritative"),
                                                          QStringLiteral("/usr/bin/game"));
    QVERIFY(!presetId.isEmpty());
    m_sessionManager->setInstancePreset(0, presetId);
    const QVariantMap directory{{KEY("path"), QStringLiteral("/data/first")},
                                {KEY("mode"), QStringLiteral("copy")}};
    QVERIFY(presetManager.setDataDirectories(presetId, {directory}));

    QVariantMap config = m_sessionManager->getInstanceConfig(0);
    QVERIFY(!config.contains(KEY("dataDirectories")));
    QVERIFY(!config.contains(KEY("dataDirectoriesSnapshotted")));
    config[KEY("streamCodec")] = QStringLiteral("h265");
    m_sessionManager->setInstanceConfig(0, config);
    QCOMPARE(presetManager.getDataDirectories(presetId).size(), 1);

    QVERIFY(presetManager.setDataDirectories(presetId, {}));
    QCOMPARE(presetManager.getDataDirectories(presetId).size(), 0);
    config = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(config.value(KEY("streamCodec")).toString(), QStringLiteral("h265"));
    QVERIFY(!config.contains(KEY("dataDirectories")));
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

void TestSessionManager::testPrivateTemporaryDirectoriesRoundtrip()
{
    m_sessionManager->setInstanceCount(2);

    QVariantMap playerOne = m_sessionManager->getInstanceConfig(0);
    playerOne[KEY("privateTemporaryDirectories")] = true;
    m_sessionManager->setInstanceConfig(0, playerOne);

    QVariantMap playerTwo = m_sessionManager->getInstanceConfig(1);
    playerTwo[KEY("privateTemporaryDirectories")] = true;
    m_sessionManager->setInstanceConfig(1, playerTwo);
    playerTwo = m_sessionManager->getInstanceConfig(1);
    playerTwo[KEY("privateTemporaryDirectories")] = false;
    playerTwo[KEY("streamCodec")] = QStringLiteral("h265");
    m_sessionManager->setInstanceConfig(1, playerTwo);

    playerOne = m_sessionManager->getInstanceConfig(0);
    playerOne[KEY("refreshRate")] = 144;
    m_sessionManager->setInstanceConfig(0, playerOne);

    QCOMPARE(m_sessionManager->getInstanceConfig(0).value(KEY("privateTemporaryDirectories")).toBool(), true);
    QCOMPARE(m_sessionManager->getInstanceConfig(1).value(KEY("privateTemporaryDirectories")).toBool(), false);
    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("PrivateTemporaryDirectoriesProfile")));

    m_sessionManager->newSession();
    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("PrivateTemporaryDirectoriesProfile")));

    const QVariantMap restoredPlayerOne = m_sessionManager->getInstanceConfig(0);
    const QVariantMap restoredPlayerTwo = m_sessionManager->getInstanceConfig(1);
    QCOMPARE(restoredPlayerOne.value(KEY("privateTemporaryDirectories")).toBool(), true);
    QCOMPARE(restoredPlayerTwo.value(KEY("privateTemporaryDirectories")).toBool(), false);
    QCOMPARE(restoredPlayerOne.value(KEY("refreshRate")).toInt(), 144);
    QCOMPARE(restoredPlayerTwo.value(KEY("streamCodec")).toString(), QStringLiteral("h265"));

    m_sessionManager->deleteProfile(QStringLiteral("PrivateTemporaryDirectoriesProfile"));
}

void TestSessionManager::testLoadProfileLegacySharedDirectories()
{
    const QString profilesDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles");
    QDir().mkpath(profilesDir);
    const QString path = profilesDir + QStringLiteral("/LegacySharedDirsProfile.conf");
    {
        KConfig config(path, KConfig::SimpleConfig);
        config.group(QStringLiteral("General")).writeEntry("instanceCount", 1);
        KConfigGroup inst = config.group(QStringLiteral("Instance0"));
        inst.writeEntry("username", QStringLiteral("player1"));
        inst.writeEntry("presetId", QStringLiteral("steam"));
        inst.writeEntry("sharedDirectories", QStringList{QStringLiteral("/home/compositor/Games")});
        inst.writeEntry("dataDirectories", QStringLiteral("legacy payload"));
        inst.writeEntry("unrelatedKey", QStringLiteral("preserve me"));
        config.group(QStringLiteral("UnknownGroup")).writeEntry("unknown", 42);
        config.sync();
    }
    QFile originalFile(path);
    QVERIFY(originalFile.open(QIODevice::ReadOnly));
    const QByteArray original = originalFile.readAll();
    originalFile.close();

    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("LegacySharedDirsProfile")));
    const QVariantMap instance = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(instance.value(KEY("privateTemporaryDirectories")).toBool(), false);
    QVERIFY(!instance.contains(KEY("dataDirectories")));
    QVERIFY(!instance.contains(KEY("dataDirectoriesSnapshotted")));

    const QString backupPath = path + QStringLiteral(".pre-directory-snapshot.bak");
    QFile backup(backupPath);
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), original);
    backup.close();
    KConfig migrated(path, KConfig::SimpleConfig);
    const KConfigGroup migratedInstance = migrated.group(QStringLiteral("Instance0"));
    QVERIFY(!migratedInstance.hasKey("sharedDirectories"));
    QVERIFY(!migratedInstance.hasKey("dataDirectories"));
    QCOMPARE(migratedInstance.readEntry("unrelatedKey", QString()), QStringLiteral("preserve me"));
    QCOMPARE(migrated.group(QStringLiteral("UnknownGroup")).readEntry("unknown", 0), 42);

    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("LegacySharedDirsProfile")));
    QVERIFY(!QFile::exists(backupPath + QStringLiteral(".2")));
    QVERIFY(m_sessionManager->deleteProfile(QStringLiteral("LegacySharedDirsProfile")));
    QVERIFY(QFile::remove(backupPath));
}

void TestSessionManager::testMigrationPreservesExistingBackup()
{
    const QString name = QStringLiteral("BackupCollision");
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles/") + name + QStringLiteral(".conf");
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    const QByteArray original("[General]\ninstanceCount=1\n[Instance0]\nusername=player1\npresetId=steam\ndataDirectories=legacy\n");
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), qint64(original.size()));
    source.close();
    const QString backupPath = path + QStringLiteral(".pre-directory-snapshot.bak");
    QFile previousBackup(backupPath);
    QVERIFY(previousBackup.open(QIODevice::WriteOnly));
    QCOMPARE(previousBackup.write("previous profile"), qint64(16));
    previousBackup.close();
    QVERIFY(m_sessionManager->loadProfile(name));
    QVERIFY(previousBackup.open(QIODevice::ReadOnly));
    QCOMPARE(previousBackup.readAll(), QByteArray("previous profile"));
    previousBackup.close();
    QFile newBackup(backupPath + QStringLiteral(".2"));
    QVERIFY(newBackup.open(QIODevice::ReadOnly));
    QCOMPARE(newBackup.readAll(), original);
    newBackup.close();
    QVERIFY(m_sessionManager->loadProfile(name));
    QVERIFY(!QFile::exists(backupPath + QStringLiteral(".3")));
    QVERIFY(m_sessionManager->deleteProfile(name));
    QVERIFY(QFile::remove(backupPath));
    QVERIFY(QFile::remove(newBackup.fileName()));
}

void TestSessionManager::testMigrationUnsafeBackupLeavesStateUnchanged()
{
    const QString name = QStringLiteral("UnsafeBackup");
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles/") + name + QStringLiteral(".conf");
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    const QByteArray original("[General]\ninstanceCount=1\n[Instance0]\nusername=player1\ndataDirectories=legacy\n");
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), qint64(original.size()));
    source.close();
    const QString backupPath = path + QStringLiteral(".pre-directory-snapshot.bak");
    QVERIFY(QFile::link(path, backupPath));
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("unchanged-player"));
    QSignalSpy errors(m_sessionManager, &SessionManager::errorOccurred);
    QVERIFY(!m_sessionManager->loadProfile(name));
    QCOMPARE(errors.count(), 1);
    QCOMPARE(m_sessionManager->getInstanceConfig(0).value(KEY("username")).toString(),
             QStringLiteral("unchanged-player"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), original);
    source.close();
    QVERIFY(QFile::remove(backupPath));
    QVERIFY(QFile::remove(path));
}

void TestSessionManager::testLegacyMigrationOnSaveAndDuplicate_data()
{
    QTest::addColumn<bool>("duplicate");
    QTest::newRow("save") << false;
    QTest::newRow("duplicate") << true;
}

void TestSessionManager::testLegacyMigrationOnSaveAndDuplicate()
{
    QFETCH(bool, duplicate);
    const QString name = QStringLiteral("LegacyMutation");
    const QString profiles = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles");
    QVERIFY(QDir().mkpath(profiles));
    const QString path = profiles + QLatin1Char('/') + name + QStringLiteral(".conf");
    const QByteArray original("[General]\ninstanceCount=1\n[Instance0]\nusername=player1\npresetId=steam\nsharedDirectories=/source\n[UnknownGroup]\nretained=value\n");
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), qint64(original.size()));
    source.close();
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player2"));
    QString resultName = name;
    if (duplicate) {
        resultName = m_sessionManager->duplicateProfile(name);
        QVERIFY(!resultName.isEmpty());
    } else {
        QVERIFY(m_sessionManager->saveProfile(name));
    }
    QFile backup(path + QStringLiteral(".pre-directory-snapshot.bak"));
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), original);
    backup.close();
    KConfig result(profiles + QLatin1Char('/') + resultName + QStringLiteral(".conf"), KConfig::SimpleConfig);
    QVERIFY(!result.group(QStringLiteral("Instance0")).hasKey("sharedDirectories"));
    QCOMPARE(result.group(QStringLiteral("UnknownGroup")).readEntry("retained", QString()), QStringLiteral("value"));
    QCOMPARE(result.group(QStringLiteral("Instance0")).readEntry("username", QString()),
             duplicate ? QStringLiteral("player1") : QStringLiteral("player2"));
    if (duplicate) {
        QVERIFY(m_sessionManager->deleteProfile(resultName));
    }
    QVERIFY(m_sessionManager->deleteProfile(name));
    QVERIFY(QFile::remove(backup.fileName()));
}

void TestSessionManager::testLegacyOverrideSettingsAreDiscardedOnSave()
{
    QTemporaryDir legacyFiles;
    QVERIFY(legacyFiles.isValid());
    const QString savedFile = legacyFiles.path() + QStringLiteral("/save.sav");
    QFile file(savedFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("preserve");
    file.close();

    const QString profilesDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles");
    QDir().mkpath(profilesDir);
    const QString path = profilesDir + QStringLiteral("/LegacyOverrideSettingsProfile.conf");
    {
        KConfig config(path, KConfig::SimpleConfig);
        KConfigGroup general = config.group(QStringLiteral("General"));
        general.writeEntry("instanceCount", 1);
        KConfigGroup inst = config.group(QStringLiteral("Instance0"));
        inst.writeEntry("username", QStringLiteral("player1"));
        inst.writeEntry("presetId", QStringLiteral("steam"));
        inst.writeEntry("privateTemporaryDirectories", true);
        inst.writeEntry("sharedDirectories", QStringList{QStringLiteral("/home/compositor/Games")});
        inst.writeEntry("overrideGamePath", legacyFiles.path());
        inst.writeEntry("overrideFiles", QStringList{QStringLiteral("save.sav")});
        inst.writeEntry("overridePatterns", QStringList{QStringLiteral("*.sav")});
        inst.writeEntry("overlayGamePath", legacyFiles.path());
        inst.writeEntry("overlayPatterns", QStringList{QStringLiteral("*.sav")});
        config.sync();
    }

    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("LegacyOverrideSettingsProfile")));
    const QVariantMap instance = m_sessionManager->getInstanceConfig(0);
    QVERIFY(instance.value(KEY("privateTemporaryDirectories")).toBool());
    QVERIFY(!instance.contains(KEY("dataDirectories")));
    QVERIFY(!instance.contains(KEY("dataDirectoriesSnapshotted")));
    QVERIFY(!instance.contains(KEY("overrideGamePath")));
    QVERIFY(!instance.contains(KEY("overrideFiles")));
    QVERIFY(!instance.contains(KEY("overridePatterns")));
    QVERIFY(!instance.contains(KEY("overlayGamePath")));
    QVERIFY(!instance.contains(KEY("overlayPatterns")));
    QVERIFY(QFile::exists(path + QStringLiteral(".pre-directory-snapshot.bak")));

    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("LegacyOverrideSettingsProfile")));
    KConfig saved(path, KConfig::SimpleConfig);
    const KConfigGroup savedInstance = saved.group(QStringLiteral("Instance0"));
    QVERIFY(!savedInstance.hasKey("overrideGamePath"));
    QVERIFY(!savedInstance.hasKey("overrideFiles"));
    QVERIFY(!savedInstance.hasKey("overridePatterns"));
    QVERIFY(!savedInstance.hasKey("overlayGamePath"));
    QVERIFY(!savedInstance.hasKey("overlayPatterns"));
    QVERIFY(QFile::exists(savedFile));

    m_sessionManager->deleteProfile(QStringLiteral("LegacyOverrideSettingsProfile"));
}

void TestSessionManager::testPlayerDataFolderEligibilityTransitions()
{
    m_sessionManager->setInstanceCount(1);
    QVERIFY(!m_sessionManager->hasPlayerDataStagingDirectories(-1));
    QVERIFY(!m_sessionManager->hasPlayerDataStagingDirectories(0));
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));

    PresetManager presetManager;
    const QString presetId = presetManager.addCustomPreset(QStringLiteral("Staging eligibility"),
                                                          QStringLiteral("/usr/bin/game"));
    QVERIFY(!presetId.isEmpty());
    m_sessionManager->setPresetManager(&presetManager);
    m_sessionManager->setInstancePreset(0, presetId);
    const QVariantMap overlay{{KEY("path"), QStringLiteral("/home/compositor/Games")},
                              {KEY("mode"), QStringLiteral("overlay")}};
    const QVariantMap acl{{KEY("path"), QStringLiteral("/home/compositor/ReadOnly")},
                          {KEY("mode"), QStringLiteral("acl")}};
    const QVariantMap bind{{KEY("path"), QStringLiteral("/home/compositor/Shared")},
                           {KEY("mode"), QStringLiteral("bind")}};
    const QVariantMap copy{{KEY("path"), QStringLiteral("/home/compositor/Games")},
                           {KEY("mode"), QStringLiteral("copy")}};

    QVERIFY(presetManager.setDataDirectories(presetId, {overlay}));
    QVERIFY(m_sessionManager->hasPlayerDataStagingDirectories(0));
    QVERIFY(presetManager.setDataDirectories(presetId, {acl, bind}));
    QVERIFY(!m_sessionManager->hasPlayerDataStagingDirectories(0));
    QVERIFY(presetManager.setDataDirectories(presetId, {copy}));
    QVERIFY(m_sessionManager->hasPlayerDataStagingDirectories(0));
}

void TestSessionManager::testApplicationDirectoryEditsAffectLoadedProfile()
{
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    PresetManager presetManager;
    const QString presetId = presetManager.addCustomPreset(QStringLiteral("Mutable directories"),
                                                          QStringLiteral("/usr/bin/game"));
    QVERIFY(!presetId.isEmpty());
    m_sessionManager->setPresetManager(&presetManager);
    m_sessionManager->setInstancePreset(0, presetId);
    const QVariantMap copy{{KEY("path"), QStringLiteral("/home/compositor/ExampleApp")},
                           {KEY("mode"), QStringLiteral("copy")}};
    QVERIFY(presetManager.setDataDirectories(presetId, {copy}));
    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("ApplicationDirectoryInheritance")));
    QVERIFY(m_sessionManager->hasPlayerDataStagingDirectories(0));

    QVERIFY(presetManager.setDataDirectories(presetId, {}));
    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("ApplicationDirectoryInheritance")));
    QVERIFY(!m_sessionManager->hasPlayerDataStagingDirectories(0));

    const QVariantMap overlay{{KEY("path"), QStringLiteral("/home/compositor/ExampleApp")},
                              {KEY("mode"), QStringLiteral("overlay")}};
    QVERIFY(presetManager.setDataDirectories(presetId, {overlay}));
    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("ApplicationDirectoryInheritance")));
    QVERIFY(m_sessionManager->hasPlayerDataStagingDirectories(0));
    QVERIFY(m_sessionManager->deleteProfile(QStringLiteral("ApplicationDirectoryInheritance")));
}

void TestSessionManager::testPlayerDataFolderPath()
{
    // Invalid index and missing user yield an empty path
    QCOMPARE(m_sessionManager->playerDataFolderPath(99), QString());
    m_sessionManager->setInstanceCount(1);
    QCOMPARE(m_sessionManager->playerDataFolderPath(0), QString());

    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    PresetManager presetManager;
    m_sessionManager->setPresetManager(&presetManager);
    const QString presetId = presetManager.addCustomPreset(QStringLiteral("custom-staging"),
                                                          QStringLiteral("/usr/bin/game"));
    QVERIFY(!presetId.isEmpty());
    m_sessionManager->setInstancePreset(0, presetId);

    const QVariantMap overlayDir{{QStringLiteral("path"), QStringLiteral("/home/compositor/Games/MyGame")},
                                 {QStringLiteral("mode"), QStringLiteral("overlay")}};
    const QVariantMap aclDir{{QStringLiteral("path"), QStringLiteral("/home/compositor/ReadOnly")},
                             {QStringLiteral("mode"), QStringLiteral("acl")}};
    QVERIFY(presetManager.setDataDirectories(presetId, {overlayDir, aclDir}));

    const QString expectedRoot = QDir::homePath()
        + QStringLiteral("/.local/share/couchplay/player-data/") + presetId + QStringLiteral("/player1");
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

    auto stagingDirCount = [this, &presetManager](const QString &presetId, const QVariantList &dirs, bool *eligible) -> int {
        m_sessionManager->setInstanceCount(1);
        m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
        m_sessionManager->setInstancePreset(0, presetId);
        if (!presetManager.setDataDirectories(presetId, dirs)) return -2;
        *eligible = m_sessionManager->hasPlayerDataStagingDirectories(0);
        const QString root = m_sessionManager->playerDataFolderPath(0);
        if (root.isEmpty()) {
            return -1;
        }
        const int count = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        QDir(root).removeRecursively();
        return count;
    };

    // Compute everything first so a failed assertion can't leak the patched env
    bool steamOverlayEligible = false;
    bool steamCopyEligible = false;
    bool customOverlayEligible = false;
    const int steamOverlayCount = stagingDirCount(QStringLiteral("steam"), QVariantList{markerDir}, &steamOverlayEligible);
    const int steamCopyCount = stagingDirCount(QStringLiteral("steam"), QVariantList{copyRootDir}, &steamCopyEligible);
    const QString customId =
        presetManager.addCustomPreset(QStringLiteral("Marker Test"), QStringLiteral("/usr/bin/game"));
    const int customOverlayCount = stagingDirCount(customId, QVariantList{markerDir}, &customOverlayEligible);

    QDir(QDir::homePath() + QStringLiteral("/.local/share/couchplay/player-data")).removeRecursively();
    QStandardPaths::setTestModeEnabled(false);
    if (!originalHome.isNull()) {
        qputenv("HOME", originalHome);
    } else {
        qunsetenv("HOME");
    }

    QCOMPARE(steamOverlayCount, 0); // marker: no staging folder
    QCOMPARE(steamCopyCount, 1); // copy mode at the same path: staged
    QCOMPARE(customOverlayCount, 1); // non-Steam preset: staged
    QVERIFY(!steamOverlayEligible);
    QVERIFY(steamCopyEligible);
    QVERIFY(customOverlayEligible);
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

void TestSessionManager::testProfileHooksAndGameSelectionRoundtrip()
{
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setPreSessionExecutable(QStringLiteral("/usr/local/bin/pre-session"));
    m_sessionManager->setPostSessionExecutable(QStringLiteral("/usr/local/bin/post-session"));
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstanceGame(0, QVariantMap{{QStringLiteral("launcherId"), QStringLiteral("heroic")},
                                                     {QStringLiteral("backend"), QStringLiteral("legendary")},
                                                     {QStringLiteral("gameId"), QStringLiteral("EpicGame")},
                                                     {QStringLiteral("title"), QStringLiteral("Test Game")}});
    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("HookRoundtrip")));

    m_sessionManager->newSession();
    QVERIFY(m_sessionManager->loadProfile(QStringLiteral("HookRoundtrip")));
    QCOMPARE(m_sessionManager->preSessionExecutable(), QStringLiteral("/usr/local/bin/pre-session"));
    QCOMPARE(m_sessionManager->postSessionExecutable(), QStringLiteral("/usr/local/bin/post-session"));
    const QVariantMap selection = m_sessionManager->getInstanceConfig(0).value(QStringLiteral("gameSelection")).toMap();
    QCOMPARE(selection.value(QStringLiteral("launcherId")).toString(), QStringLiteral("heroic"));
    QCOMPARE(selection.value(QStringLiteral("backend")).toString(), QStringLiteral("legendary"));
    QCOMPARE(selection.value(QStringLiteral("gameId")).toString(), QStringLiteral("EpicGame"));
    m_sessionManager->deleteProfile(QStringLiteral("HookRoundtrip"));
}

void TestSessionManager::testProfileHookPathsPreserveWhitespace()
{
    const QString prePath = QStringLiteral(" /tmp/pre-session ");
    const QString postPath = QStringLiteral(" /tmp/post-session ");
    m_sessionManager->setPreSessionExecutable(prePath);
    m_sessionManager->setPostSessionExecutable(postPath);
    QCOMPARE(m_sessionManager->preSessionExecutable(), prePath);
    QCOMPARE(m_sessionManager->postSessionExecutable(), postPath);
}

void TestSessionManager::testDuplicateProfilePreservesData()
{
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setPreSessionExecutable(QStringLiteral("/usr/local/bin/pre-session"));
    m_sessionManager->setInstanceGame(0, QVariantMap{{QStringLiteral("launcherId"), QStringLiteral("steam")},
                                                     {QStringLiteral("backend"), QStringLiteral("native")},
                                                     {QStringLiteral("gameId"), QStringLiteral("123")},
                                                     {QStringLiteral("title"), QStringLiteral("Game")}});
    QVERIFY(m_sessionManager->saveProfile(QStringLiteral("DuplicateProfile")));

    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles/DuplicateProfile.conf");
    KConfig source(path, KConfig::SimpleConfig);
    source.group(QStringLiteral("Future")).writeEntry(QStringLiteral("value"), QStringLiteral("preserved"));
    source.sync();

    const QString copyName = m_sessionManager->duplicateProfile(QStringLiteral("DuplicateProfile"));
    QCOMPARE(copyName, QStringLiteral("DuplicateProfile Copy"));
    QCOMPARE(m_sessionManager->currentProfileName(), QStringLiteral("DuplicateProfile"));

    KConfig copy(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                     + QStringLiteral("/profiles/DuplicateProfile Copy.conf"),
                 KConfig::SimpleConfig);
    QCOMPARE(copy.group(QStringLiteral("General")).readEntry("name", QString()), copyName);
    QCOMPARE(copy.group(QStringLiteral("Future")).readEntry("value", QString()), QStringLiteral("preserved"));

    QVERIFY(m_sessionManager->duplicateProfile(QStringLiteral("DuplicateProfile")) != QString());
    m_sessionManager->deleteProfile(QStringLiteral("DuplicateProfile"));
    m_sessionManager->deleteProfile(QStringLiteral("DuplicateProfile Copy"));
    m_sessionManager->deleteProfile(QStringLiteral("DuplicateProfile Copy 2"));
}

void TestSessionManager::testProfileNameValidation()
{
    QVERIFY(SessionManager::isValidProfileName(QStringLiteral("Family Night")));
    QVERIFY(!SessionManager::isValidProfileName(QStringLiteral("")));
    QVERIFY(!SessionManager::isValidProfileName(QStringLiteral("../escape")));
    QVERIFY(!SessionManager::isValidProfileName(QStringLiteral("bad/name")));
    QVERIFY(!SessionManager::isValidProfileName(QStringLiteral(" trailing")));
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
