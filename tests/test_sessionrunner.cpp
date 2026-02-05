// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QList>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <pwd.h>
#include <unistd.h>

#define private public
#include "SessionRunner.h"
#undef private
#include "SessionManager.h"
#include "PresetManager.h"
#include "SteamConfigManager.h"
#include "HeroicConfigManager.h"
#define private public
#include "CouchPlayHelperClient.h"
#undef private

class MockCouchPlayHelperClient : public CouchPlayHelperClient
{
    Q_OBJECT

public:
    using CouchPlayHelperClient::CouchPlayHelperClient;

    struct AclCall {
        QString path;
        QString username;
    };

    struct MountCall {
        QString username;
        uint compositorUid;
        QStringList directories;
    };

    QList<AclCall> aclCalls;
    QList<MountCall> mountCalls;

    explicit MockCouchPlayHelperClient(QObject *parent = nullptr)
        : CouchPlayHelperClient(parent)
    {
        m_available = true;
    }

    bool setPathAclWithParents(const QString &path, const QString &username) override
    {
        aclCalls.append({path, username});
        return true;
    }

    int mountSharedDirectories(const QString &username, uint compositorUid,
                               const QStringList &directories) override
    {
        mountCalls.append({username, compositorUid, directories});
        return directories.size();
    }
};

class TestSessionRunner : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Shared directories tests
    void testSetupSharedDirectoriesFormatting();
    void testSetupSharedDirectoriesMultipleInstances();
    void testSetupSharedDirectoriesNoSharedDirs();
    void testSetupSharedDirectoriesEmptyUsername();

    // Steam config tests
    void testSetupSteamConfigWithSteamLauncher();
    void testSetupSteamConfigWithNonSteamLauncher();
    void testSetupSteamConfigSteamIntegrationDisabled();
    void testSetupSteamConfigAppliesHeroicAcls();
    void testStartSessionHeroicPresetUsesAclsAndSharedConfig();

private:
    void createMockHeroicConfig(const QString &basePath);
    void createMockLegendaryConfig(const QString &basePath);

    QByteArray m_originalHome;
    SessionRunner *m_runner = nullptr;
    SessionManager *m_sessionManager = nullptr;
    PresetManager *m_presetManager = nullptr;
    SteamConfigManager *m_steamConfigManager = nullptr;
    MockCouchPlayHelperClient *m_helperClient = nullptr;
};

void TestSessionRunner::initTestCase()
{
    m_originalHome = qgetenv("HOME");
}

void TestSessionRunner::cleanupTestCase()
{
    if (!m_originalHome.isEmpty()) {
        qputenv("HOME", m_originalHome);
    }
}

void TestSessionRunner::init()
{
    m_sessionManager = new SessionManager(this);
    m_presetManager = new PresetManager(this);
    m_steamConfigManager = new SteamConfigManager(this);
    m_helperClient = new MockCouchPlayHelperClient(this);

    m_runner = new SessionRunner(this);

    m_runner->setSessionManager(m_sessionManager);
    m_runner->setHelperClient(m_helperClient);
    m_runner->setSteamConfigManager(m_steamConfigManager);
    m_runner->setPresetManager(m_presetManager);
}

void TestSessionRunner::cleanup()
{
    delete m_runner;
    m_runner = nullptr;
    delete m_sessionManager;
    m_sessionManager = nullptr;
    delete m_helperClient;
    m_helperClient = nullptr;
    delete m_steamConfigManager;
    m_steamConfigManager = nullptr;
    delete m_presetManager;
    m_presetManager = nullptr;
}

void TestSessionRunner::createMockHeroicConfig(const QString &basePath)
{
    QString heroicRoot = basePath + QStringLiteral("/.config/heroic");

    QDir().mkpath(heroicRoot + QStringLiteral("/GamesConfig"));

    QFile configFile(heroicRoot + QStringLiteral("/config.json"));
    if (configFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;
        QJsonObject defaultSettings;
        defaultSettings[QStringLiteral("defaultInstallPath")] = QString(basePath + QStringLiteral("/Games/Heroic"));
        root[QStringLiteral("defaultSettings")] = defaultSettings;
        configFile.write(QJsonDocument(root).toJson());
        configFile.close();
    }

    QDir().mkpath(basePath + QStringLiteral("/Games/Heroic"));
}

void TestSessionRunner::createMockLegendaryConfig(const QString &basePath)
{
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
}

void TestSessionRunner::testSetupSharedDirectoriesFormatting()
{
    m_sessionManager->setInstanceCount(1);
    QStringList sharedDirs = {QStringLiteral("/home/compositor/Games"),
                            QStringLiteral("/home/compositor/Saves")};
    m_sessionManager->setInstanceSharedDirectories(0, sharedDirs);

    QStringList expectedFormatted;
    for (const QString &dir : sharedDirs) {
        expectedFormatted << dir + QLatin1Char('|');
    }

    QCOMPARE(expectedFormatted[0], QStringLiteral("/home/compositor/Games|"));
    QCOMPARE(expectedFormatted[1], QStringLiteral("/home/compositor/Saves|"));
}

