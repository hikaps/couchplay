// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <KConfigGroup>
#include <KSharedConfig>

#include "PresetManager.h"

class TestPresetManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void testBuiltinPresetsExist();
    void testGetCommand();
    void testStaleFlatpakCacheIgnored();
    void testGetWorkingDirectory();
    void testGetLauncherId();

    void testAddCustomPreset();
    void testRemoveCustomPreset();

    void testGetSetSharedDirectories();
    void testSetDataDirectoriesQmlShape();
    void testLauncherInfoFlags();

    void testLauncherIdPersistence();
    void testKConfigMigration();
    void testKConfigMigrationLegacySharedDirectories();

    void testDetectLauncherId_NativeSteam();
    void testDetectLauncherId_NativeHeroic();
    void testDetectLauncherId_NativeLutris();
    void testDetectLauncherId_FlatpakSteam();
    void testDetectLauncherId_FlatpakHeroic();
    void testDetectLauncherId_FlatpakLutris();
    void testDetectLauncherId_UnknownBinary();
    void testDetectLauncherId_UrlScheme();

    void testPopulateLauncherInfo_SteamCustom();
    void testPopulateLauncherInfo_HeroicCustom();
    void testPopulateLauncherInfo_EmptyLauncherId();
    void testPopulateLauncherInfo_FreshOnAccess();

private:
    QTemporaryDir *m_tempDir = nullptr;
};

void TestPresetManager::initTestCase()
{
}

void TestPresetManager::cleanupTestCase()
{
}

void TestPresetManager::init()
{
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);

    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);

    QString presetsPath = configDir + QStringLiteral("/presets.json");
    QFile::remove(presetsPath);
    QFile::remove(presetsPath + QStringLiteral(".bak"));

    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    static const QString prefix = QStringLiteral("Preset: ");
    for (const QString &groupName : config->groupList()) {
        if (groupName.startsWith(prefix)) {
            config->deleteGroup(groupName);
        }
    }
    config->sync();
}

void TestPresetManager::cleanup()
{
    delete m_tempDir;
    m_tempDir = nullptr;
    QStandardPaths::setTestModeEnabled(false);
}

void TestPresetManager::testBuiltinPresetsExist()
{
    PresetManager manager;

    QVariantList presets = manager.presetsAsVariant();
    QVERIFY(presets.size() >= 3); // Steam, Heroic, Lutris

    bool foundSteam = false;
    bool foundHeroic = false;
    bool foundLutris = false;

    for (const QVariant &presetVar : presets) {
        QVariantMap preset = presetVar.toMap();
        QString id = preset[QStringLiteral("id")].toString();

        if (id == QStringLiteral("steam")) {
            foundSteam = true;
            QCOMPARE(preset[QStringLiteral("name")].toString(), QStringLiteral("Steam Big Picture"));
            QCOMPARE(preset[QStringLiteral("isBuiltin")].toBool(), true);
        } else if (id == QStringLiteral("heroic")) {
            foundHeroic = true;
            QCOMPARE(preset[QStringLiteral("name")].toString(), QStringLiteral("Heroic Games"));
            QCOMPARE(preset[QStringLiteral("isBuiltin")].toBool(), true);
        } else if (id == QStringLiteral("lutris")) {
            foundLutris = true;
            QCOMPARE(preset[QStringLiteral("name")].toString(), QStringLiteral("Lutris"));
            QCOMPARE(preset[QStringLiteral("isBuiltin")].toBool(), true);
        }
    }

    QVERIFY(foundSteam);
    QVERIFY(foundHeroic);
    QVERIFY(foundLutris);
}

void TestPresetManager::testGetCommand()
{
    PresetManager manager;

    QString steamCommand = manager.getCommand(QStringLiteral("steam"));
    QCOMPARE(steamCommand, QStringLiteral("steam -bigpicture"));

    QString heroicCommand = manager.getCommand(QStringLiteral("heroic"));
    QVERIFY(!heroicCommand.isEmpty());

    QString lutrisCommand = manager.getCommand(QStringLiteral("lutris"));
    QCOMPARE(lutrisCommand, QStringLiteral("lutris"));
}

