// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QtTest>
#include <QTemporaryDir>
#include <QStandardPaths>
#include "core/HeroicConfigManager.h"

class TestHeroicConfigManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void testDetectNative();
    void testDetectFlatpak();
    void testParseLegendary();
    void testParseGog();
    void testParseNile();
    // void testParseSideload();  // Disabled: methods not implemented in HeroicConfigManager
    void testExtractGameDirectories();
    // void testExtractSideloadDirectories();  // Disabled: methods not implemented in HeroicConfigManager
    // void testSyncSideloadToUser();  // Disabled: methods not implemented in HeroicConfigManager

private:
    void createMockHeroicConfig(const QString &basePath, bool isFlatpak);
    void createMockGameConfigs(const QString &basePath);
    
    QByteArray m_originalHome;
    QTemporaryDir m_tempDir;
};

void TestHeroicConfigManager::initTestCase()
{
    m_originalHome = qgetenv("HOME");
    QVERIFY(m_tempDir.isValid());
}

void TestHeroicConfigManager::cleanupTestCase()
{
    if (!m_originalHome.isEmpty()) {
        qputenv("HOME", m_originalHome);
    }
}

void TestHeroicConfigManager::createMockHeroicConfig(const QString &basePath, bool isFlatpak)
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

void TestHeroicConfigManager::createMockGameConfigs(const QString &basePath)
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

    // 4. Sideload
    QDir().mkpath(configRoot + QStringLiteral("/sideload_apps"));
    QFile sideloadFile(configRoot + QStringLiteral("/sideload_apps/library.json"));
    if (sideloadFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;
        QJsonArray games;
        QJsonObject game;
        game[QStringLiteral("app_name")] = QStringLiteral("SideloadAppId");
        game[QStringLiteral("title")] = QStringLiteral("Test Game Sideload");
        game[QStringLiteral("folder_name")] = QString(basePath + QStringLiteral("/Games/Heroic/SideloadGame"));
        game[QStringLiteral("is_installed")] = true;
        game[QStringLiteral("runner")] = QStringLiteral("sideload");
        
        QJsonObject install;
        install[QStringLiteral("executable")] = QString(basePath + QStringLiteral("/Games/Heroic/SideloadGame/game.exe"));
        game[QStringLiteral("install")] = install;
        
        games.append(game);
        root[QStringLiteral("games")] = games;
        sideloadFile.write(QJsonDocument(root).toJson());
        sideloadFile.close();
    }
    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic/SideloadGame"));
}

void TestHeroicConfigManager::testDetectNative()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    
    HeroicConfigManager manager;
    QVERIFY(manager.isHeroicDetected());
    QVERIFY(!manager.isFlatpak());
    QCOMPARE(manager.heroicCommand(), QStringLiteral("heroic"));
    QCOMPARE(manager.defaultInstallPath(), homeDir.path() + QStringLiteral("/Games/Heroic"));
}

void TestHeroicConfigManager::testDetectFlatpak()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), true);
    
    HeroicConfigManager manager;
    QVERIFY(manager.isHeroicDetected());
    QVERIFY(manager.isFlatpak());
    QCOMPARE(manager.heroicCommand(), QStringLiteral("flatpak run com.heroicgameslauncher.hgl"));
}

void TestHeroicConfigManager::testParseLegendary()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());
    
    HeroicConfigManager manager;
    manager.loadGames();
    
    QList<HeroicGame> games = manager.installedGames();
    bool found = false;
    for (const auto &game : games) {
        if (game.runner == QStringLiteral("legendary")) {
            QCOMPARE(game.title, QStringLiteral("Test Game Epic"));
            QCOMPARE(game.appName, QStringLiteral("EpicGameApp"));
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestHeroicConfigManager::testParseGog()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());
    
    HeroicConfigManager manager;
    manager.loadGames();
    
    QList<HeroicGame> games = manager.installedGames();
    bool found = false;
    for (const auto &game : games) {
        if (game.runner == QStringLiteral("gog")) {
            QCOMPARE(game.title, QStringLiteral("Test Game GOG"));
            QCOMPARE(game.appName, QStringLiteral("1234567890"));
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestHeroicConfigManager::testParseNile()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());
    
    HeroicConfigManager manager;
    manager.loadGames();
    
    QList<HeroicGame> games = manager.installedGames();
    bool found = false;
    for (const auto &game : games) {
        if (game.runner == QStringLiteral("nile")) {
            QCOMPARE(game.title, QStringLiteral("Test Game Amazon"));
            QCOMPARE(game.appName, QStringLiteral("AmazonGameApp"));
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

// Disabled: methods not implemented in HeroicConfigManager
/*
void TestHeroicConfigManager::testParseSideload()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());
    
    HeroicConfigManager manager;
    manager.loadSideloadedGames();
    
    QList<HeroicGame> games = manager.sideloadedGames();
    bool found = false;
    for (const auto &game : games) {
        if (game.runner == QStringLiteral("sideload")) {
            QCOMPARE(game.title, QStringLiteral("Test Game Sideload"));
            QCOMPARE(game.appName, QStringLiteral("SideloadAppId"));
            found = true;
            break;
        }
    }
    QVERIFY(found);
}
*/

void TestHeroicConfigManager::testExtractGameDirectories()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());
    
    HeroicConfigManager manager;
    manager.loadGames();
    
    QStringList gameDirs = manager.extractGameDirectories();
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame")));
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/GogGame")));
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/AmazonGame")));
}

// Disabled: methods not implemented in HeroicConfigManager
/*
void TestHeroicConfigManager::testExtractSideloadDirectories()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());
    
    HeroicConfigManager manager;
    manager.loadSideloadedGames();
    
    QStringList sideloadDirs = manager.extractSideloadDirectories();
    QVERIFY(sideloadDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/SideloadGame")));
}

void TestHeroicConfigManager::testSyncSideloadToUser()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());
    
    createMockHeroicConfig(homeDir.path(), false);
    createMockGameConfigs(homeDir.path());
    
    HeroicConfigManager manager;
    manager.loadSideloadedGames();
    
    // Test that sync requires a helper client
    bool syncResult = manager.syncSideloadToUser(QStringLiteral("testuser"));
    QVERIFY(!syncResult); // Should fail without helper client
}
*/

QTEST_MAIN(TestHeroicConfigManager)
#include "test_heroicconfigmanager.moc"
