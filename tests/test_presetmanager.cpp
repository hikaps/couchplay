// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <KSharedConfig>
#include <KConfigGroup>

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
    void testGetWorkingDirectory();
    void testGetLauncherId();
    void testGetSteamIntegration();

    void testAddCustomPreset();
    void testRemoveCustomPreset();

    void testGetSetSharedDirectories();

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
    QCOMPARE(steamCommand, QStringLiteral("steam -tenfoot -steamdeck"));

    QString heroicCommand = manager.getCommand(QStringLiteral("heroic"));
    QVERIFY(!heroicCommand.isEmpty());

    QString lutrisCommand = manager.getCommand(QStringLiteral("lutris"));
    QCOMPARE(lutrisCommand, QStringLiteral("lutris"));
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

void TestPresetManager::testGetSteamIntegration()
{
    PresetManager manager;

    QVERIFY(manager.getSteamIntegration(QStringLiteral("steam")));

    QVERIFY(!manager.getSteamIntegration(QStringLiteral("heroic")));
    QVERIFY(!manager.getSteamIntegration(QStringLiteral("lutris")));
}

void TestPresetManager::testAddCustomPreset()
{
    PresetManager manager;
    QSignalSpy presetsChangedSpy(&manager, &PresetManager::presetsChanged);

    QString id = manager.addCustomPreset(
        QStringLiteral("Test Game"),
        QStringLiteral("/path/to/game"),
        QStringLiteral("/working/dir"),
        QStringLiteral("test-icon"),
        true
    );

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
            QCOMPARE(preset[QStringLiteral("steamIntegration")].toBool(), true);
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
    QString id = manager.addCustomPreset(
        QStringLiteral("To Remove"),
        QStringLiteral("/path/to/game")
    );
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

    // Add a custom preset
    QString id = manager.addCustomPreset(
        QStringLiteral("Shared Test"),
        QStringLiteral("/path/to/game")
    );

    // Initially empty
    QStringList dirs = manager.getSharedDirectories(id);
    QVERIFY(dirs.isEmpty());
    presetsChangedSpy.clear();

    // Set shared directories
    QStringList newDirs = {QStringLiteral("/shared/dir1"), QStringLiteral("/shared/dir2")};
    bool result = manager.setSharedDirectories(id, newDirs);
    QVERIFY(result);
    QCOMPARE(presetsChangedSpy.count(), 1);

    // Verify they were set
    dirs = manager.getSharedDirectories(id);
    QCOMPARE(dirs, newDirs);
}

QTEST_MAIN(TestPresetManager)
#include "test_presetmanager.moc"