void TestPresetManager::testStaleFlatpakCacheIgnored()
{
    // Pre-seed a stale v1 cache; a fresh timestamp keeps it inside the TTL so
    // only the version gate can prevent it from overriding the builtin.
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup cacheGroup = config->group(QStringLiteral("FlatpakCache"));
    cacheGroup.writeEntry(QStringLiteral("version"), 1);
    cacheGroup.writeEntry(QStringLiteral("steam/command"), QStringLiteral("steam -tenfoot -steamdeck"));
    cacheGroup.writeEntry(QStringLiteral("steam/timestamp"),
                          QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddTHHmmss")));
    cacheGroup.sync();

    PresetManager manager;
    QCOMPARE(manager.getCommand(QStringLiteral("steam")), QStringLiteral("steam -bigpicture"));

    // Constructor rewrote the cache at the current version.
    cacheGroup = config->group(QStringLiteral("FlatpakCache"));
    QCOMPARE(cacheGroup.readEntry(QStringLiteral("version"), 0), 2);
    QCOMPARE(cacheGroup.readEntry(QStringLiteral("steam/command")), QStringLiteral("steam -bigpicture"));
}

void TestPresetManager::testGetWorkingDirectory()
{
    PresetManager manager;

    QString steamDir = manager.getWorkingDirectory(QStringLiteral("steam"));
    QVERIFY(steamDir.isEmpty());

    QString heroicDir = manager.getWorkingDirectory(QStringLiteral("heroic"));
    QVERIFY(heroicDir.isEmpty());

    QString lutrisDir = manager.getWorkingDirectory(QStringLiteral("lutris"));
    QVERIFY(lutrisDir.isEmpty());
}

void TestPresetManager::testGetLauncherId()
{
    PresetManager manager;

    QCOMPARE(manager.getLauncherId(QStringLiteral("steam")), QStringLiteral("steam"));
    QCOMPARE(manager.getLauncherId(QStringLiteral("heroic")), QStringLiteral("heroic"));
    QCOMPARE(manager.getLauncherId(QStringLiteral("lutris")), QStringLiteral("lutris"));
}

void TestPresetManager::testAddCustomPreset()
{
    PresetManager manager;
    QSignalSpy presetsChangedSpy(&manager, &PresetManager::presetsChanged);

    QString id = manager.addCustomPreset(QStringLiteral("Test Game"),
                                         QStringLiteral("/path/to/game"),
                                         QStringLiteral("/working/dir"),
                                         QStringLiteral("test-icon"));

    QVERIFY(!id.isEmpty());
    QVERIFY(id.startsWith(QStringLiteral("custom-")));
    QCOMPARE(presetsChangedSpy.count(), 1);

    QVariantList presets = manager.presetsAsVariant();
    bool found = false;
    for (const QVariant &presetVar : presets) {
        QVariantMap preset = presetVar.toMap();
        if (preset[QStringLiteral("id")] == id) {
            found = true;
            QCOMPARE(preset[QStringLiteral("name")].toString(), QStringLiteral("Test Game"));
            QCOMPARE(preset[QStringLiteral("command")].toString(), QStringLiteral("/path/to/game"));
            QCOMPARE(preset[QStringLiteral("workingDirectory")].toString(), QStringLiteral("/working/dir"));
            QCOMPARE(preset[QStringLiteral("iconName")].toString(), QStringLiteral("test-icon"));
            QCOMPARE(preset[QStringLiteral("isBuiltin")].toBool(), false);
        }
    }
    QVERIFY(found);
}

void TestPresetManager::testRemoveCustomPreset()
{
    PresetManager manager;
    QSignalSpy presetsChangedSpy(&manager, &PresetManager::presetsChanged);

    // Add a custom preset
    QString id = manager.addCustomPreset(QStringLiteral("To Remove"), QStringLiteral("/path/to/game"));
    QVERIFY(!id.isEmpty());
    presetsChangedSpy.clear();

    // Remove it
    bool result = manager.removeCustomPreset(id);
    QVERIFY(result);
    QCOMPARE(presetsChangedSpy.count(), 1);

    // Verify it's gone
    QVariantList presets = manager.presetsAsVariant();
    for (const QVariant &presetVar : presets) {
        QVariantMap preset = presetVar.toMap();
        QVERIFY(preset[QStringLiteral("id")] != id);
    }
}

void TestPresetManager::testGetSetSharedDirectories()
{
    PresetManager manager;
    QSignalSpy presetsChangedSpy(&manager, &PresetManager::presetsChanged);

    QString id = manager.addCustomPreset(QStringLiteral("Shared Test"), QStringLiteral("/path/to/game"));

    QVariantList dirs = manager.getDataDirectories(id);
    QVERIFY(dirs.isEmpty());
    presetsChangedSpy.clear();

    QVariantList newDirs;
    DataDirectory dir1{QStringLiteral("/shared/dir1"), QStringLiteral("acl")};
    DataDirectory dir2{QStringLiteral("/shared/dir2"), QStringLiteral("overlay")};
    newDirs.append(QVariant::fromValue(dir1));
    newDirs.append(QVariant::fromValue(dir2));
    bool result = manager.setDataDirectories(id, newDirs);
    QVERIFY(result);
    QCOMPARE(presetsChangedSpy.count(), 1);

    dirs = manager.getDataDirectories(id);
    QCOMPARE(dirs.size(), 2);
}

void TestPresetManager::testSetDataDirectoriesQmlShape()
{
    PresetManager manager;

    QString id = manager.addCustomPreset(QStringLiteral("QML Shape Test"), QStringLiteral("/path/to/game"));

    // QML sends JS objects, which arrive as QVariantMap — not gadget variants
    QVariantMap map1;
    map1[QStringLiteral("path")] = QStringLiteral("/qml/dir1");
    map1[QStringLiteral("mode")] = QStringLiteral("copy");
    QVariantMap map2;
    map2[QStringLiteral("path")] = QStringLiteral("/qml/dir2");
    map2[QStringLiteral("mode")] = QStringLiteral("overlay");
    QVariantMap map3; // unknown mode must sanitize to "acl"
    map3[QStringLiteral("path")] = QStringLiteral("/qml/dir3");
    map3[QStringLiteral("mode")] = QStringLiteral("bogus");
    QVariantMap map4; // empty mode must default to "acl"
    map4[QStringLiteral("path")] = QStringLiteral("/qml/dir4");

    QVariantList qmlDirs;
    qmlDirs.append(map1);
    qmlDirs.append(map2);
    qmlDirs.append(map3);
    qmlDirs.append(map4);

    QVERIFY(manager.setDataDirectories(id, qmlDirs));

    QVariantList result = manager.getDataDirectories(id);
    QCOMPARE(result.size(), 4);

    QCOMPARE(result[0].value<DataDirectory>().path, QStringLiteral("/qml/dir1"));
    QCOMPARE(result[0].value<DataDirectory>().mode, QStringLiteral("copy"));
    QCOMPARE(result[1].value<DataDirectory>().path, QStringLiteral("/qml/dir2"));
    QCOMPARE(result[1].value<DataDirectory>().mode, QStringLiteral("overlay"));
    QCOMPARE(result[2].value<DataDirectory>().path, QStringLiteral("/qml/dir3"));
    QCOMPARE(result[2].value<DataDirectory>().mode, QStringLiteral("acl"));
    QCOMPARE(result[3].value<DataDirectory>().path, QStringLiteral("/qml/dir4"));
    QCOMPARE(result[3].value<DataDirectory>().mode, QStringLiteral("acl"));

    // Mixed shapes in one call must also work (map + gadget)
    QVariantList mixed;
    mixed.append(map1);
    mixed.append(QVariant::fromValue(DataDirectory{QStringLiteral("/cpp/dir"), QStringLiteral("acl")}));
    QVERIFY(manager.setDataDirectories(id, mixed));
    QCOMPARE(manager.getDataDirectories(id).size(), 2);
}

void TestPresetManager::testLauncherInfoFlags()
{
    PresetManager manager;

    LaunchPreset steam = manager.getPreset(QStringLiteral("steam"));
    QVERIFY(!steam.launcherInfo.configPath.isEmpty() || steam.launcherId == QStringLiteral("steam"));

    LaunchPreset heroic = manager.getPreset(QStringLiteral("heroic"));
    QCOMPARE(heroic.launcherId, QStringLiteral("heroic"));

    LaunchPreset lutris = manager.getPreset(QStringLiteral("lutris"));
    QCOMPARE(lutris.launcherId, QStringLiteral("lutris"));
}

void TestPresetManager::testLauncherIdPersistence()
{
    {
        PresetManager manager;
        QString id = manager.addCustomPreset(QStringLiteral("Heroic Preset"),
                                             QStringLiteral("heroic"),
                                             QString(),
                                             QStringLiteral("heroic-icon"));

        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        KConfigGroup group = config->group(QStringLiteral("Preset: ") + id);
        group.writeEntry(QStringLiteral("launcherId"), QStringLiteral("heroic"));
        config->sync();
    }

    {
        PresetManager manager;
        LaunchPreset found;
        for (const LaunchPreset &p : manager.presets()) {
            if (p.name == QStringLiteral("Heroic Preset")) {
                found = p;
                break;
            }
        }
        QCOMPARE(found.launcherId, QStringLiteral("heroic"));
    }
}

void TestPresetManager::testKConfigMigration()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Preset: custom-migrate-test"));
    group.writeEntry(QStringLiteral("id"), QStringLiteral("custom-migrate-test"));
    group.writeEntry(QStringLiteral("name"), QStringLiteral("Migrate Test"));
    group.writeEntry(QStringLiteral("command"), QStringLiteral("steam"));
    group.writeEntry(QStringLiteral("steamIntegration"), true);
    config->sync();

    PresetManager manager;
    LaunchPreset preset = manager.getPreset(QStringLiteral("custom-migrate-test"));
    QCOMPARE(preset.launcherId, QStringLiteral("steam"));
}

