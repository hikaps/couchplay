// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QtTest>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include "core/PresetManager.h"
#include "core/HeroicConfigManager.h"

class TestPresetManagerIntegration : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void testHeroicDetected();
    void testHeroicNotDetected();

private:
    void createMockHeroicConfig(const QString &basePath, bool isFlatpak);
    void createMockGameConfigs(const QString &basePath);

    QByteArray m_originalHome;
};

void TestPresetManagerIntegration::initTestCase()
{
    m_originalHome = qgetenv("HOME");
}

void TestPresetManagerIntegration::cleanupTestCase()
{
    if (!m_originalHome.isEmpty()) {
        qputenv("HOME", m_originalHome);
    }
}

void TestPresetManagerIntegration::createMockHeroicConfig(const QString &basePath, bool isFlatpak)
{
    QString heroicRoot;
    if (isFlatpak) {
        heroicRoot = basePath + QStringLiteral("/.var/app/com.heroicgameslauncher.hgl/config/heroic");
    } else {
        heroicRoot = basePath + QStringLiteral("/.config/heroic");
    }

    QDir().mkpath(heroicRoot);
    QDir().mkpath(heroicRoot + QStringLiteral("/GamesConfig"));

    // Create config.json
    QFile configFile(heroicRoot + QStringLiteral("/config.json"));
    if (configFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;
        QJsonObject defaultSettings;
        defaultSettings[QStringLiteral("defaultInstallPath")] = QString(basePath + QStringLiteral("/Games/Heroic"));
        defaultSettings[QStringLiteral("defaultWinePrefix")] = QString(basePath + QStringLiteral("/Games/Heroic/Prefixes/default"));
        root[QStringLiteral("defaultSettings")] = defaultSettings;
        configFile.write(QJsonDocument(root).toJson());
        configFile.close();
    }

    // Create dummy directories referenced in config
    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic"));
    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic/Prefixes/default"));
}

void TestPresetManagerIntegration::createMockGameConfigs(const QString &basePath)
{
    QString configRoot = basePath + QStringLiteral("/.config/heroic"); // Assuming native for this helper

    // 1. Legendary (Epic)
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

    // 2. GOG
    QDir().mkpath(configRoot + QStringLiteral("/gog_store"));
    QFile gogFile(configRoot + QStringLiteral("/gog_store/installed.json"));
    if (gogFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;
        QJsonArray installed;
        QJsonObject game;
        game[QStringLiteral("appName")] = QStringLiteral("1234567890");
        game[QStringLiteral("title")] = QStringLiteral("Test Game GOG");
        game[QStringLiteral("install_path")] = QString(basePath + QStringLiteral("/Games/Heroic/GogGame"));
        game[QStringLiteral("executable")] = QStringLiteral("game.exe");
        game[QStringLiteral("install_size")] = 2048;
        installed.append(game);
        root[QStringLiteral("installed")] = installed;
        gogFile.write(QJsonDocument(root).toJson());
        gogFile.close();
    }
    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic/GogGame"));

    // 3. Nile (Amazon)
    QDir().mkpath(configRoot + QStringLiteral("/nile_config"));
    QFile nileFile(configRoot + QStringLiteral("/nile_config/installed.json"));
    if (nileFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;
        QJsonArray installed;
        QJsonObject game;
        game[QStringLiteral("id")] = QStringLiteral("AmazonGameApp");
        game[QStringLiteral("title")] = QStringLiteral("Test Game Amazon");
        game[QStringLiteral("install_path")] = QString(basePath + QStringLiteral("/Games/Heroic/AmazonGame"));
        game[QStringLiteral("executable")] = QStringLiteral("bin/game.exe");
        game[QStringLiteral("install_size")] = 4096;
        installed.append(game);
        root[QStringLiteral("installed")] = installed;
        nileFile.write(QJsonDocument(root).toJson());
        nileFile.close();
    }
    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic/AmazonGame"));
}