void TestSessionRunner::testSetupSharedDirectoriesMultipleInstances()
{
    m_sessionManager->setInstanceCount(2);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));

    QStringList dirs1 = {QStringLiteral("/home/compositor/Games1")};
    QStringList dirs2 = {QStringLiteral("/home/compositor/Games2"),
                      QStringLiteral("/home/compositor/Saves2")};

    m_sessionManager->setInstanceSharedDirectories(0, dirs1);
    m_sessionManager->setInstanceSharedDirectories(1, dirs2);

    QStringList expectedPlayer1 = {QStringLiteral("/home/compositor/Games1|")};
    QStringList expectedPlayer2 = {QStringLiteral("/home/compositor/Games2|"),
                                 QStringLiteral("/home/compositor/Saves2|")};

    QCOMPARE(expectedPlayer1.size(), 1);
    QCOMPARE(expectedPlayer2.size(), 2);
    QCOMPARE(expectedPlayer1[0], QStringLiteral("/home/compositor/Games1|"));
    QCOMPARE(expectedPlayer2[0], QStringLiteral("/home/compositor/Games2|"));
    QCOMPARE(expectedPlayer2[1], QStringLiteral("/home/compositor/Saves2|"));
}

void TestSessionRunner::testSetupSharedDirectoriesNoSharedDirs()
{
    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));

    m_sessionManager->setInstanceSharedDirectories(0, QStringList());

    QStringList expectedFormatted;

    QCOMPARE(expectedFormatted.size(), 0);
}

void TestSessionRunner::testSetupSharedDirectoriesEmptyUsername()
{
    m_sessionManager->setInstanceCount(1);

    QStringList sharedDirs = {QStringLiteral("/home/compositor/Games")};
    m_sessionManager->setInstanceSharedDirectories(0, sharedDirs);

    QVERIFY(m_sessionManager->getInstanceConfig(0).value(QStringLiteral("username")).toString().isEmpty());
}

void TestSessionRunner::testSetupSteamConfigWithSteamLauncher()
{
    LaunchPreset steamPreset = m_presetManager->getPreset(QStringLiteral("steam"));

    QVERIFY(steamPreset.launcherId == QStringLiteral("steam"));
    QVERIFY(steamPreset.steamIntegration);

    bool needsSteamSync = steamPreset.steamIntegration || steamPreset.launcherId == QStringLiteral("steam");
    QVERIFY(needsSteamSync);
}

void TestSessionRunner::testSetupSteamConfigWithNonSteamLauncher()
{
    LaunchPreset heroicPreset = m_presetManager->getPreset(QStringLiteral("heroic"));

    QVERIFY(heroicPreset.launcherId == QStringLiteral("heroic"));
    QVERIFY(!heroicPreset.steamIntegration);

    bool needsSteamSync = heroicPreset.steamIntegration || heroicPreset.launcherId == QStringLiteral("steam");
    QVERIFY(!needsSteamSync);
}

void TestSessionRunner::testSetupSteamConfigSteamIntegrationDisabled()
{
    m_steamConfigManager->setSyncShortcutsEnabled(false);

    QVERIFY(!m_steamConfigManager->syncShortcutsEnabled());
}

void TestSessionRunner::testSetupSteamConfigAppliesHeroicAcls()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    createMockHeroicConfig(homeDir.path());
    createMockLegendaryConfig(homeDir.path());

    HeroicConfigManager heroicManager;
    m_presetManager->setHeroicConfigManager(&heroicManager);

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, QStringLiteral("player1"));
    m_sessionManager->setInstancePreset(0, QStringLiteral("heroic"));

    m_runner->setupLauncherAccess();

    QString expectedPath = homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
    QCOMPARE(m_helperClient->aclCalls.size(), 1);
    QCOMPARE(m_helperClient->aclCalls[0].path, expectedPath);
    QCOMPARE(m_helperClient->aclCalls[0].username, QStringLiteral("player1"));
}

void TestSessionRunner::testStartSessionHeroicPresetUsesAclsAndSharedConfig()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    qputenv("HOME", homeDir.path().toLocal8Bit());

    createMockHeroicConfig(homeDir.path());
    createMockLegendaryConfig(homeDir.path());

    HeroicConfigManager heroicManager;
    m_presetManager->setHeroicConfigManager(&heroicManager);

    struct passwd *pw = getpwuid(getuid());
    QString sessionUser = pw ? QString::fromLocal8Bit(pw->pw_name) : QStringLiteral("compositor");

    m_sessionManager->setInstanceCount(1);
    m_sessionManager->setInstanceUser(0, sessionUser);
    m_sessionManager->setInstancePreset(0, QStringLiteral("heroic"));
    m_sessionManager->setInstanceSharedDirectories(0, {heroicManager.configPath()});

    QVERIFY(m_runner->start());

    QCOMPARE(m_helperClient->mountCalls.size(), 1);
    QCOMPARE(m_helperClient->mountCalls[0].username, sessionUser);
    QCOMPARE(m_helperClient->mountCalls[0].compositorUid, static_cast<uint>(getuid()));
    QCOMPARE(m_helperClient->mountCalls[0].directories.size(), 1);
    QCOMPARE(m_helperClient->mountCalls[0].directories[0], heroicManager.configPath() + QLatin1Char('|'));
    QVERIFY(!m_helperClient->mountCalls[0].directories.contains(heroicManager.defaultInstallPath() + QLatin1Char('|')));

    QString expectedPath = homeDir.path() + QStringLiteral("/Games/Heroic/EpicGame");
    QCOMPARE(m_helperClient->aclCalls.size(), 1);
    QCOMPARE(m_helperClient->aclCalls[0].path, expectedPath);
    QCOMPARE(m_helperClient->aclCalls[0].username, sessionUser);
}

QTEST_MAIN(TestSessionRunner)
#include "test_sessionrunner.moc"
