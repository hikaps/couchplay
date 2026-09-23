// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"

namespace {
void appendString(QByteArray &data, const QByteArray &key, const QByteArray &value)
{
    data.append(char(0x01));
    data.append(key);
    data.append(char('\0'));
    data.append(value);
    data.append(char('\0'));
}

void appendInt32(QByteArray &data, const QByteArray &key, quint32 value)
{
    data.append(char(0x02));
    data.append(key);
    data.append(char('\0'));
    data.append(char(value & 0xff));
    data.append(char((value >> 8) & 0xff));
    data.append(char((value >> 16) & 0xff));
    data.append(char((value >> 24) & 0xff));
}

QByteArray foreignEntry()
{
    QByteArray entry;
    entry.append(char(0x00));
    entry.append("0");
    entry.append(char('\0'));
    appendInt32(entry, "appid", 123);
    appendString(entry, "AppName", "Foreign Game");
    appendString(entry, "exe", "/usr/bin/foreign");
    appendString(entry, "icon", "/tmp/foreign.png");
    entry.append(char(0x00));
    entry.append("tags");
    entry.append(char('\0'));
    appendString(entry, "0", "Foreign");
    entry.append(char(0x08));
    entry.append(char(0x08));
    return entry;
}

QByteArray documentWithForeignEntry()
{
    QByteArray document = SteamShortcutsVdf::emptyDocument();
    document.insert(document.size() - 2, foreignEntry());
    return document;
}

}

class TestSteamConfigManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCodecUpsertPreservesForeignBytes()
    {
        SteamShortcut shortcut;
        shortcut.appId = 0x80000042u;
        shortcut.appName = QStringLiteral("CouchPlay - Family Night");
        shortcut.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        shortcut.startDir = QStringLiteral("/tmp");
        shortcut.icon = QStringLiteral("/tmp/couchplay.png");
        shortcut.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('a'));
        shortcut.tags = {QStringLiteral("CouchPlay")};

        QString error;
        QByteArray combined;
        QVERIFY2(SteamShortcutsVdf::upsert(documentWithForeignEntry(), shortcut, &combined, &error), qPrintable(error));

        QList<SteamShortcut> decoded;
        QVERIFY2(SteamShortcutsVdf::decode(combined, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.size(), 2);

        SteamShortcut changed = shortcut;
        changed.appName = QStringLiteral("CouchPlay - Changed");
        changed.launchOptions = QStringLiteral("--profile=Family");
        QByteArray updated;
        QVERIFY2(SteamShortcutsVdf::upsert(combined, changed, &updated, &error), qPrintable(error));
        QVERIFY(updated != combined);
        QVERIFY(updated.contains("Foreign Game"));
        QVERIFY(updated.contains("/tmp/foreign.png"));
        QVERIFY(updated.contains("CouchPlay - Changed"));

        QByteArray repeated;
        QVERIFY2(SteamShortcutsVdf::upsert(updated, changed, &repeated, &error), qPrintable(error));
        QCOMPARE(repeated, updated);

        QByteArray stripped;
        QVERIFY2(SteamShortcutsVdf::withoutProfiles(updated, &stripped, &error), qPrintable(error));
        QCOMPARE(stripped, documentWithForeignEntry());
    }

    void testCodecRejectsMalformedInput()
    {
        QList<SteamShortcut> shortcuts;
        QString error;
        QByteArray malformed = SteamShortcutsVdf::emptyDocument();
        malformed.chop(2);
        QVERIFY(!SteamShortcutsVdf::decode(malformed, &shortcuts, &error));
        QVERIFY(!error.isEmpty());

        QByteArray unsupported = SteamShortcutsVdf::emptyDocument();
        unsupported.insert(unsupported.size() - 2, char(0x09));
        unsupported.insert(unsupported.size() - 1, "bad", 3);
        QVERIFY(!SteamShortcutsVdf::decode(unsupported, &shortcuts, &error));
    }
    void testCodecRejectsNulTagsAndNonExactMarkers()
    {
        SteamShortcut shortcut;
        shortcut.appName = QStringLiteral("CouchPlay - Test");
        shortcut.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        shortcut.startDir = QStringLiteral("/tmp");
        shortcut.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('b'));
        shortcut.tags = {QStringLiteral("bad") + QChar(0) + QStringLiteral("tag")};
        QByteArray result;
        QString error;
        QVERIFY(!SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), shortcut, &result, &error));
        QVERIFY(!error.isEmpty());

        shortcut.tags = {QStringLiteral("CouchPlay")};
        shortcut.shortcutPath += QLatin1Char('\n');
        QVERIFY(!SteamShortcutsVdf::isProfileShortcut(shortcut));
    }

    void testWithoutProfilesSupportsAliasedInput()
    {
        SteamShortcut shortcut;
        shortcut.appName = QStringLiteral("CouchPlay - Test");
        shortcut.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        shortcut.startDir = QStringLiteral("/tmp");
        shortcut.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('c'));
        QByteArray document;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(documentWithForeignEntry(), shortcut, &document, &error), qPrintable(error));
        const QByteArray expected = documentWithForeignEntry();
        QVERIFY2(SteamShortcutsVdf::withoutProfiles(document, &document, &error), qPrintable(error));
        QCOMPARE(document, expected);
    }
    void testSyncPreservesTargetProfiles()
    {
        SteamShortcut sourceProfile;
        sourceProfile.appName = QStringLiteral("Source profile");
        sourceProfile.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        sourceProfile.startDir = QStringLiteral("/tmp");
        sourceProfile.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('e'));
        QByteArray source;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(documentWithForeignEntry(), sourceProfile, &source, &error), qPrintable(error));

        SteamShortcut targetProfile;
        targetProfile.appName = QStringLiteral("Player-owned profile");
        targetProfile.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        targetProfile.startDir = QStringLiteral("/tmp");
        targetProfile.icon = QStringLiteral("/tmp/player-artwork.png");
        targetProfile.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('f'));
        targetProfile.tags = {QStringLiteral("Player tag")};
        QByteArray target;
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), targetProfile, &target, &error), qPrintable(error));

        QByteArray merged;
        QVERIFY2(SteamShortcutsVdf::mergePreservingProfiles(source, target, &merged, &error), qPrintable(error));
        QList<SteamShortcut> shortcuts;
        QVERIFY2(SteamShortcutsVdf::decode(merged, &shortcuts, &error), qPrintable(error));
        QCOMPARE(shortcuts.size(), 2);
        QVERIFY(merged.contains("Foreign Game"));
        bool retainedTargetProfile = false;
        for (const SteamShortcut &shortcut : shortcuts) {
            QVERIFY(shortcut.shortcutPath != sourceProfile.shortcutPath);
            if (shortcut.shortcutPath == targetProfile.shortcutPath) {
                retainedTargetProfile = true;
                QCOMPARE(shortcut.appName, QStringLiteral("Player-owned profile"));
                QCOMPARE(shortcut.icon, QStringLiteral("/tmp/player-artwork.png"));
                QCOMPARE(shortcut.tags, QStringList{QStringLiteral("Player tag")});
            }
        }
        QVERIFY(retainedTargetProfile);
    }

    void testProfileUpdatePreservesSteamCustomization()
    {
        SteamShortcut shortcut;
        shortcut.appName = QStringLiteral("CouchPlay - Test");
        shortcut.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        shortcut.startDir = QStringLiteral("/tmp");
        shortcut.icon = QStringLiteral("/tmp/user-artwork.png");
        shortcut.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('d'));
        shortcut.tags = {QStringLiteral("Favorites")};
        QByteArray initial;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), shortcut, &initial, &error), qPrintable(error));
        shortcut.appName = QStringLiteral("Updated profile name");
        shortcut.icon = QStringLiteral("/tmp/generated.png");
        shortcut.tags = {QStringLiteral("CouchPlay")};
        // Steam-side user metadata is deliberately retained by profile re-upsert.
        // The new values describe the application, while Steam-customized artwork/tags remain user-owned.
        QByteArray updated;
        QVERIFY2(SteamShortcutsVdf::upsert(initial, shortcut, &updated, &error), qPrintable(error));
        QList<SteamShortcut> loaded;
        QVERIFY2(SteamShortcutsVdf::decode(updated, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.constLast().appName, QStringLiteral("Updated profile name"));
        QCOMPARE(loaded.constLast().icon, QStringLiteral("/tmp/user-artwork.png"));
        QCOMPARE(loaded.constLast().tags, QStringList{QStringLiteral("Favorites")});
    }
    void testUpsertRejectsOversizedOutputs()
    {
        SteamShortcut shortcut;
        shortcut.appName = QStringLiteral("CouchPlay - Size Boundary");
        shortcut.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        shortcut.startDir = QStringLiteral("/tmp");
        shortcut.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('9'));

        QString error;
        const QByteArray foreignName("Foreign Game");
        QByteArray insertionSource = documentWithForeignEntry();
        const qsizetype foreignNameSize = SteamShortcutsVdf::MaxDocumentSize - insertionSource.size()
            + foreignName.size();
        insertionSource.replace(foreignName, QByteArray(foreignNameSize, 'x'));
        QCOMPARE(insertionSource.size(), SteamShortcutsVdf::MaxDocumentSize);
        QList<SteamShortcut> decoded;
        QVERIFY2(SteamShortcutsVdf::decode(insertionSource, &decoded, &error), qPrintable(error));

        QByteArray result("unchanged");
        QVERIFY(!SteamShortcutsVdf::upsert(insertionSource, shortcut, &result, &error));
        QCOMPARE(result, QByteArray("unchanged"));
        QVERIFY(!error.isEmpty());

        QByteArray replacementSource;
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), shortcut, &replacementSource, &error),
                 qPrintable(error));
        const QByteArray originalName = shortcut.appName.toUtf8();
        const QByteArray prefix("CouchPlay - ");
        const qsizetype longNameSize = SteamShortcutsVdf::MaxDocumentSize - replacementSource.size()
            + originalName.size() - 1;
        const QByteArray longName = prefix + QByteArray(longNameSize - prefix.size(), 'y');
        replacementSource.replace(originalName, longName);
        QCOMPARE(replacementSource.size(), SteamShortcutsVdf::MaxDocumentSize - 1);
        QVERIFY2(SteamShortcutsVdf::decode(replacementSource, &decoded, &error), qPrintable(error));

        shortcut.appName = QString::fromUtf8(longName + QByteArray("xx"));
        result = QByteArray("unchanged");
        QVERIFY(!SteamShortcutsVdf::upsert(replacementSource, shortcut, &result, &error));
        QCOMPARE(result, QByteArray("unchanged"));
        QVERIFY(!error.isEmpty());
    }

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