void TestPresetManagerIntegration::testHeroicDetected()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    // Create mock Heroic installation
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());

    // Create managers
    HeroicConfigManager heroicManager;
    PresetManager presetManager;

    // Verify Heroic is detected
    heroicManager.detectHeroicPaths();
    QVERIFY(heroicManager.isHeroicDetected());
    QVERIFY(!heroicManager.isFlatpak());
    QCOMPARE(heroicManager.heroicCommand(), QStringLiteral("heroic"));

    // Inject HeroicConfigManager into PresetManager
    QSignalSpy presetsChangedSpy(&presetManager, &PresetManager::presetsChanged);
    presetManager.setHeroicConfigManager(&heroicManager);
    QCOMPARE(presetsChangedSpy.count(), 1);

    // Get the Heroic preset
    LaunchPreset heroicPreset = presetManager.getPreset(QStringLiteral("heroic"));
    QCOMPARE(heroicPreset.id, QStringLiteral("heroic"));
    QCOMPARE(heroicPreset.name, QStringLiteral("Heroic Games"));
    QCOMPARE(heroicPreset.launcherId, QStringLiteral("heroic"));
    QCOMPARE(heroicPreset.isBuiltin, true);

    // Verify launcherInfo is populated
    QVERIFY(!heroicPreset.launcherInfo.configPath.isEmpty());
    QCOMPARE(heroicPreset.launcherInfo.configPath,
             homeDir.path() + QStringLiteral("/.config/heroic"));

    QVERIFY(!heroicPreset.launcherInfo.dataPath.isEmpty());
    QCOMPARE(heroicPreset.launcherInfo.dataPath,
             homeDir.path() + QStringLiteral("/Games/Heroic"));

    QCOMPARE(heroicPreset.launcherInfo.requiresAcls, true);
    QCOMPARE(heroicPreset.launcherInfo.hasShortcutSync, false);

    // Verify gameDirectories are extracted
    QCOMPARE(heroicPreset.launcherInfo.gameDirectories.size(), 3);
    QVERIFY(heroicPreset.launcherInfo.gameDirectories.contains(
        homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame")));
    QVERIFY(heroicPreset.launcherInfo.gameDirectories.contains(
        homeDir.path() + QStringLiteral("/Games/Heroic/GogGame")));
    QVERIFY(heroicPreset.launcherInfo.gameDirectories.contains(
        homeDir.path() + QStringLiteral("/Games/Heroic/AmazonGame")));

    // Verify command is set from HeroicConfigManager
    QCOMPARE(heroicPreset.command, QStringLiteral("heroic"));

    // Verify shared directories for Heroic exclude game directories
    QStringList sharedDirs = presetManager.getSharedDirectories(QStringLiteral("heroic"));
    QCOMPARE(sharedDirs.size(), 2); // configPath + installPath

    QVERIFY(sharedDirs.contains(homeDir.path() + QStringLiteral("/.config/heroic")));
    QVERIFY(sharedDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic")));
    QVERIFY(!sharedDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame")));
    QVERIFY(!sharedDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/GogGame")));
    QVERIFY(!sharedDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/AmazonGame")));
}

void TestPresetManagerIntegration::testHeroicNotDetected()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    // Do NOT create any Heroic config files

    // Create managers
    HeroicConfigManager heroicManager;
    PresetManager presetManager;

    // Verify Heroic is NOT detected
    heroicManager.detectHeroicPaths();
    QVERIFY(!heroicManager.isHeroicDetected());

    // Inject HeroicConfigManager into PresetManager
    QSignalSpy presetsChangedSpy(&presetManager, &PresetManager::presetsChanged);
    presetManager.setHeroicConfigManager(&heroicManager);
    QCOMPARE(presetsChangedSpy.count(), 1);

    // Get the Heroic preset
    LaunchPreset heroicPreset = presetManager.getPreset(QStringLiteral("heroic"));
    QCOMPARE(heroicPreset.id, QStringLiteral("heroic"));
    QCOMPARE(heroicPreset.name, QStringLiteral("Heroic Games"));
    QCOMPARE(heroicPreset.launcherId, QStringLiteral("heroic"));
    QCOMPARE(heroicPreset.isBuiltin, true);

    // Verify launcherInfo is NOT populated (empty)
    QVERIFY(heroicPreset.launcherInfo.configPath.isEmpty());
    QVERIFY(heroicPreset.launcherInfo.dataPath.isEmpty());
    QVERIFY(heroicPreset.launcherInfo.gameDirectories.isEmpty());
    QCOMPARE(heroicPreset.launcherInfo.requiresAcls, false);

    // Verify default command is set
    QCOMPARE(heroicPreset.command, QStringLiteral("heroic"));

    // Verify getDefaultSharedDirectories for Heroic returns empty
    QStringList sharedDirs = presetManager.getSharedDirectories(QStringLiteral("heroic"));
    QVERIFY(sharedDirs.isEmpty());
}

QTEST_MAIN(TestPresetManagerIntegration)
#include "test_presetmanager_integration.moc"
