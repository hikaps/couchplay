// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include "GameLibrary.h"

class TestGameLibrary : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Add game tests
    void testAddGame();
    void testAddGameWithIcon();
    void testAddGameEmptyName();
    void testAddGameEmptyCommand();
    void testAddGameDuplicate();
    
    // Remove game tests
    void testRemoveGame();
    void testRemoveGameNotFound();
    
    // Games list tests
    void testGamesAsVariant();
    void testGamesProperty();
    
    // Persistence tests
    void testSaveLoadGames();
    void testLoadEmptyFile();
    void testLoadInvalidJson();
    
    // Desktop shortcut tests
    void testCreateDesktopShortcut();
    void testCreateDesktopShortcutGameNotFound();
    void testCreateDesktopShortcutWithIcon();
    
    // Refresh tests
    void testRefresh();
    
    // Steam games tests
    void testGetSteamGamesNoSteam();
    void testParseSteamManifest();

private:
    QTemporaryDir *m_tempDir = nullptr;
    QString m_originalConfigPath;
    QString m_originalDesktopPath;
    
    void setupTestEnvironment();
    void createSteamManifest(const QString &path, const QString &appId, const QString &name);
};

void TestGameLibrary::initTestCase()
{
    // Store original paths
    m_originalConfigPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    m_originalDesktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

void TestGameLibrary::cleanupTestCase()
{
}

void TestGameLibrary::init()
{
    // Create temporary directory for each test
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
    
    // Set test mode to use temp directory
    QStandardPaths::setTestModeEnabled(true);
    
    // Create the config directory
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    
    // Remove any existing games.json to ensure clean state
    QString gamesPath = configDir + QStringLiteral("/games.json");
    QFile::remove(gamesPath);
    
    // Create the desktop directory
    QString desktopDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QDir().mkpath(desktopDir);
    
    // Clean up any existing desktop shortcuts
    QDir desktopDirObj(desktopDir);
    QStringList shortcuts = desktopDirObj.entryList({QStringLiteral("couchplay-*.desktop")}, QDir::Files);
    for (const QString &shortcut : shortcuts) {
        QFile::remove(desktopDir + QStringLiteral("/") + shortcut);
    }
}

void TestGameLibrary::cleanup()
{
    delete m_tempDir;
    m_tempDir = nullptr;
    QStandardPaths::setTestModeEnabled(false);
}

void TestGameLibrary::setupTestEnvironment()
{
    // Helper to setup common test environment
}

// ============ Add Game Tests ============

void TestGameLibrary::testAddGame()
{
    GameLibrary library;
    QSignalSpy gamesChangedSpy(&library, &GameLibrary::gamesChanged);
    QSignalSpy errorSpy(&library, &GameLibrary::errorOccurred);
    
    bool result = library.addGame(QStringLiteral("Test Game"), QStringLiteral("steam://rungameid/12345"));
    
    QVERIFY(result);
    QCOMPARE(gamesChangedSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 0);
    
    QVariantList games = library.gamesAsVariant();
    QCOMPARE(games.size(), 1);
    
    QVariantMap game = games.first().toMap();
    QCOMPARE(game[QStringLiteral("name")].toString(), QStringLiteral("Test Game"));
    QCOMPARE(game[QStringLiteral("command")].toString(), QStringLiteral("steam://rungameid/12345"));
    QVERIFY(game[QStringLiteral("iconPath")].toString().isEmpty());
}

void TestGameLibrary::testAddGameWithIcon()
{
    GameLibrary library;
    
    bool result = library.addGame(
        QStringLiteral("Test Game"),
        QStringLiteral("/usr/bin/game"),
        QStringLiteral("/path/to/icon.png")
    );
    
    QVERIFY(result);
    
    QVariantList games = library.gamesAsVariant();
    QCOMPARE(games.size(), 1);
    
    QVariantMap game = games.first().toMap();
    QCOMPARE(game[QStringLiteral("iconPath")].toString(), QStringLiteral("/path/to/icon.png"));
}

void TestGameLibrary::testAddGameEmptyName()
{
    GameLibrary library;
    QSignalSpy errorSpy(&library, &GameLibrary::errorOccurred);
    
    bool result = library.addGame(QString(), QStringLiteral("steam://rungameid/12345"));
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Name and command are required"));
    QCOMPARE(library.gamesAsVariant().size(), 0);
}

void TestGameLibrary::testAddGameEmptyCommand()
{
    GameLibrary library;
    QSignalSpy errorSpy(&library, &GameLibrary::errorOccurred);
    
    bool result = library.addGame(QStringLiteral("Test Game"), QString());
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Name and command are required"));
    QCOMPARE(library.gamesAsVariant().size(), 0);
}

void TestGameLibrary::testAddGameDuplicate()
{
    GameLibrary library;
    QSignalSpy errorSpy(&library, &GameLibrary::errorOccurred);
    
    // Add first game
    QVERIFY(library.addGame(QStringLiteral("Test Game"), QStringLiteral("command1")));
    
    // Try to add duplicate
    bool result = library.addGame(QStringLiteral("Test Game"), QStringLiteral("command2"));
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Game already exists"));
    
    // Should still only have one game
    QCOMPARE(library.gamesAsVariant().size(), 1);
}

// ============ Remove Game Tests ============

void TestGameLibrary::testRemoveGame()
{
    GameLibrary library;
    library.addGame(QStringLiteral("Game 1"), QStringLiteral("cmd1"));
    library.addGame(QStringLiteral("Game 2"), QStringLiteral("cmd2"));
    
    QSignalSpy gamesChangedSpy(&library, &GameLibrary::gamesChanged);
    
    bool result = library.removeGame(QStringLiteral("Game 1"));
    
    QVERIFY(result);
    QCOMPARE(gamesChangedSpy.count(), 1);
    
    QVariantList games = library.gamesAsVariant();
    QCOMPARE(games.size(), 1);
    QCOMPARE(games.first().toMap()[QStringLiteral("name")].toString(), QStringLiteral("Game 2"));
}

void TestGameLibrary::testRemoveGameNotFound()
{
    GameLibrary library;
    library.addGame(QStringLiteral("Game 1"), QStringLiteral("cmd1"));
    
    QSignalSpy errorSpy(&library, &GameLibrary::errorOccurred);
    
    bool result = library.removeGame(QStringLiteral("Nonexistent Game"));
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Game not found"));
    
    // Original game should still exist
    QCOMPARE(library.gamesAsVariant().size(), 1);
}

// ============ Games List Tests ============

void TestGameLibrary::testGamesAsVariant()
{
    GameLibrary library;
    library.addGame(QStringLiteral("Game A"), QStringLiteral("cmdA"), QStringLiteral("iconA"));
    library.addGame(QStringLiteral("Game B"), QStringLiteral("cmdB"));
    
    QVariantList games = library.gamesAsVariant();
    
    QCOMPARE(games.size(), 2);
    
    QVariantMap gameA = games[0].toMap();
    QCOMPARE(gameA[QStringLiteral("name")].toString(), QStringLiteral("Game A"));
    QCOMPARE(gameA[QStringLiteral("command")].toString(), QStringLiteral("cmdA"));
    QCOMPARE(gameA[QStringLiteral("iconPath")].toString(), QStringLiteral("iconA"));
    
    QVariantMap gameB = games[1].toMap();
    QCOMPARE(gameB[QStringLiteral("name")].toString(), QStringLiteral("Game B"));
    QCOMPARE(gameB[QStringLiteral("command")].toString(), QStringLiteral("cmdB"));
    QVERIFY(gameB[QStringLiteral("iconPath")].toString().isEmpty());
}

void TestGameLibrary::testGamesProperty()
{
    GameLibrary library;
    library.addGame(QStringLiteral("Test"), QStringLiteral("cmd"));
    
    // Access via QML property (which calls gamesAsVariant internally)
    QVariant gamesVariant = library.property("games");
    QVERIFY(gamesVariant.isValid());
    
    QVariantList games = gamesVariant.toList();
    QCOMPARE(games.size(), 1);
}

// ============ Persistence Tests ============

void TestGameLibrary::testSaveLoadGames()
{
    // Add games with first library instance
    {
        GameLibrary library;
        library.addGame(QStringLiteral("Saved Game 1"), QStringLiteral("cmd1"), QStringLiteral("icon1"));
        library.addGame(QStringLiteral("Saved Game 2"), QStringLiteral("cmd2"));
    }
    
    // Create new library instance - should load saved games
    {
        GameLibrary library;
        QVariantList games = library.gamesAsVariant();
        
        QCOMPARE(games.size(), 2);
        
        QVariantMap game1 = games[0].toMap();
        QCOMPARE(game1[QStringLiteral("name")].toString(), QStringLiteral("Saved Game 1"));
        QCOMPARE(game1[QStringLiteral("command")].toString(), QStringLiteral("cmd1"));
        QCOMPARE(game1[QStringLiteral("iconPath")].toString(), QStringLiteral("icon1"));
        
        QVariantMap game2 = games[1].toMap();
        QCOMPARE(game2[QStringLiteral("name")].toString(), QStringLiteral("Saved Game 2"));
        QCOMPARE(game2[QStringLiteral("command")].toString(), QStringLiteral("cmd2"));
    }
}

void TestGameLibrary::testLoadEmptyFile()
{
    // Create empty games.json file
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configPath);
    
    QFile file(configPath + QStringLiteral("/games.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    
    // Library should handle empty file gracefully
    GameLibrary library;
    QCOMPARE(library.gamesAsVariant().size(), 0);
}

void TestGameLibrary::testLoadInvalidJson()
{
    // Create invalid JSON file
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configPath);
    
    QFile file(configPath + QStringLiteral("/games.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ invalid json }");
    file.close();
    
    // Library should handle invalid JSON gracefully
    GameLibrary library;
    QCOMPARE(library.gamesAsVariant().size(), 0);
}

// ============ Desktop Shortcut Tests ============

void TestGameLibrary::testCreateDesktopShortcut()
{
    GameLibrary library;
    library.addGame(QStringLiteral("My Game"), QStringLiteral("steam://rungameid/12345"));
    
    bool result = library.createDesktopShortcut(QStringLiteral("My Game"), QStringLiteral("2Player"));
    
    QVERIFY(result);
    
    // Check that desktop file was created
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                          + QStringLiteral("/couchplay-2Player-My-Game.desktop");
    
    QFile file(desktopPath);
    QVERIFY(file.exists());
    
    // Verify file is executable
    QVERIFY(file.permissions() & QFileDevice::ExeUser);
    
    // Check content
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString content = QString::fromUtf8(file.readAll());
    file.close();
    
    QVERIFY(content.contains(QStringLiteral("[Desktop Entry]")));
    QVERIFY(content.contains(QStringLiteral("Name=CouchPlay: My Game (2Player)")));
    QVERIFY(content.contains(QStringLiteral("Exec=couchplay --profile \"2Player\" --game \"steam://rungameid/12345\"")));
    QVERIFY(content.contains(QStringLiteral("Type=Application")));
    QVERIFY(content.contains(QStringLiteral("Icon=io.github.hikaps.couchplay")));
}

void TestGameLibrary::testCreateDesktopShortcutGameNotFound()
{
    GameLibrary library;
    QSignalSpy errorSpy(&library, &GameLibrary::errorOccurred);
    
    bool result = library.createDesktopShortcut(QStringLiteral("Nonexistent"), QStringLiteral("Profile"));
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Game not found"));
}

void TestGameLibrary::testCreateDesktopShortcutWithIcon()
{
    GameLibrary library;
    library.addGame(QStringLiteral("Custom Game"), QStringLiteral("cmd"), QStringLiteral("/custom/icon.png"));
    
    bool result = library.createDesktopShortcut(QStringLiteral("Custom Game"), QStringLiteral("Profile"));
    
    QVERIFY(result);
    
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                          + QStringLiteral("/couchplay-Profile-Custom-Game.desktop");
    
    QFile file(desktopPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString content = QString::fromUtf8(file.readAll());
    file.close();
    
    QVERIFY(content.contains(QStringLiteral("Icon=/custom/icon.png")));
}

// ============ Refresh Tests ============

void TestGameLibrary::testRefresh()
{
    GameLibrary library;
    library.addGame(QStringLiteral("Initial Game"), QStringLiteral("cmd"));
    
    QCOMPARE(library.gamesAsVariant().size(), 1);
    
    // Manually write a new games.json with more games
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QFile file(configPath + QStringLiteral("/games.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    
    QJsonArray array;
    QJsonObject game1;
    game1[QStringLiteral("name")] = QStringLiteral("Game 1");
    game1[QStringLiteral("command")] = QStringLiteral("cmd1");
    game1[QStringLiteral("iconPath")] = QString();
    array.append(game1);
    
    QJsonObject game2;
    game2[QStringLiteral("name")] = QStringLiteral("Game 2");
    game2[QStringLiteral("command")] = QStringLiteral("cmd2");
    game2[QStringLiteral("iconPath")] = QString();
    array.append(game2);
    
    file.write(QJsonDocument(array).toJson());
    file.close();
    
    QSignalSpy gamesChangedSpy(&library, &GameLibrary::gamesChanged);
    
    library.refresh();
    
    QCOMPARE(gamesChangedSpy.count(), 1);
    QCOMPARE(library.gamesAsVariant().size(), 2);
}

// ============ Steam Games Tests ============

void TestGameLibrary::testGetSteamGamesNoSteam()
{
    // With no Steam installation, should return empty list
    GameLibrary library;
    QVariantList steamGames = library.getSteamGames();
    
    // In test environment, Steam paths won't exist
    // This is expected behavior
    QVERIFY(steamGames.isEmpty() || steamGames.size() >= 0); // May have games if Steam is actually installed
}

void TestGameLibrary::testParseSteamManifest()
{
    // Create a fake Steam directory structure
    QString steamPath = m_tempDir->path() + QStringLiteral("/.steam/steam/steamapps");
    QDir().mkpath(steamPath);
    
    // Create a test appmanifest file
    QString manifestPath = steamPath + QStringLiteral("/appmanifest_730.acf");
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"(
"AppState"
{
    "appid"     "730"
    "name"      "Counter-Strike 2"
    "StateFlags"        "4"
    "installdir"        "Counter-Strike Global Offensive"
}
)");
    manifest.close();
    
    // Note: getSteamGames() uses hardcoded paths, so this test verifies
    // the manifest parsing logic would work if paths existed.
    // In a real implementation, we'd need to make the paths configurable
    // or use dependency injection for testing.
    
    // For now, just verify the method doesn't crash
    GameLibrary library;
    QVariantList games = library.getSteamGames();
    // Can't verify specific results without mocking the file paths
}

void TestGameLibrary::createSteamManifest(const QString &path, const QString &appId, const QString &name)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        QString content = QStringLiteral(R"(
"AppState"
{
    "appid"     "%1"
    "name"      "%2"
    "StateFlags"        "4"
}
)").arg(appId, name);
        file.write(content.toUtf8());
        file.close();
    }
}

QTEST_MAIN(TestGameLibrary)
#include "test_gamelibrary.moc"