void TestPresetManager::testKConfigMigrationLegacySharedDirectories()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Preset: custom-legacy-dirs"));
    group.writeEntry(QStringLiteral("id"), QStringLiteral("custom-legacy-dirs"));
    group.writeEntry(QStringLiteral("name"), QStringLiteral("Legacy Dirs Test"));
    group.writeEntry(QStringLiteral("command"), QStringLiteral("/usr/bin/game"));
    // Legacy format: sharedDirectories as a plain QStringList, no dataDirectories key
    group.writeEntry(QStringLiteral("sharedDirectories"),
                     QStringList{QStringLiteral("/home/compositor/Games"),
                                 QStringLiteral("/home/compositor/Saves")});
    config->sync();

    PresetManager manager;
    QVariantList dirs = manager.getDataDirectories(QStringLiteral("custom-legacy-dirs"));
    QCOMPARE(dirs.size(), 2);
    QCOMPARE(dirs[0].value<DataDirectory>().path, QStringLiteral("/home/compositor/Games"));
    QCOMPARE(dirs[0].value<DataDirectory>().mode, QStringLiteral("acl"));
    QCOMPARE(dirs[1].value<DataDirectory>().path, QStringLiteral("/home/compositor/Saves"));
    QCOMPARE(dirs[1].value<DataDirectory>().mode, QStringLiteral("acl"));
}

