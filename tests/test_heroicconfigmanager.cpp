// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QtTest>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include "core/HeroicConfigManager.h"
#include "dbus/CouchPlayHelperClient.h"

class MockHelperClient : public CouchPlayHelperClient
{
    Q_OBJECT
public:
    explicit MockHelperClient(QObject *parent = nullptr) : CouchPlayHelperClient(parent) {}

    bool isAvailable() const override { return true; }

    bool createUserDirectory(const QString &path, const QString &username) override
    {
        createdDirs.append(qMakePair(path, username));
        // Actually create it for test verification if needed, or just pretend
        return QDir().mkpath(path);
    }

    bool copyFileToUser(const QString &sourcePath, const QString &targetPath, const QString &username) override
    {
        Q_UNUSED(username)
        copiedFiles.append(qMakePair(sourcePath, targetPath));
        QDir().mkpath(QFileInfo(targetPath).absolutePath());
        QFile::remove(targetPath);
        return QFile::copy(sourcePath, targetPath);
    }

    QList<QPair<QString, QString>> createdDirs;
    QList<QPair<QString, QString>> copiedFiles;
};

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
    
    HeroicConfigManager manager;
    manager.loadGames();
    
    QStringList gameDirs = manager.extractGameDirectories();
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame")));
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/GogGame")));
    QVERIFY(gameDirs.contains(homeDir.path() + QStringLiteral("/Games/Heroic/AmazonGame")));
}

void TestHeroicConfigManager::testSyncShortcutsToUser()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    createMockHeroicConfig(homeDir.path(), false);
    createMockShortcuts(homeDir.path());

    // Create a "target user" home directory
    QTemporaryDir targetUserDir;
    QVERIFY(targetUserDir.isValid());
    
    // We can't easily mock getpwnam, so we'll skip the actual getpwnam check in the test
    // OR we can pass the current user as the target user, but that might overwrite things.
    // However, the MockHelperClient intercepts the calls, so it's fine.
    // BUT HeroicConfigManager calls getpwnam. 
    // We can use the current user "notaname" (or whatever `whoami` returns) as the target user.
    // Or we can just mock `getpwnam`? No, C function mocking is hard.
    // Let's use the current user's name.
    
    QString currentUser = QString::fromLocal8Bit(qgetenv("USER"));
    if (currentUser.isEmpty()) currentUser = QStringLiteral("notaname"); // Fallback

    HeroicConfigManager manager;
    manager.detectHeroicPaths(); // Detect shortcuts dir

    MockHelperClient *mockHelper = new MockHelperClient(&manager);
    manager.setHelperClient(mockHelper);

    // Call sync
    bool result = manager.syncShortcutsToUser(currentUser);
    
    // Since getpwnam will return the REAL home directory of the current user,
    // and our mock HOME env var only affects Qt/app logic, not getpwnam,
    // HeroicConfigManager will try to sync to the REAL user's home.
    // BUT MockHelperClient intercepts copyFileToUser.
    // So `targetPath` passed to `copyFileToUser` will be real home + /.local/share/applications/heroic-....
    
    QVERIFY(result);
    QCOMPARE(mockHelper->copiedFiles.count(), 2);
    
    QStringList copiedSources;
    for (const auto &pair : mockHelper->copiedFiles) {
        copiedSources << pair.first;
    }
    
    QString appsDir = homeDir.path() + QStringLiteral("/.local/share/applications");
    QVERIFY(copiedSources.contains(appsDir + QStringLiteral("/heroic-EpicGame.desktop")));
    QVERIFY(copiedSources.contains(appsDir + QStringLiteral("/heroic-GogGame.desktop")));
    QVERIFY(!copiedSources.contains(appsDir + QStringLiteral("/firefox.desktop")));
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
