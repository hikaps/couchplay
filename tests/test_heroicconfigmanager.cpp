// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "core/HeroicConfigManager.h"
#include "core/PlayerStore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>



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
    void testSyncConfigCopiesCatalogsWithoutPrivateHeroicSettings();
    // void testExtractSideloadDirectories();  // Disabled: methods not implemented in HeroicConfigManager
    // void testSyncSideloadToUser();  // Disabled: methods not implemented in HeroicConfigManager
    void testSyncShortcutsToUser();

private:
    void createMockHeroicConfig(const QString &basePath, bool isFlatpak);
    void createMockGameConfigs(const QString &basePath);
    void createMockShortcuts(const QString &basePath);

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
        defaultSettings[QStringLiteral("defaultWinePrefix")] =
            QString(basePath + QStringLiteral("/Games/Heroic/Prefixes/default"));
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
        install[QStringLiteral("executable")] =
            QString(basePath + QStringLiteral("/Games/Heroic/SideloadGame/game.exe"));
        game[QStringLiteral("install")] = install;

        games.append(game);
        root[QStringLiteral("games")] = games;
        sideloadFile.write(QJsonDocument(root).toJson());
        sideloadFile.close();
    }
    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic/SideloadGame"));
}

void TestHeroicConfigManager::createMockShortcuts(const QString &basePath)
{
    QString appsDir = basePath + QStringLiteral("/.local/share/applications");
    QDir().mkpath(appsDir);

    // Heroic shortcut
    QFile hFile(appsDir + QStringLiteral("/heroic-EpicGame.desktop"));
    if (hFile.open(QIODevice::WriteOnly)) {
        hFile.write("[Desktop Entry]\nName=Epic Game\nExec=heroic launch\n");
        hFile.close();
    }

    // Another Heroic shortcut
    QFile hFile2(appsDir + QStringLiteral("/heroic-GogGame.desktop"));
    if (hFile2.open(QIODevice::WriteOnly)) {
        hFile2.write("[Desktop Entry]\nName=GOG Game\nExec=heroic launch\n");
        hFile2.close();
    }

    // Non-Heroic shortcut (should not be synced)
    QFile otherFile(appsDir + QStringLiteral("/firefox.desktop"));
    if (otherFile.open(QIODevice::WriteOnly)) {
        otherFile.write("[Desktop Entry]\nName=Firefox\nExec=firefox\n");
        otherFile.close();
    }
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

    const QString externalGameDir = homeDir.path() + QStringLiteral("/external/InstalledGame");
    QVERIFY(QDir().mkpath(externalGameDir));
    QFile externalExe(externalGameDir + QStringLiteral("/Game.exe"));
    QVERIFY(externalExe.open(QIODevice::WriteOnly));
    externalExe.write("windows-game");
    externalExe.close();
    QVERIFY(externalExe.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));

    QJsonObject installedSideload;
    installedSideload[QStringLiteral("app_name")] = QStringLiteral("ExternalGame");
    installedSideload[QStringLiteral("title")] = QStringLiteral("External Game");
    installedSideload[QStringLiteral("folder_name")] = externalGameDir;
    installedSideload[QStringLiteral("is_installed")] = true;
    QJsonObject installedConfig;
    installedConfig[QStringLiteral("platform")] = QStringLiteral("Windows");
    installedConfig[QStringLiteral("executable")] = QString(externalGameDir + QStringLiteral("/Game.exe"));
    installedSideload[QStringLiteral("install")] = installedConfig;

    QJsonObject cachedSideload;
    cachedSideload[QStringLiteral("app_name")] = QStringLiteral("CachedGame");
    cachedSideload[QStringLiteral("title")] = QStringLiteral("Cached Game");
    cachedSideload[QStringLiteral("folder_name")] = externalGameDir;
    cachedSideload[QStringLiteral("is_installed")] = false;
    QJsonObject cachedConfig;
    cachedConfig[QStringLiteral("platform")] = QStringLiteral("Windows");
    cachedConfig[QStringLiteral("executable")] = QString(externalGameDir + QStringLiteral("/Missing.exe"));
    cachedSideload[QStringLiteral("install")] = cachedConfig;

    QJsonArray sideloadGames;
    sideloadGames.append(installedSideload);
    sideloadGames.append(cachedSideload);
    QJsonObject sideloadRoot;
    sideloadRoot[QStringLiteral("games")] = sideloadGames;
    const QString sideloadDir = homeDir.path() + QStringLiteral("/.config/heroic/sideload_apps");
    QVERIFY(QDir().mkpath(sideloadDir));
    QFile sideloadFile(sideloadDir + QStringLiteral("/library.json"));
    QVERIFY(sideloadFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    sideloadFile.write(QJsonDocument(sideloadRoot).toJson());
    sideloadFile.close();

    HeroicConfigManager manager;
    manager.loadGames();

    QStringList gameDirs = manager.extractGameDirectories();
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame")));
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/GogGame")));
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/AmazonGame")));
    QVERIFY(gameDirs.contains(externalGameDir));
    bool foundExternalGame = false;
    bool foundUninstalledGame = false;
    for (const HeroicGame &game : manager.installedGames()) {
        foundExternalGame |= game.appName == QStringLiteral("ExternalGame");
        foundUninstalledGame |= game.appName == QStringLiteral("CachedGame");
    }
    QVERIFY(foundExternalGame);
    QVERIFY(!foundUninstalledGame);
}