void TestPresetManager::testDetectLauncherId_NativeSteam()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("steam -tenfoot")), QStringLiteral("steam"));
}

void TestPresetManager::testDetectLauncherId_NativeHeroic()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("heroic")), QStringLiteral("heroic"));
}

void TestPresetManager::testDetectLauncherId_NativeLutris()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("lutris")), QStringLiteral("lutris"));
}

void TestPresetManager::testDetectLauncherId_FlatpakSteam()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("flatpak run com.valvesoftware.Steam")), QStringLiteral("steam"));
}

void TestPresetManager::testDetectLauncherId_FlatpakHeroic()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("flatpak run com.heroicgameslauncher.hgl")), QStringLiteral("heroic"));
}

void TestPresetManager::testDetectLauncherId_FlatpakLutris()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("flatpak run net.lutris.Lutris")), QStringLiteral("lutris"));
}

void TestPresetManager::testDetectLauncherId_UnknownBinary()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("/usr/bin/my-game --flag")), QString());
}

void TestPresetManager::testDetectLauncherId_UrlScheme()
{
    PresetManager manager;
    QCOMPARE(manager.detectLauncherId(QStringLiteral("heroic://launch/gog/abc")), QString());
}

void TestPresetManager::testPopulateLauncherInfo_SteamCustom()
{
    PresetManager manager;

    QString id = manager.addCustomPreset(QStringLiteral("My Steam"),
                                          QStringLiteral("steam -tenfoot"));

    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Preset: ") + id);
    group.writeEntry(QStringLiteral("launcherId"), QStringLiteral("steam"));
    config->sync();

    PresetManager manager2;
    LaunchPreset preset = manager2.getPreset(id);

    QVERIFY(!preset.isBuiltin);
    QCOMPARE(preset.launcherId, QStringLiteral("steam"));
}

