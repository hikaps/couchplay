// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include "PresetManager.h"

class TestPresetManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Builtin preset tests
    void testBuiltinPresetsExist();
    void testSteamPresetProperties();
    void testLutrisPresetProperties();

    // Preset lookup tests
    void testGetPresetById();
    void testGetPresetByIdNotFound();
    void testGetCommand();
    void testGetWorkingDirectory();
    void testGetSteamIntegration();

    // Custom preset CRUD tests
    void testAddCustomPreset();
    void testAddCustomPresetEmitsSignal();
    void testUpdateCustomPreset();
    void testUpdateBuiltinPresetFails();
    void testRemoveCustomPreset();
    void testRemoveBuiltinPresetFails();

    // Exec= field cleaning tests
    void testCleanExecCommand();
    void testCleanExecCommandMultipleCodes();
    void testCleanExecCommandNoChanges();

    // Desktop file parsing tests
    void testAddPresetFromDesktopFile();
    void testAddPresetFromDesktopFileInvalid();
    void testAddPresetFromDesktopFileDuplicate();

    // Persistence tests
    void testCustomPresetsPersist();

    // Signal tests
    void testPresetsChangedSignal();

private:
    PresetManager *m_presetManager = nullptr;
    QTemporaryDir *m_tempDir = nullptr;
    QString m_originalConfigPath;

    QString createTestDesktopFile(const QString &name, const QString &exec,
                                   const QString &icon = QString(),
                                   const QString &path = QString(),
                                   bool hidden = false);
};

void TestPresetManager::initTestCase()
{
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    // Set XDG_CONFIG_HOME to temp dir so we don't affect real config
    m_originalConfigPath = qEnvironmentVariable("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", m_tempDir->path().toUtf8());
}

