// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "../src/core/PlayerStore.h"
#include "../src/core/SunshineConfig.h"

class TestPlayerStore : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void validNames_data();
    void validNames();
    void rootIsUnderHome();
    void createExistsAndList();
    void createUsesPrivatePermissions();
    void createDuplicateFails();
    void createInvalidFails();
    void pathsForInvalidNamesAreEmpty();
    void homeAndLayerPaths();
    void layerDirKeyIsStableAndPerSource();
    void deleteRemovesDirectory();
    void deleteRemovesSunshineState();
    void deleteInvalidNameDoesNotTouchOutside();
    void refreshPicksUpExternalPlayers();
    void refreshIgnoresInvalidDirectories();

private:
    QTemporaryDir m_home;
    QByteArray m_flatpakBefore;
    bool m_flatpakWasSet = false;
};

void TestPlayerStore::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void TestPlayerStore::init()
{
    QVERIFY(m_home.isValid());
    // Fresh HOME per test, native layout (FLATPAK_ID would switch the root to AppDataLocation).
    m_flatpakWasSet = qEnvironmentVariableIsSet("FLATPAK_ID");
    m_flatpakBefore = qgetenv("FLATPAK_ID");
    qunsetenv("FLATPAK_ID");
    qputenv("HOME", m_home.path().toUtf8());
    QDir(m_home.path()).removeRecursively();
    QDir().mkpath(m_home.path());
}

void TestPlayerStore::cleanup()
{
    if (m_flatpakWasSet) {
        qputenv("FLATPAK_ID", m_flatpakBefore);
    }
}

void TestPlayerStore::validNames_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("valid");

    QTest::newRow("simple") << QStringLiteral("player1") << true;
    QTest::newRow("underscore start") << QStringLiteral("_p") << true;
    QTest::newRow("dash and digits") << QStringLiteral("p-1_x") << true;
    QTest::newRow("single char") << QStringLiteral("a") << true;
    QTest::newRow("32 chars") << QString(32, QLatin1Char('a')) << true;
    QTest::newRow("33 chars") << QString(33, QLatin1Char('a')) << false;
    QTest::newRow("empty") << QStringLiteral("") << false;
    QTest::newRow("dot") << QStringLiteral(".") << false;
    QTest::newRow("dotdot") << QStringLiteral("..") << false;
    QTest::newRow("traversal") << QStringLiteral("../x") << false;
    QTest::newRow("slash") << QStringLiteral("a/b") << false;
    QTest::newRow("leading slash") << QStringLiteral("/etc") << false;
    QTest::newRow("uppercase") << QStringLiteral("Root") << false;
    QTest::newRow("leading digit") << QStringLiteral("1p") << false;
    QTest::newRow("leading dash") << QStringLiteral("-p") << false;
    QTest::newRow("space") << QStringLiteral("a b") << false;
    }

void TestPlayerStore::validNames()
{
    QFETCH(QString, name);
    QFETCH(bool, valid);
    QCOMPARE(PlayerStore::isValidName(name), valid);
}

void TestPlayerStore::rootIsUnderHome()
{
    QCOMPARE(PlayerStore::rootDir(), QDir::homePath() + QStringLiteral("/.local/share/couchplay/players"));
}

void TestPlayerStore::createExistsAndList()
{
    PlayerStore store;
    QVERIFY(store.players().isEmpty());
    QVERIFY(!store.exists(QStringLiteral("alice")));

    QSignalSpy changed(&store, &PlayerStore::playersChanged);
    QSignalSpy created(&store, &PlayerStore::playerCreated);
    QVERIFY(store.createPlayer(QStringLiteral("bob")));
    QVERIFY(store.createPlayer(QStringLiteral("alice")));

    QVERIFY(store.exists(QStringLiteral("alice")));
    QVERIFY(QFileInfo(PlayerStore::homeDir(QStringLiteral("alice"))).isDir());
    QCOMPARE(store.players(), (QStringList{QStringLiteral("alice"), QStringLiteral("bob")}));
    QCOMPARE(changed.count(), 2);
    QCOMPARE(created.count(), 2);
    QCOMPARE(created.at(0).at(0).toString(), QStringLiteral("bob"));
}

