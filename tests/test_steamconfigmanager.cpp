// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>
#include <qqmlintegration.h>

#define private public
#include "../src/dbus/CouchPlayHelperClient.h"
#undef private

#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"
#include "PresetManager.h"

#include <atomic>
#include <functional>
#include <thread>
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
QByteArray documentWithEntry(const QByteArray &entry)
{
    QByteArray document = SteamShortcutsVdf::emptyDocument();
    document.insert(document.size() - 2, entry);
    return document;
}

}

class MockShortcutSyncHelper final : public CouchPlayHelperClient
{
public:
    MockShortcutSyncHelper() { m_available = false; }
    bool isAvailable() const override { return true; }
    QVariantMap getUserInfo(const QString &) override
    {
        QVariantMap result;
        result.insert(QStringLiteral("uid"), 1001U);
        result.insert(QStringLiteral("gid"), 1001U);
        result.insert(QStringLiteral("home"), userHome);
        return result;
    }
    QString getUserSteamRoot(const QString &) override { return steamRoot; }
    bool readSteamLibraryFoldersForUser(const QString &, QByteArray *content, bool *exists) override
    {
        QFile file(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
        *exists = file.exists();
        if (!*exists) { content->clear(); return true; }
        if (!file.open(QIODevice::ReadOnly)) return false;
        *content = file.readAll();
        return true;
    }
    bool restoreSteamLibraryFoldersForUser(const QString &, bool existed, const QByteArray &content) override
    {
        const QString path = steamRoot + QStringLiteral("/config/libraryfolders.vdf");
        if (!existed) return !QFile::exists(path) || QFile::remove(path);
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
    }
    bool writeFileToUser(const QByteArray &content, const QString &path, const QString &) override
    {
        writePaths.append(path);
        if (onWrite) {
            onWrite(path);
        }
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
    }
    QString getUserSteamId(const QString &) override
    {
        return steamIdLookups++ == 0 ? steamId : alternateSteamId;
    }
    bool isSteamBootstrapped(const QString &) override { return true; }

    QString shortcutsPath(const QString &accountId) const
    {
        return steamRoot + QStringLiteral("/userdata/") + accountId + QStringLiteral("/config/shortcuts.vdf");
    }

    bool readSteamShortcutsForUser(const QString &,
                                   const QString &selectedSteamId,
                                   QByteArray *content,
                                   std::function<bool()> shouldContinue,
                                   QString *errorMessage) override
    {
        if (onRead) {
            onRead();
        }
        if (readFails) {
            if (errorMessage) {
                *errorMessage = readError;
            }
            return false;
        }
        if (shouldContinue && !shouldContinue()) {
            return false;
        }
        QFile target(shortcutsPath(selectedSteamId));
        if (!target.exists()) {
            content->clear();
            return true;
        }
        if (!target.open(QIODevice::ReadOnly)) {
            if (errorMessage) {
                *errorMessage = target.errorString();
            }
            return false;
        }
        *content = target.readAll();
        return true;
    }

    bool writeSteamShortcutsForUser(const QString &,
                                    const QString &selectedSteamId,
                                    const QByteArray &expectedDigest,
                                    const QByteArray &content,
                                    QString *errorMessage) override
    {
        const QString path = shortcutsPath(selectedSteamId);
        QByteArray currentDigest = QByteArrayLiteral("missing");
        QFile current(path);
        if (current.exists()) {
            if (!current.open(QIODevice::ReadOnly)) {
                if (errorMessage) {
                    *errorMessage = current.errorString();
                }
                return false;
            }
            currentDigest = QCryptographicHash::hash(current.readAll(), QCryptographicHash::Sha256).toHex();
            current.close();
        }
        if (currentDigest != expectedDigest) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Steam shortcuts file changed during sync");
            }
            return false;
        }
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Could not create test Steam account directory");
            }
            return false;
        }
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) || output.write(content) != content.size()) {
            if (errorMessage) {
                *errorMessage = output.errorString();
            }
            return false;
        }
        return true;
    }

    QString userHome;
    QString steamRoot;
    QString steamId = QStringLiteral("76561198000000002");
    QString alternateSteamId = QStringLiteral("76561198000000003");
    int steamIdLookups = 0;
    QString readError = QStringLiteral("permission denied by helper");
    bool readFails = false;
    std::function<void()> onRead;
    QStringList writePaths;
    std::function<void(const QString &)> onWrite;
};

class TestSteamConfigManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFinalizeRestoresTargetLibraryFolders()
    {
        QTemporaryDir hostHome;
        QTemporaryDir targetHome;
        QTemporaryDir library;
        QVERIFY(hostHome.isValid());
        QVERIFY(targetHome.isValid());
        QVERIFY(library.isValid());
        const QByteArray previousHome = qgetenv("HOME");
        qputenv("HOME", hostHome.path().toLocal8Bit());
        QStandardPaths::setTestModeEnabled(true);

        const QString ownRoot = hostHome.path() + QStringLiteral("/.local/share/Steam");
        const QString configDir = ownRoot + QStringLiteral("/config");
        QVERIFY(QDir().mkpath(configDir));
        const QString sourceVdf = configDir + QStringLiteral("/libraryfolders.vdf");
        QFile source(sourceVdf);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray sourceBytes = QByteArrayLiteral("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\"")
            + library.path().toUtf8() + QByteArrayLiteral("\"\n\t}\n}\n");
        QCOMPARE(source.write(sourceBytes), qint64(sourceBytes.size()));
        source.close();

        SteamConfigManager manager;
        manager.detectSteamPaths();
        manager.loadLibraryFolders();
        QCOMPARE(manager.libraryCount(), 1);

        MockShortcutSyncHelper helper;
        helper.userHome = targetHome.path();
        helper.steamRoot = targetHome.path() + QStringLiteral("/.local/share/Steam");
        const QString targetConfig = helper.steamRoot + QStringLiteral("/config");
        QVERIFY(QDir().mkpath(targetConfig));
        const QByteArray original = QByteArrayLiteral("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\"/target/steam\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\"/target/secondary-library\"\n\t}\n}\n");
        QFile target(targetConfig + QStringLiteral("/libraryfolders.vdf"));
        QVERIFY(target.open(QIODevice::WriteOnly));
        QCOMPARE(target.write(original), qint64(original.size()));
        target.close();
        manager.setHelperClient(&helper);

        DataDirectory directory;
        directory.path = ownRoot;
        directory.mode = QStringLiteral("overlay");
        QVERIFY(manager.finalizeDataDir(directory, QStringLiteral("player1")));
        QVERIFY(target.open(QIODevice::ReadOnly));
        const QByteArray duringSession = target.readAll();
        target.close();
        QVERIFY(duringSession.contains(QByteArrayLiteral(".couchplay/steam-libs/0")));
        QVERIFY(!duringSession.contains(QByteArrayLiteral("/target/secondary-library")));

        QVERIFY(manager.cleanupLibrarySharing(QStringLiteral("player1")));
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), original);
        target.close();
        QVERIFY(original.contains(QByteArrayLiteral("/target/secondary-library")));

        if (previousHome.isNull()) qunsetenv("HOME");
        else qputenv("HOME", previousHome);
        QStandardPaths::setTestModeEnabled(false);
    }
    void testFinalizeCancellationRestoresSnapshotBeforeLibraryFoldersWrite()
    {
        QTemporaryDir hostHome;
        QTemporaryDir targetHome;
        QTemporaryDir library;
        QVERIFY(hostHome.isValid());
        QVERIFY(targetHome.isValid());
        QVERIFY(library.isValid());

        const QByteArray previousHome = qgetenv("HOME");
        qputenv("HOME", hostHome.path().toLocal8Bit());
        QStandardPaths::setTestModeEnabled(true);

        const QString ownRoot = hostHome.path() + QStringLiteral("/.local/share/Steam");
        const QString configDir = ownRoot + QStringLiteral("/config");
        QVERIFY(QDir().mkpath(configDir));
        QFile source(configDir + QStringLiteral("/libraryfolders.vdf"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray sourceBytes = QByteArrayLiteral("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\"")
            + library.path().toUtf8() + QByteArrayLiteral("\"\n\t}\n}\n");
        QCOMPARE(source.write(sourceBytes), qint64(sourceBytes.size()));
        source.close();

        const QString manifestPath = library.path() + QStringLiteral("/steamapps/appmanifest_123.acf");
        QVERIFY(QDir().mkpath(QFileInfo(manifestPath).absolutePath()));
        QFile manifest(manifestPath);
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray manifestBytes = QByteArrayLiteral("\"AppState\"\n{\n\t\"appid\"\t\"123\"\n}\n");
        QCOMPARE(manifest.write(manifestBytes), qint64(manifestBytes.size()));
        manifest.close();

        SteamConfigManager manager;
        manager.detectSteamPaths();
        manager.loadLibraryFolders();
        QCOMPARE(manager.libraryCount(), 1);

        MockShortcutSyncHelper helper;
        helper.userHome = targetHome.path();
        helper.steamRoot = targetHome.path() + QStringLiteral("/.local/share/Steam");
        const QString targetConfig = helper.steamRoot + QStringLiteral("/config");
        QVERIFY(QDir().mkpath(targetConfig));
        const QByteArray original = QByteArrayLiteral("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\"/target/steam\"\n\t}\n}\n");
        QFile target(targetConfig + QStringLiteral("/libraryfolders.vdf"));
        QVERIFY(target.open(QIODevice::WriteOnly));
        QCOMPARE(target.write(original), qint64(original.size()));
        target.close();
        manager.setHelperClient(&helper);

        bool cancellationRequested = false;
        helper.onWrite = [&](const QString &path) {
            if (path.endsWith(QStringLiteral("/appmanifest_123.acf"))) {
                cancellationRequested = true;
            }
        };

        DataDirectory directory;
        directory.path = ownRoot;
        directory.mode = QStringLiteral("overlay");
        QVERIFY(!manager.finalizeDataDir(directory, QStringLiteral("player1"), [&]() {
            return !cancellationRequested;
        }));

        const QString targetManifestPath = targetHome.path()
            + QStringLiteral("/.couchplay/steam-libs/0/steamapps/appmanifest_123.acf");
        QCOMPARE(helper.writePaths, QStringList{targetManifestPath});
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), original);
        target.close();

        if (previousHome.isNull()) qunsetenv("HOME");
        else qputenv("HOME", previousHome);
        QStandardPaths::setTestModeEnabled(false);
    }
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

    void testCodecRejectsShortcutIndexWithFinalNewline()
    {
        QByteArray entry;
        entry.append(char(0x00));
        entry.append("0\n");
        entry.append(char('\0'));
        entry.append(char(0x08));

        QList<SteamShortcut> shortcuts;
        QString error;
        QVERIFY(!SteamShortcutsVdf::decode(documentWithEntry(entry), &shortcuts, &error));
        QVERIFY(!error.isEmpty());
    }

    void testCodecBoundsParserNodeExpansion()
    {
        constexpr int entryCount = 50'000;
        QByteArray document = SteamShortcutsVdf::emptyDocument();
        document.chop(2);
        document.reserve(entryCount * 9 + document.size() + 2);
        for (int i = 0; i < entryCount; ++i) {
            document.append(char(0x00));
            document.append(QByteArray::number(i));
            document.append(char('\0'));
            document.append(char(0x08));
        }
        document.append(char(0x08));
        document.append(char(0x08));

        QList<SteamShortcut> shortcuts;
        QString error;
        QVERIFY(document.size() < SteamShortcutsVdf::MaxDocumentSize);
        QVERIFY(!SteamShortcutsVdf::decode(document, &shortcuts, &error));
        QVERIFY(!error.isEmpty());
    }

    void testMergeReassignsProfileAppIdCollidingWithForeignShortcut()
    {
        SteamShortcut profile;
        profile.appId = 123;
        profile.appName = QStringLiteral("Player-owned profile");
        profile.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        profile.startDir = QStringLiteral("/tmp");
        profile.icon = QStringLiteral("/tmp/player-artwork.png");
        profile.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('a'));
        profile.launchOptions = QStringLiteral("--profile=Original");
        profile.tags = {QStringLiteral("Player tag")};

        QByteArray target;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), profile, &target, &error),
                 qPrintable(error));
        const QByteArray customField = QByteArray::fromHex("01637573746f6d4d65746164617461006b6565702d6d6500");
        target.insert(target.size() - 3, customField);

        QByteArray merged;
        QVERIFY2(SteamShortcutsVdf::mergePreservingProfiles(documentWithForeignEntry(), target, &merged, &error),
                 qPrintable(error));
        QVERIFY(merged.contains(foreignEntry()));
        QList<SteamShortcut> shortcuts;
        QVERIFY2(SteamShortcutsVdf::decode(merged, &shortcuts, &error), qPrintable(error));
        QCOMPARE(shortcuts.size(), 2);
        QCOMPARE(shortcuts.at(0).appId, quint32(123));
        QCOMPARE(shortcuts.at(0).appName, QStringLiteral("Foreign Game"));
        QCOMPARE(shortcuts.at(1).shortcutPath, profile.shortcutPath);
        QVERIFY(shortcuts.at(1).appId != 0);
        QVERIFY(shortcuts.at(1).appId != shortcuts.at(0).appId);
        QCOMPARE(shortcuts.at(1).appName, profile.appName);
        QCOMPARE(shortcuts.at(1).icon, profile.icon);
        QCOMPARE(shortcuts.at(1).tags, profile.tags);
        QCOMPARE(shortcuts.at(1).launchOptions, profile.launchOptions);
        QVERIFY(merged.contains(customField));
    }

    void testUpsertReassignsManagedAppIdCollision()
    {
        SteamShortcut profile;
        profile.appId = 123;
        profile.appName = QStringLiteral("Player-owned profile");
        profile.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        profile.startDir = QStringLiteral("/tmp");
        profile.icon = QStringLiteral("/tmp/user-artwork.png");
        profile.launchOptions = QStringLiteral("--profile=Original");
        profile.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('c'));
        profile.tags = {QStringLiteral("Player tag")};

        QByteArray document;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), profile, &document, &error),
                 qPrintable(error));
        const QByteArray customField = QByteArray::fromHex("01637573746f6d4d65746164617461006b6565702d6d6500");
        document.insert(document.size() - 3, customField);
        QByteArray foreign = foreignEntry();
        foreign[1] = '1';
        document.insert(document.size() - 2, foreign);

        QList<SteamShortcut> before;
        QVERIFY2(SteamShortcutsVdf::decode(document, &before, &error), qPrintable(error));
        QCOMPARE(before.size(), 2);
        QCOMPARE(before.at(0).appId, quint32(123));
        QCOMPARE(before.at(1).appId, quint32(123));

        profile.appName = QStringLiteral("Refreshed profile");
        profile.icon = QStringLiteral("/tmp/generated.png");
        profile.launchOptions = QStringLiteral("--profile=Updated");
        profile.tags = {QStringLiteral("CouchPlay")};
        QByteArray updated;
        QVERIFY2(SteamShortcutsVdf::upsert(document, profile, &updated, &error), qPrintable(error));

        QList<SteamShortcut> after;
        QVERIFY2(SteamShortcutsVdf::decode(updated, &after, &error), qPrintable(error));
        QCOMPARE(after.size(), 2);
        QVERIFY(after.at(0).appId != 0);
        QVERIFY(after.at(0).appId != after.at(1).appId);
        QCOMPARE(after.at(0).appName, QStringLiteral("Refreshed profile"));
        QCOMPARE(after.at(0).launchOptions, QStringLiteral("--profile=Updated"));
        QCOMPARE(after.at(0).icon, QStringLiteral("/tmp/user-artwork.png"));
        QCOMPARE(after.at(0).tags, QStringList{QStringLiteral("Player tag")});
        QCOMPARE(after.at(1).appId, quint32(123));
        QCOMPARE(after.at(1).appName, QStringLiteral("Foreign Game"));
        QVERIFY(updated.contains(customField));
        QVERIFY(updated.contains(foreign));
    }

    void testUpsertRejectsManagedProfilesWithInvalidAppId()
    {
        SteamShortcut profile;
        profile.appId = 77;
        profile.appName = QStringLiteral("CouchPlay - Corrupt AppId");
        profile.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        profile.startDir = QStringLiteral("/tmp");
        profile.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('b'));

        QByteArray valid;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), profile, &valid, &error),
                 qPrintable(error));

        const QByteArray appIdField = QByteArray::fromHex("02617070696400");
        const qsizetype appIdPosition = valid.indexOf(appIdField);
        QVERIFY(appIdPosition >= 0);
        QByteArray missing = valid;
        missing.remove(appIdPosition, appIdField.size() + 4);

        QByteArray wrongType = valid;
        QByteArray wrongTypeField;
        appendString(wrongTypeField, "appid", "77");
        wrongType.replace(appIdPosition, appIdField.size() + 4, wrongTypeField);

        QByteArray zero = valid;
        zero.replace(appIdPosition + appIdField.size(), 4, QByteArray(4, char(0)));

        profile.appName = QStringLiteral("Updated profile");
        const QList<QByteArray> corruptRecords{missing, wrongType, zero};
        for (const QByteArray &corrupt : corruptRecords) {
            QByteArray result("unchanged");
            QVERIFY2(!SteamShortcutsVdf::upsert(corrupt, profile, &result, &error), qPrintable(error));
            QCOMPARE(result, QByteArray("unchanged"));
        }
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
        QList<SteamShortcut> initialShortcuts;
        QVERIFY2(SteamShortcutsVdf::decode(initial, &initialShortcuts, &error), qPrintable(error));
        const quint32 initialAppId = initialShortcuts.constLast().appId;
        shortcut.appName = QStringLiteral("Updated profile name");
        shortcut.icon = QStringLiteral("/tmp/generated.png");
        shortcut.tags = {QStringLiteral("CouchPlay")};
        // Steam-side user metadata is deliberately retained by profile re-upsert.
        // The new values describe the application, while Steam-customized artwork/tags remain user-owned.
        QByteArray updated;
        QVERIFY2(SteamShortcutsVdf::upsert(initial, shortcut, &updated, &error), qPrintable(error));
        QList<SteamShortcut> loaded;
        QVERIFY2(SteamShortcutsVdf::decode(updated, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.constLast().appId, initialAppId);
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
    void testShortcutSyncPropagatesReadErrorsAndBindsOneSteamAccount()
    {
        QTemporaryDir sourceHome;
        QTemporaryDir targetHome;
        QTemporaryDir configHome;
        QVERIFY(sourceHome.isValid());
        QVERIFY(targetHome.isValid());
        QVERIFY(configHome.isValid());

        struct EnvironmentRestore {
            bool hadHome;
            QByteArray home;
            bool hadConfig;
            QByteArray config;
            ~EnvironmentRestore()
            {
                if (hadHome) {
                    qputenv("HOME", home);
                } else {
                    qunsetenv("HOME");
                }
                if (hadConfig) {
                    qputenv("XDG_CONFIG_HOME", config);
                } else {
                    qunsetenv("XDG_CONFIG_HOME");
                }
            }
        } restore{qEnvironmentVariableIsSet("HOME"), qgetenv("HOME"),
                  qEnvironmentVariableIsSet("XDG_CONFIG_HOME"), qgetenv("XDG_CONFIG_HOME")};
        qputenv("HOME", sourceHome.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", configHome.path().toLocal8Bit());

        const QString sourceRoot = sourceHome.path() + QStringLiteral("/.local/share/Steam");
        const QString sourceConfig = sourceRoot + QStringLiteral("/config");
        const QString sourceSteamId = QStringLiteral("76561198000000002");
        const QString sourceUserConfig = sourceRoot + QStringLiteral("/userdata/") + sourceSteamId
            + QStringLiteral("/config");
        QVERIFY(QDir().mkpath(sourceConfig));
        QVERIFY(QDir().mkpath(sourceUserConfig));
        QFile sourceLibraryFolders(sourceConfig + QStringLiteral("/libraryfolders.vdf"));
        QVERIFY(sourceLibraryFolders.open(QIODevice::WriteOnly));
        const QByteArray sourceLibraryData = QByteArrayLiteral("\"libraryfolders\"\n{\n}\n");
        QCOMPARE(sourceLibraryFolders.write(sourceLibraryData), sourceLibraryData.size());
        sourceLibraryFolders.close();
        QFile sourceShortcuts(sourceUserConfig + QStringLiteral("/shortcuts.vdf"));
        const QByteArray sourceData = documentWithForeignEntry();
        QVERIFY(sourceShortcuts.open(QIODevice::WriteOnly));
        QCOMPARE(sourceShortcuts.write(sourceData), sourceData.size());
        sourceShortcuts.close();

        MockShortcutSyncHelper helper;
        helper.userHome = targetHome.path();
        helper.steamRoot = targetHome.path() + QStringLiteral("/.local/share/Steam");
        const QString selectedPath = helper.shortcutsPath(helper.steamId);
        const QString changedAccountPath = helper.shortcutsPath(helper.alternateSteamId);
        QVERIFY(QDir().mkpath(QFileInfo(selectedPath).absolutePath()));
        QVERIFY(QDir().mkpath(QFileInfo(changedAccountPath).absolutePath()));

        SteamShortcut alternateProfile;
        alternateProfile.appName = QStringLiteral("Other account profile");
        alternateProfile.exe = QStringLiteral("/tmp/couchplay-profile.sh");
        alternateProfile.startDir = QStringLiteral("/tmp");
        alternateProfile.shortcutPath = QStringLiteral("couchplay://profile/") + QString(64, QLatin1Char('f'));
        alternateProfile.tags = {QStringLiteral("CouchPlay")};
        QByteArray alternateAccountData;
        QString vdfError;
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), alternateProfile,
                                           &alternateAccountData, &vdfError), qPrintable(vdfError));
        QFile alternateAccountFile(changedAccountPath);
        QVERIFY(alternateAccountFile.open(QIODevice::WriteOnly));
        QCOMPARE(alternateAccountFile.write(alternateAccountData), alternateAccountData.size());
        alternateAccountFile.close();

        SteamConfigManager manager;
        manager.setHelperClient(&helper);
        QSignalSpy failed(&manager, &SteamConfigManager::syncFailed);
        QSignalSpy completed(&manager, &SteamConfigManager::syncCompleted);
        bool shouldContinue = true;

        helper.steamIdLookups = 0;
        helper.readFails = true;
        QVERIFY(!manager.syncShortcutsToUser(QStringLiteral("player1"), [&] { return shouldContinue; }));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.constFirst().at(1).toString(),
                 QStringLiteral("Failed to read target shortcuts.vdf: permission denied by helper"));
        QVERIFY(!QFile::exists(selectedPath));

        failed.clear();
        helper.steamIdLookups = 0;
        helper.onRead = [&] { shouldContinue = false; };
        QVERIFY(!manager.syncShortcutsToUser(QStringLiteral("player1"), [&] { return shouldContinue; }));
        QVERIFY(failed.isEmpty());
        QVERIFY(!QFile::exists(selectedPath));

        helper.readFails = false;
        helper.onRead = {};
        shouldContinue = true;
        helper.steamIdLookups = 0;
        QVERIFY(manager.syncShortcutsToUser(QStringLiteral("player1"), [&] { return shouldContinue; }));

        QFile selectedAccountFile(selectedPath);
        QVERIFY(selectedAccountFile.open(QIODevice::ReadOnly));
        QList<SteamShortcut> selectedShortcuts;
        QString decodeError;
        QVERIFY2(SteamShortcutsVdf::decode(selectedAccountFile.readAll(), &selectedShortcuts, &decodeError),
                 qPrintable(decodeError));
        QCOMPARE(selectedShortcuts.size(), 1);
        QCOMPARE(selectedShortcuts.constFirst().appName, QStringLiteral("Foreign Game"));
        QVERIFY(alternateAccountFile.open(QIODevice::ReadOnly));
        QCOMPARE(alternateAccountFile.readAll(), alternateAccountData);
        QCOMPARE(completed.size(), 1);
    }
};

QTEST_MAIN(TestSteamConfigManager)
#include "test_steamconfigmanager.moc"