void TestPresetManager::cleanupTestCase()
{
    // Restore original config path
    if (m_originalConfigPath.isEmpty()) {
        qunsetenv("XDG_CONFIG_HOME");
    } else {
        qputenv("XDG_CONFIG_HOME", m_originalConfigPath.toUtf8());
    }

    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestPresetManager::init()
{
    m_presetManager = new PresetManager(this);
}

void TestPresetManager::cleanup()
{
    delete m_presetManager;
    m_presetManager = nullptr;

    // Clean up preset config file
    QString configPath = m_tempDir->path() + QStringLiteral("/couchplay/presets.json");
    QFile::remove(configPath);
}

// Helper to create test .desktop files
QString TestPresetManager::createTestDesktopFile(const QString &name, const QString &exec,
                                                   const QString &icon, const QString &path,
                                                   bool hidden)
{
    QTemporaryFile *file = new QTemporaryFile(m_tempDir->path() + QStringLiteral("/XXXXXX.desktop"));
    file->setAutoRemove(false);  // Keep for test duration
    if (!file->open()) {
        delete file;
        return QString();
    }

    QTextStream stream(file);
    stream << "[Desktop Entry]\n";
    stream << "Type=Application\n";
    stream << "Name=" << name << "\n";
    stream << "Exec=" << exec << "\n";
    if (!icon.isEmpty()) {
        stream << "Icon=" << icon << "\n";
    }
    if (!path.isEmpty()) {
        stream << "Path=" << path << "\n";
    }
    if (hidden) {
        stream << "Hidden=true\n";
    }
    stream.flush();

    QString filePath = file->fileName();
    file->close();
    delete file;

    return filePath;
}

void TestPresetManager::testBuiltinPresetsExist()
{
    QList<LaunchPreset> presets = m_presetManager->presets();

    QVERIFY(presets.size() >= 2);

    bool foundSteam = false;
    bool foundLutris = false;
    for (const LaunchPreset &preset : presets) {
        if (preset.id == QStringLiteral("steam")) {
            foundSteam = true;
            QVERIFY(preset.isBuiltin);
        }
        if (preset.id == QStringLiteral("lutris")) {
            foundLutris = true;
            QVERIFY(preset.isBuiltin);
        }
    }

    QVERIFY2(foundSteam, "Steam preset not found");
    QVERIFY2(foundLutris, "Lutris preset not found");
}

void TestPresetManager::testSteamPresetProperties()
{
    LaunchPreset steam = m_presetManager->getPreset(QStringLiteral("steam"));

    QCOMPARE(steam.id, QStringLiteral("steam"));
    QCOMPARE(steam.name, QStringLiteral("Steam Big Picture"));
    QCOMPARE(steam.command, QStringLiteral("steam -tenfoot -steamdeck"));
    QCOMPARE(steam.iconName, QStringLiteral("steam"));
    QVERIFY(steam.isBuiltin);
    QVERIFY(steam.steamIntegration);
}

void TestPresetManager::testLutrisPresetProperties()
{
    LaunchPreset lutris = m_presetManager->getPreset(QStringLiteral("lutris"));

    QCOMPARE(lutris.id, QStringLiteral("lutris"));
    QCOMPARE(lutris.name, QStringLiteral("Lutris"));
    QCOMPARE(lutris.command, QStringLiteral("lutris"));
    QCOMPARE(lutris.iconName, QStringLiteral("lutris"));
    QVERIFY(lutris.isBuiltin);
    QVERIFY(!lutris.steamIntegration);
}

void TestPresetManager::testGetPresetById()
{
    LaunchPreset preset = m_presetManager->getPreset(QStringLiteral("steam"));
    QCOMPARE(preset.id, QStringLiteral("steam"));

    preset = m_presetManager->getPreset(QStringLiteral("lutris"));
    QCOMPARE(preset.id, QStringLiteral("lutris"));
}

void TestPresetManager::testGetPresetByIdNotFound()
{
    // When not found, should return default (Steam)
    LaunchPreset preset = m_presetManager->getPreset(QStringLiteral("nonexistent"));
    QCOMPARE(preset.id, QStringLiteral("steam"));
}

void TestPresetManager::testGetCommand()
{
    QString command = m_presetManager->getCommand(QStringLiteral("steam"));
    QCOMPARE(command, QStringLiteral("steam -tenfoot -steamdeck"));

    command = m_presetManager->getCommand(QStringLiteral("lutris"));
    QCOMPARE(command, QStringLiteral("lutris"));
}

void TestPresetManager::testGetWorkingDirectory()
{
    // Builtin presets have no working directory
    QString workDir = m_presetManager->getWorkingDirectory(QStringLiteral("steam"));
    QVERIFY(workDir.isEmpty());
}

void TestPresetManager::testGetSteamIntegration()
{
    QVERIFY(m_presetManager->getSteamIntegration(QStringLiteral("steam")));
    QVERIFY(!m_presetManager->getSteamIntegration(QStringLiteral("lutris")));
}

void TestPresetManager::testAddCustomPreset()
{
    QString id = m_presetManager->addCustomPreset(
        QStringLiteral("Test Game"),
        QStringLiteral("/usr/bin/testgame"),
        QStringLiteral("/home/user/games"),
        QStringLiteral("testgame-icon"),
        true  // steamIntegration
    );

    QVERIFY(!id.isEmpty());
    QVERIFY(id.startsWith(QStringLiteral("custom-")));

    LaunchPreset preset = m_presetManager->getPreset(id);
    QCOMPARE(preset.id, id);
    QCOMPARE(preset.name, QStringLiteral("Test Game"));
    QCOMPARE(preset.command, QStringLiteral("/usr/bin/testgame"));
    QCOMPARE(preset.workingDirectory, QStringLiteral("/home/user/games"));
    QCOMPARE(preset.iconName, QStringLiteral("testgame-icon"));
    QVERIFY(!preset.isBuiltin);
    QVERIFY(preset.steamIntegration);
}

void TestPresetManager::testAddCustomPresetEmitsSignal()
{
    QSignalSpy spy(m_presetManager, &PresetManager::presetsChanged);

    m_presetManager->addCustomPreset(
        QStringLiteral("Signal Test"),
        QStringLiteral("/bin/true")
    );

    QCOMPARE(spy.count(), 1);
}

void TestPresetManager::testUpdateCustomPreset()
{
    QString id = m_presetManager->addCustomPreset(
        QStringLiteral("Original Name"),
        QStringLiteral("original-command")
    );

    bool result = m_presetManager->updateCustomPreset(
        id,
        QStringLiteral("Updated Name"),
        QStringLiteral("updated-command"),
        QStringLiteral("/new/path"),
        QStringLiteral("new-icon"),
        true
    );

    QVERIFY(result);

    LaunchPreset preset = m_presetManager->getPreset(id);
    QCOMPARE(preset.name, QStringLiteral("Updated Name"));
    QCOMPARE(preset.command, QStringLiteral("updated-command"));
    QCOMPARE(preset.workingDirectory, QStringLiteral("/new/path"));
    QCOMPARE(preset.iconName, QStringLiteral("new-icon"));
    QVERIFY(preset.steamIntegration);
}

void TestPresetManager::testUpdateBuiltinPresetFails()
{
    bool result = m_presetManager->updateCustomPreset(
        QStringLiteral("steam"),
        QStringLiteral("Hacked Steam"),
        QStringLiteral("malicious-command"),
        QString(),
        QString(),
        false
    );

    QVERIFY(!result);

    // Verify Steam preset is unchanged
    LaunchPreset steam = m_presetManager->getPreset(QStringLiteral("steam"));
    QCOMPARE(steam.name, QStringLiteral("Steam Big Picture"));
    QCOMPARE(steam.command, QStringLiteral("steam -tenfoot -steamdeck"));
}

void TestPresetManager::testRemoveCustomPreset()
{
    QString id = m_presetManager->addCustomPreset(
        QStringLiteral("To Be Deleted"),
        QStringLiteral("/bin/delete-me")
    );

    // Verify it exists
    LaunchPreset preset = m_presetManager->getPreset(id);
    QCOMPARE(preset.id, id);

    // Remove it
    bool result = m_presetManager->removeCustomPreset(id);
    QVERIFY(result);

    // Verify it's gone (should return default now)
    preset = m_presetManager->getPreset(id);
    QCOMPARE(preset.id, QStringLiteral("steam"));
}

void TestPresetManager::testRemoveBuiltinPresetFails()
{
    bool result = m_presetManager->removeCustomPreset(QStringLiteral("steam"));
    QVERIFY(!result);

    // Steam should still exist
    LaunchPreset steam = m_presetManager->getPreset(QStringLiteral("steam"));
    QCOMPARE(steam.id, QStringLiteral("steam"));
}

void TestPresetManager::testCleanExecCommand()
{
    // Test single field code removal
    QString cleaned = PresetManager::cleanExecCommand(QStringLiteral("steam %U"));
    QCOMPARE(cleaned, QStringLiteral("steam"));

    cleaned = PresetManager::cleanExecCommand(QStringLiteral("game --launch %f"));
    QCOMPARE(cleaned, QStringLiteral("game --launch"));
}

void TestPresetManager::testCleanExecCommandMultipleCodes()
{
    QString cleaned = PresetManager::cleanExecCommand(
        QStringLiteral("flatpak run com.example.Game %u %F %i %c"));
    QCOMPARE(cleaned, QStringLiteral("flatpak run com.example.Game"));
}

void TestPresetManager::testCleanExecCommandNoChanges()
{
    QString cleaned = PresetManager::cleanExecCommand(
        QStringLiteral("/usr/bin/game --option value"));
    QCOMPARE(cleaned, QStringLiteral("/usr/bin/game --option value"));
}

void TestPresetManager::testAddPresetFromDesktopFile()
{
    QString desktopFile = createTestDesktopFile(
        QStringLiteral("Test Application"),
        QStringLiteral("/usr/bin/testapp %U"),
        QStringLiteral("testapp-icon"),
        QStringLiteral("/opt/testapp")
    );

    // Verify file was created
    QVERIFY2(!desktopFile.isEmpty(), "Failed to create test desktop file");
    QVERIFY2(QFile::exists(desktopFile), "Desktop file does not exist");

    QString id = m_presetManager->addPresetFromDesktopFile(desktopFile);

    QVERIFY2(!id.isEmpty(), qPrintable(QStringLiteral("Failed to add preset from: %1").arg(desktopFile)));
    QVERIFY(id.startsWith(QStringLiteral("custom-")));

    LaunchPreset preset = m_presetManager->getPreset(id);
    QCOMPARE(preset.name, QStringLiteral("Test Application"));
    QCOMPARE(preset.command, QStringLiteral("/usr/bin/testapp"));  // %U stripped
    QCOMPARE(preset.iconName, QStringLiteral("testapp-icon"));
    QCOMPARE(preset.workingDirectory, QStringLiteral("/opt/testapp"));
    QCOMPARE(preset.desktopFilePath, desktopFile);
    QVERIFY(!preset.isBuiltin);

    // Cleanup
    QFile::remove(desktopFile);
}

void TestPresetManager::testAddPresetFromDesktopFileInvalid()
{
    QSignalSpy spy(m_presetManager, &PresetManager::errorOccurred);

    QString id = m_presetManager->addPresetFromDesktopFile(
        QStringLiteral("/nonexistent/path/to/app.desktop"));

    QVERIFY(id.isEmpty());
    QCOMPARE(spy.count(), 1);
}

void TestPresetManager::testAddPresetFromDesktopFileDuplicate()
{
    QString desktopFile = createTestDesktopFile(
        QStringLiteral("Duplicate Test"),
        QStringLiteral("/usr/bin/duptest")
    );

    // Verify file was created
    QVERIFY2(!desktopFile.isEmpty(), "Failed to create test desktop file");
    QVERIFY2(QFile::exists(desktopFile), "Desktop file does not exist");

    QString id1 = m_presetManager->addPresetFromDesktopFile(desktopFile);
    QVERIFY2(!id1.isEmpty(), "Failed to add preset from desktop file");

    // Adding same file again should return existing ID
    QString id2 = m_presetManager->addPresetFromDesktopFile(desktopFile);
    QCOMPARE(id1, id2);

    // Cleanup
    QFile::remove(desktopFile);
}

void TestPresetManager::testCustomPresetsPersist()
{
    // Add a custom preset
    QString id = m_presetManager->addCustomPreset(
        QStringLiteral("Persistent Preset"),
        QStringLiteral("/usr/bin/persist"),
        QStringLiteral("/home/persist"),
        QStringLiteral("persist-icon"),
        true
    );

    // Delete this manager
    delete m_presetManager;

    // Create a new manager - should load from disk
    m_presetManager = new PresetManager(this);

    // Verify preset was loaded
    LaunchPreset preset = m_presetManager->getPreset(id);
    QCOMPARE(preset.id, id);
    QCOMPARE(preset.name, QStringLiteral("Persistent Preset"));
    QCOMPARE(preset.command, QStringLiteral("/usr/bin/persist"));
    QCOMPARE(preset.workingDirectory, QStringLiteral("/home/persist"));
    QCOMPARE(preset.iconName, QStringLiteral("persist-icon"));
    QVERIFY(preset.steamIntegration);
}

void TestPresetManager::testPresetsChangedSignal()
{
    QSignalSpy spy(m_presetManager, &PresetManager::presetsChanged);

    // Add
    QString id = m_presetManager->addCustomPreset(
        QStringLiteral("Signal Test 1"),
        QStringLiteral("/bin/true")
    );
    QCOMPARE(spy.count(), 1);

    // Update
    m_presetManager->updateCustomPreset(id, QStringLiteral("Updated"), QStringLiteral("/bin/false"),
                                         QString(), QString(), false);
    QCOMPARE(spy.count(), 2);

    // Remove
    m_presetManager->removeCustomPreset(id);
    QCOMPARE(spy.count(), 3);

    // Refresh
    m_presetManager->refresh();
    QCOMPARE(spy.count(), 4);
}

QTEST_MAIN(TestPresetManager)
#include "test_presetmanager.moc"
