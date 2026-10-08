// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "SteamConfigManager.h"

namespace {
void appendString(QByteArray &data, const QByteArray &key, const QByteArray &value)
{
    data.append(char(0x01));
    data.append(key);
    data.append(char('\0'));
    data.append(value);
    data.append(char('\0'));
}
}

QByteArray makeShortcutDocument(const QByteArray &name, quint8 appId)
{
    QByteArray data;
    data.append(char(0x00));
    data.append("shortcuts");
    data.append(char(0));
    data.append(char(0x00));
    data.append("0");
    data.append(char(0));
    appendString(data, "AppName", name);
    data.append(char(0x02));
    data.append("appid");
    data.append(char(0));
    data.append(static_cast<char>(appId));
    data.append(char(0));
    data.append(char(0));
    data.append(char(0));
    data.append(char(0x08));
    data.append(char(0x08));
    return data;
}

class EnvironmentGuard {
public:
    explicit EnvironmentGuard(const char *name) : m_name(name), m_value(qgetenv(name)), m_wasSet(qEnvironmentVariableIsSet(name)) {}
    ~EnvironmentGuard() {
        if (m_wasSet) qputenv(m_name.constData(), m_value);
        else qunsetenv(m_name.constData());
    }
private:
    QByteArray m_name;
    QByteArray m_value;
    bool m_wasSet;
};

class TestSteamConfigManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLoadGamesMergesNativeAndShortcut()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());
        const QString steamRoot = home.path() + QStringLiteral("/.steam/steam");
        const QString libraryRoot = home.path() + QStringLiteral("/library");
        const QString configDir = steamRoot + QStringLiteral("/config");
        const QString userConfigDir = steamRoot + QStringLiteral("/userdata/76561198000000000/config");
        QVERIFY(QDir().mkpath(configDir));
        QVERIFY(QDir().mkpath(userConfigDir));
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/userdata/0/config")));
        QVERIFY(QDir().mkpath(libraryRoot + QStringLiteral("/steamapps/common/TestNative")));

        QFile libraries(configDir + QStringLiteral("/libraryfolders.vdf"));
        QVERIFY(libraries.open(QIODevice::WriteOnly | QIODevice::Text));
        libraries.write("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\"");
        libraries.write(libraryRoot.toUtf8());
        libraries.write("\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"123\"\t\"123\"\n\t\t}\n\t}\n}\n");
        libraries.close();

        QFile manifest(libraryRoot + QStringLiteral("/steamapps/appmanifest_123.acf"));
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Text));
        manifest.write("\"AppState\"\n{\n\t\"appid\"\t\"123\"\n\t\"name\"\t\"Test Native\"\n\t\"installdir\"\t\"TestNative\"\n}\n");
        manifest.close();

        QByteArray shortcutVdf;
        shortcutVdf.append(char(0x00));
        shortcutVdf.append("shortcuts");
        shortcutVdf.append(char('\0'));
        shortcutVdf.append(char(0x00));
        shortcutVdf.append("0");
        shortcutVdf.append(char('\0'));
        appendString(shortcutVdf, "AppName", "Shortcut Game");
        shortcutVdf.append(char(0x02));
        shortcutVdf.append("appid");
        shortcutVdf.append(char('\0'));
        shortcutVdf.append(char(42));
        shortcutVdf.append(char(0));
        shortcutVdf.append(char(0));
        shortcutVdf.append(char(0));
        shortcutVdf.append(char(0x08));
        shortcutVdf.append(char(0x08));

        QFile shortcuts(userConfigDir + QStringLiteral("/shortcuts.vdf"));
        QVERIFY(shortcuts.open(QIODevice::WriteOnly));
        QCOMPARE(shortcuts.write(shortcutVdf), qint64(shortcutVdf.size()));
        shortcuts.close();

        SteamConfigManager manager;
        QVERIFY(manager.isSteamDetected());
        QCOMPARE(manager.getSteamUserId(), QStringLiteral("76561198000000000"));
        QVERIFY(manager.sourceAccountAvailable());
        manager.loadGames();
        QCOMPARE(manager.shortcutCount(), 1);
        const QVariantList games = manager.gamesAsVariant();
        QCOMPARE(games.size(), 2);

        bool foundNative = false;
        bool foundShortcut = false;
        for (const QVariant &entry : games) {
            const QVariantMap game = entry.toMap();
            if (game.value(QStringLiteral("source")).toString() == QStringLiteral("native")) {
                foundNative = true;
                QCOMPARE(game.value(QStringLiteral("gameId")).toString(), QStringLiteral("123"));
                QCOMPARE(game.value(QStringLiteral("title")).toString(), QStringLiteral("Test Native"));
            } else if (game.value(QStringLiteral("source")).toString() == QStringLiteral("shortcut")) {
                foundShortcut = true;
                const quint64 expectedId = (static_cast<quint64>(42) << 32) | 0x02000000ULL;
                QCOMPARE(game.value(QStringLiteral("gameId")).toString(), QString::number(expectedId));
                QCOMPARE(game.value(QStringLiteral("title")).toString(), QStringLiteral("Shortcut Game"));
            }
        }
        QVERIFY(foundNative);
        QVERIFY(foundShortcut);

    }
    void testExplicitSourceSelectionPersistsAndDoesNotFallback()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString rootA = home.path() + QStringLiteral("/.local/share/Steam");
        const QString rootB = home.path() + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam");
        const QString accountA = rootA + QStringLiteral("/userdata/123/config");
        const QString accountB = rootB + QStringLiteral("/userdata/123/config");
        QVERIFY(QDir().mkpath(accountA));
        QVERIFY(QDir().mkpath(accountB));

        QFile shortcutsA(rootA + QStringLiteral("/userdata/123/config/shortcuts.vdf"));
        QVERIFY(shortcutsA.open(QIODevice::WriteOnly));
        const QByteArray dataA = makeShortcutDocument("Account A Game", 31);
        QCOMPARE(shortcutsA.write(dataA), qint64(dataA.size()));
        shortcutsA.close();
        QFile shortcutsB(rootB + QStringLiteral("/userdata/123/config/shortcuts.vdf"));
        QVERIFY(shortcutsB.open(QIODevice::WriteOnly));
        const QByteArray dataB = makeShortcutDocument("Account B Game", 47);
        QCOMPARE(shortcutsB.write(dataB), qint64(dataB.size()));
        shortcutsB.close();

        SteamConfigManager manager;
        QCOMPARE(manager.sourceAccounts().size(), 2);
        QCOMPARE(manager.sourceAccountIndex(), -1);
        QVERIFY(!manager.sourceAccountAvailable());
        QVERIFY(manager.sourceAccountError().contains(QStringLiteral("Select a Steam source account")));
        QVERIFY(manager.selectSourceAccount(QFileInfo(rootB).canonicalFilePath(), QStringLiteral("123")));
        QVERIFY(manager.sourceAccountAvailable());
        const QVariantList selectedGames = manager.gamesAsVariant();
        QCOMPARE(selectedGames.size(), 1);
        QCOMPARE(selectedGames.constFirst().toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Account B Game"));
        QCOMPARE(manager.steamPaths().steamRoot, QFileInfo(rootB).canonicalFilePath());

        {
            SteamConfigManager reloaded;
            QCOMPARE(reloaded.sourceAccountIndex(), 1);
            QVERIFY(reloaded.sourceAccountAvailable());
            QCOMPARE(reloaded.steamPaths().steamRoot, QFileInfo(rootB).canonicalFilePath());
        }

        QVERIFY(QDir(rootB + QStringLiteral("/userdata/123")).removeRecursively());
        manager.refreshSourceAccounts();
        QCOMPARE(manager.sourceAccountIndex(), -1);
        QVERIFY(!manager.sourceAccountAvailable());
        QCOMPARE(manager.gamesAsVariant().size(), 0);
        QCOMPARE(manager.steamPaths().steamRoot, QFileInfo(rootB).canonicalFilePath());
        QVERIFY(manager.sourceAccountError().contains(QStringLiteral("Previously selected")));

        QVERIFY(QDir().mkpath(accountB));
        manager.refreshSourceAccounts();
        QVERIFY(manager.sourceAccountAvailable());
        QCOMPARE(manager.sourceAccountIndex(), 1);
    }
    void testSavedSourceDoesNotFollowRetargetedSteamRoot()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString rootA = home.path() + QStringLiteral("/.local/share/Steam");
        const QString rootB = home.path() + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam");
        const QString accountA = rootA + QStringLiteral("/userdata/123/config");
        const QString accountB = rootB + QStringLiteral("/userdata/123/config");
        QVERIFY(QDir().mkpath(accountA));
        QVERIFY(QDir().mkpath(accountB));
        const QString rootAOriginal = rootA + QStringLiteral("-original");

        SteamConfigManager manager;
        QCOMPARE(manager.sourceAccounts().size(), 2);
        QVERIFY(manager.selectSourceAccount(QFileInfo(rootA).canonicalFilePath(), QStringLiteral("123")));
        QCOMPARE(manager.steamPaths().steamRoot, QFileInfo(rootA).canonicalFilePath());

        QVERIFY(QDir().rename(rootA, rootAOriginal));
        QVERIFY(QFile::link(rootB, rootA));
        QVERIFY(!manager.selectSourceAccount(rootA, QStringLiteral("123")));
        QCOMPARE(manager.sourceAccounts().size(), 1);
        QCOMPARE(manager.sourceAccountIndex(), -1);
        QVERIFY(!manager.isSteamDetected());
        QVERIFY(!manager.sourceAccountAvailable());
        QVERIFY(manager.sourceAccountError().contains(QStringLiteral("Previously selected")));

        QVERIFY(QFile::remove(rootA));
        QVERIFY(QDir().rename(rootAOriginal, rootA));
        manager.refreshSourceAccounts();
        QVERIFY(manager.sourceAccountAvailable());
        QCOMPARE(manager.steamPaths().steamRoot, QFileInfo(rootA).canonicalFilePath());
    }
};
QTEST_MAIN(TestSteamConfigManager)
#include "test_steamconfigmanager.moc"