void TestPlayerStore::createUsesPrivatePermissions()
{
    PlayerStore store;
    QVERIFY(store.createPlayer(QStringLiteral("alice")));
    const QFileDevice::Permissions expected =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadUser
        | QFileDevice::WriteUser | QFileDevice::ExeUser; // Qt reports owner bits as both Owner and User
    QCOMPARE(QFileInfo(PlayerStore::rootDir()).permissions(), expected);
    QCOMPARE(QFileInfo(PlayerStore::rootDir() + QStringLiteral("/alice")).permissions(), expected);
}

void TestPlayerStore::createDuplicateFails()
{
    PlayerStore store;
    QVERIFY(store.createPlayer(QStringLiteral("alice")));

    // Content must survive a rejected duplicate.
    QFile marker(PlayerStore::homeDir(QStringLiteral("alice")) + QStringLiteral("/save.dat"));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.close();

    QSignalSpy errors(&store, &PlayerStore::errorOccurred);
    QVERIFY(!store.createPlayer(QStringLiteral("alice")));
    QCOMPARE(errors.count(), 1);
    QVERIFY(QFileInfo::exists(marker.fileName()));
    QCOMPARE(store.players(), QStringList{QStringLiteral("alice")});
}

void TestPlayerStore::createInvalidFails()
{
    PlayerStore store;
    QSignalSpy errors(&store, &PlayerStore::errorOccurred);
    QVERIFY(!store.createPlayer(QStringLiteral("../x")));
    QVERIFY(!store.createPlayer(QString()));
    QVERIFY(!store.createPlayer(QStringLiteral("Root")));
    QCOMPARE(errors.count(), 3);
    QVERIFY(store.players().isEmpty());
    QVERIFY(!QFileInfo::exists(m_home.path() + QStringLiteral("/.local/share/couchplay/x")));
}

void TestPlayerStore::pathsForInvalidNamesAreEmpty()
{
    QVERIFY(PlayerStore::homeDir(QStringLiteral("../x")).isEmpty());
    QVERIFY(PlayerStore::homeDir(QString()).isEmpty());
    QVERIFY(PlayerStore::layerDir(QStringLiteral("../x"), QStringLiteral("/src")).isEmpty());
    PlayerStore store;
    QVERIFY(!store.exists(QStringLiteral("../x")));
}

void TestPlayerStore::homeAndLayerPaths()
{
    const QString root = PlayerStore::rootDir();
    QCOMPARE(PlayerStore::homeDir(QStringLiteral("alice")), root + QStringLiteral("/alice/home"));

    const QString layer = PlayerStore::layerDir(QStringLiteral("alice"), QStringLiteral("/games/steam"));
    QVERIFY(layer.startsWith(root + QStringLiteral("/alice/layers/")));
    const QString key = layer.mid((root + QStringLiteral("/alice/layers/")).size());
    QCOMPARE(key.size(), 16);
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{16}$")).match(key).hasMatch());
}

void TestPlayerStore::layerDirKeyIsStableAndPerSource()
{
    const QString a = PlayerStore::layerDir(QStringLiteral("alice"), QStringLiteral("/games/steam"));
    // Same source, differently spelled, is the same layer.
    QCOMPARE(PlayerStore::layerDir(QStringLiteral("alice"), QStringLiteral("/games//steam/")), a);
    QCOMPARE(PlayerStore::layerDir(QStringLiteral("alice"), QStringLiteral("/games/./steam")), a);
    // Different sources and different players get different layers.
    QVERIFY(PlayerStore::layerDir(QStringLiteral("alice"), QStringLiteral("/games/other")) != a);
    QVERIFY(PlayerStore::layerDir(QStringLiteral("bob"), QStringLiteral("/games/steam")) != a);
    QCOMPARE(QFileInfo(a).fileName(),
             QFileInfo(PlayerStore::layerDir(QStringLiteral("bob"), QStringLiteral("/games/steam"))).fileName());
}