void TestHeroicConfigManager::testSyncConfigCopiesCatalogsWithoutPrivateHeroicSettings()
{
    QTemporaryDir sourceHome;
    QVERIFY(sourceHome.isValid());
    qputenv("HOME", sourceHome.path().toLocal8Bit());

    createMockHeroicConfig(sourceHome.path(), false);
    createMockGameConfigs(sourceHome.path());
    const QString sourceRoot = sourceHome.path() + QStringLiteral("/.config/heroic");
    QFile privateGameSettings(sourceRoot + QStringLiteral("/GamesConfig/EpicGameApp.json"));
    QVERIFY(privateGameSettings.open(QIODevice::WriteOnly));
    privateGameSettings.write(
        R"({"EpicGameApp":{"enviromentOptions":[{"name":"TOKEN","value":"private-test-token"}],"savesPath":"/private/saves"},"version":"v0","explicit":true})");
    privateGameSettings.close();
    QFile authFile(sourceRoot + QStringLiteral("/auth.json"));
    QVERIFY(authFile.open(QIODevice::WriteOnly));
    authFile.write("private-auth-test-data");
    authFile.close();

    HeroicConfigManager manager;
    QVERIFY(manager.syncConfigToUser(QStringLiteral("sync-target")));

    const QString targetRoot = PlayerStore::homeDir(QStringLiteral("sync-target")) + QStringLiteral("/.config/heroic");
    QVERIFY(QFile::exists(targetRoot + QStringLiteral("/sideload_apps/library.json")));
    QVERIFY(QFile::exists(targetRoot + QStringLiteral("/legendaryConfig/legendary/installed.json")));
    QVERIFY(QFile::exists(targetRoot + QStringLiteral("/gog_store/installed.json")));
    QVERIFY(QFile::exists(targetRoot + QStringLiteral("/nile_config/installed.json")));
    QVERIFY(!QFile::exists(targetRoot + QStringLiteral("/GamesConfig/EpicGameApp.json")));
    QVERIFY(!QFile::exists(targetRoot + QStringLiteral("/auth.json")));
}

void TestHeroicConfigManager::testSyncShortcutsToUser()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    createMockHeroicConfig(homeDir.path(), false);
    createMockShortcuts(homeDir.path());

    HeroicConfigManager manager;
    manager.detectHeroicPaths(); // Detect shortcuts dir

    const QString player = QStringLiteral("shortcut-target");
    QVERIFY(manager.syncShortcutsToUser(player));

    // Files are written straight into the player's home directory
    const QString targetApps = PlayerStore::homeDir(player) + QStringLiteral("/.local/share/applications");
    QVERIFY(QFile::exists(targetApps + QStringLiteral("/heroic-EpicGame.desktop")));
    QVERIFY(QFile::exists(targetApps + QStringLiteral("/heroic-GogGame.desktop")));
    QVERIFY(!QFile::exists(targetApps + QStringLiteral("/firefox.desktop")));

    // Invalid player names never resolve to a directory
    QVERIFY(!manager.syncShortcutsToUser(QStringLiteral("../escape")));
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
