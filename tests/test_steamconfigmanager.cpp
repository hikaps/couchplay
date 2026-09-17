// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QStandardPaths>
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

class TestSteamConfigManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLoadGamesMergesNativeAndShortcut()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QByteArray oldHome = qgetenv("HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        QStandardPaths::setTestModeEnabled(true);

        const QString steamRoot = home.path() + QStringLiteral("/.steam/steam");
        const QString libraryRoot = home.path() + QStringLiteral("/library");
        const QString configDir = steamRoot + QStringLiteral("/config");
        const QString userConfigDir = steamRoot + QStringLiteral("/userdata/76561198000000000/config");
        QVERIFY(QDir().mkpath(configDir));
        QVERIFY(QDir().mkpath(userConfigDir));
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
        manager.loadGames();
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

        if (oldHome.isNull()) {
            qunsetenv("HOME");
        } else {
            qputenv("HOME", oldHome);
        }
        QStandardPaths::setTestModeEnabled(false);
    }
};

QTEST_MAIN(TestSteamConfigManager)
#include "test_steamconfigmanager.moc"