void TestPlayerStore::deleteRemovesDirectory()
{
    PlayerStore store;
    QVERIFY(store.createPlayer(QStringLiteral("alice")));
    QVERIFY(store.createPlayer(QStringLiteral("bob")));

    // Nested content, including overlay layers, is removed with the player.
    QVERIFY(QDir().mkpath(PlayerStore::layerDir(QStringLiteral("alice"), QStringLiteral("/s")) + QStringLiteral("/upper")));
    QVERIFY(QDir().mkpath(PlayerStore::homeDir(QStringLiteral("alice")) + QStringLiteral("/.config/x")));

    QSignalSpy deleted(&store, &PlayerStore::playerDeleted);
    QVERIFY(store.deletePlayer(QStringLiteral("alice")));
    QCOMPARE(deleted.count(), 1);
    QVERIFY(!store.exists(QStringLiteral("alice")));
    QVERIFY(!QFileInfo::exists(PlayerStore::rootDir() + QStringLiteral("/alice")));
    QCOMPARE(store.players(), QStringList{QStringLiteral("bob")});
    QVERIFY(store.exists(QStringLiteral("bob")));
}

void TestPlayerStore::deleteRemovesSunshineState()
{
    PlayerStore store;
    QVERIFY(store.createPlayer(QStringLiteral("alice")));

    const QString state = SunshineConfig::stateDirectory(QStringLiteral("alice"));
    QVERIFY(QDir().mkpath(state));
    QFile pairing(state + QStringLiteral("/sunshine_state.json"));
    QVERIFY(pairing.open(QIODevice::WriteOnly));
    pairing.write("{}");
    pairing.close();

    // Another player's state must be left alone.
    const QString otherState = SunshineConfig::stateDirectory(QStringLiteral("bob"));
    QVERIFY(QDir().mkpath(otherState));

    QVERIFY(store.deletePlayer(QStringLiteral("alice")));
    QVERIFY(!QFileInfo::exists(state));
    QVERIFY(QFileInfo(otherState).isDir());
}

void TestPlayerStore::deleteInvalidNameDoesNotTouchOutside()
{
    PlayerStore store;
    QVERIFY(store.createPlayer(QStringLiteral("alice")));
    const QString outside = m_home.path() + QStringLiteral("/.local/share/couchplay/precious");
    QVERIFY(QDir().mkpath(outside));

    QSignalSpy errors(&store, &PlayerStore::errorOccurred);
    QVERIFY(!store.deletePlayer(QStringLiteral("../precious")));
    QVERIFY(!store.deletePlayer(QString()));
    QCOMPARE(errors.count(), 2);
    QVERIFY(QFileInfo(outside).isDir());
    QVERIFY(store.exists(QStringLiteral("alice")));
}

void TestPlayerStore::refreshPicksUpExternalPlayers()
{
    PlayerStore store;
    QVERIFY(store.players().isEmpty());

    QVERIFY(QDir().mkpath(PlayerStore::homeDir(QStringLiteral("imported"))));
    QSignalSpy changed(&store, &PlayerStore::playersChanged);
    store.refresh();
    QCOMPARE(store.players(), QStringList{QStringLiteral("imported")});
    QCOMPARE(changed.count(), 1);

    // No change, no signal.
    store.refresh();
    QCOMPARE(changed.count(), 1);

    // Constructing a new store sees existing players immediately.
    PlayerStore second;
    QCOMPARE(second.players(), QStringList{QStringLiteral("imported")});
}

void TestPlayerStore::refreshIgnoresInvalidDirectories()
{
    // Directories with invalid names, or without a home/, are not players.
    QVERIFY(QDir().mkpath(PlayerStore::rootDir() + QStringLiteral("/Upper/home")));
    QVERIFY(QDir().mkpath(PlayerStore::rootDir() + QStringLiteral("/nohome")));
    QVERIFY(QDir().mkpath(PlayerStore::homeDir(QStringLiteral("real"))));

    PlayerStore store;
    QCOMPARE(store.players(), QStringList{QStringLiteral("real")});
    QVERIFY(!store.exists(QStringLiteral("nohome")));
}

QTEST_MAIN(TestPlayerStore)
#include "test_playerstore.moc"