void TestPresetManager::testPopulateLauncherInfo_HeroicCustom()
{
    PresetManager manager;

    QString id = manager.addCustomPreset(QStringLiteral("My Heroic"),
                                          QStringLiteral("heroic"));

    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Preset: ") + id);
    group.writeEntry(QStringLiteral("launcherId"), QStringLiteral("heroic"));
    config->sync();

    PresetManager manager2;
    LaunchPreset preset = manager2.getPreset(id);

    QVERIFY(!preset.isBuiltin);
    QCOMPARE(preset.launcherId, QStringLiteral("heroic"));
}

void TestPresetManager::testPopulateLauncherInfo_EmptyLauncherId()
{
    PresetManager manager;

    QString id = manager.addCustomPreset(QStringLiteral("Generic Game"),
                                          QStringLiteral("/usr/bin/my-game"));

    LaunchPreset preset = manager.getPreset(id);

    QVERIFY(!preset.isBuiltin);
    QVERIFY(preset.launcherId.isEmpty());
    QVERIFY(preset.launcherInfo.configPath.isEmpty());
    QVERIFY(preset.launcherInfo.dataPath.isEmpty());
}

void TestPresetManager::testPopulateLauncherInfo_FreshOnAccess()
{
    PresetManager manager;

    QString id = manager.addCustomPreset(QStringLiteral("Fresh Test"),
                                          QStringLiteral("steam"));

    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Preset: ") + id);
    group.writeEntry(QStringLiteral("launcherId"), QStringLiteral("steam"));
    config->sync();

    PresetManager manager2;

    LaunchPreset first = manager2.getPreset(id);
    LaunchPreset second = manager2.getPreset(id);

    QCOMPARE(first.launcherInfo.configPath, second.launcherInfo.configPath);
    QCOMPARE(first.launcherInfo.dataPath, second.launcherInfo.dataPath);
}

QTEST_MAIN(TestPresetManager)
#include "test_presetmanager.moc"
