// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QStringList>
#include <QVariantList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include "PlayerStore.h"
#include "PresetManager.h"
#include "SandboxCommand.h"
#include "SteamCompatToolMapping.h"
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

QByteArray makeShortcutDocument(const QByteArray &name, quint8 appId, const QByteArray &exe = {},
                                const QByteArray &startDir = {}, const QByteArray &icon = {})
{
    QByteArray data;
    data.append(char(0x00));
    data.append("shortcuts");
    data.append(char(0));
    data.append(char(0x00));
    data.append("0");
    data.append(char(0));
    appendString(data, "AppName", name);
    if (!exe.isEmpty()) appendString(data, "exe", exe);
    if (!startDir.isEmpty()) appendString(data, "StartDir", startDir);
    if (!icon.isEmpty()) appendString(data, "icon", icon);
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
    void testPreparationSharesGameFilesAndShaderCacheOnlyAndCleanupRestoresConfig()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString steamRoot = home.path() + QStringLiteral("/.steam/steam");
        const QString externalLibrary = home.path() + QStringLiteral("/external library");
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
        const QString primaryCommon = steamRoot + QStringLiteral("/steamapps/common");
        const QString primaryShaderCache = steamRoot + QStringLiteral("/steamapps/shadercache");
        const QString externalCommon = externalLibrary + QStringLiteral("/steamapps/common");
        QVERIFY(QDir().mkpath(primaryCommon));
        QVERIFY(QDir().mkpath(primaryShaderCache));
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/steamapps/compatdata")));
        QVERIFY(QDir().mkpath(externalCommon));
        QFile sourceSentinel(primaryCommon + QStringLiteral("/source.txt"));
        QVERIFY(sourceSentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sourceSentinel.write("source remains private\n"), qint64(23));
        sourceSentinel.close();

        QFile libraries(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
        QVERIFY(libraries.open(QIODevice::WriteOnly | QIODevice::Text));
        libraries.write("\"libraryfolders\"\n{\n"
                        "  \"0\"\n  {\n    \"path\"\t\"" + steamRoot.toUtf8() + "\"\n  }\n"
                        "  \"1\"\n  {\n    \"path\"\t\"" + externalLibrary.toUtf8() + "\"\n  }\n}\n");
        libraries.close();

        const QString player = QStringLiteral("player");
        const QString playerHome = PlayerStore::homeDir(player);
        const QString playerSteam = playerHome + QStringLiteral("/.local/share/Steam");
        QVERIFY(QDir().mkpath(playerSteam + QStringLiteral("/config")));
        QVERIFY(QDir().mkpath(playerSteam + QStringLiteral("/userdata/76561198000000000/config")));
        QFile steamSh(playerSteam + QStringLiteral("/steam.sh"));
        QVERIFY(steamSh.open(QIODevice::WriteOnly));
        steamSh.close();

        SteamConfigManager manager;
        manager.setShareLibraryEnabled(true);
        QVERIFY(manager.isSteamDetected());

        DataDirectory marker;
        marker.path = steamRoot;
        marker.mode = QStringLiteral("overlay");
        QList<SandboxMount> mounts;
        QVERIFY(manager.prepareDataDir(marker, player, mounts));
        // Primary library: game files + shader cache. External library has no shader cache, so only its game files.
        // compatdata (saves, settings) is never shared.
        QCOMPARE(mounts.size(), 3);
        const QString libsRoot = QDir::homePath() + QStringLiteral("/.couchplay/steam-libs");
        const QStringList sources{primaryCommon, primaryShaderCache, externalCommon};
        const QStringList targets{libsRoot + QStringLiteral("/0/steamapps/common"),
                                  libsRoot + QStringLiteral("/0/steamapps/shadercache"),
                                  libsRoot + QStringLiteral("/1/steamapps/common")};
        for (int i = 0; i < 3; ++i) {
            const SandboxMount &mount = mounts.at(i);
            QCOMPARE(mount.kind, SandboxMount::Overlay);
            QCOMPARE(mount.source, sources.at(i));
            QCOMPARE(mount.target, targets.at(i));
            QCOMPARE(mount.upper, PlayerStore::layerDir(player, sources.at(i)) + QStringLiteral("/upper"));
            QCOMPARE(mount.work, PlayerStore::layerDir(player, sources.at(i)) + QStringLiteral("/work"));
            QVERIFY(QFileInfo(mount.upper).isDir());
            QVERIFY(QFileInfo(mount.work).isDir());
        }
        for (const SandboxMount &mount : mounts)
            QVERIFY(!mount.source.contains(QStringLiteral("compatdata")));

        QVERIFY(manager.finalizeDataDir(marker, player));
        // Steam loads steamapps/libraryfolders.vdf, so both locations must carry the shared libraries
        const QString targetLibraryFolders = playerSteam + QStringLiteral("/config/libraryfolders.vdf");
        const QString steamappsLibraryFolders = playerSteam + QStringLiteral("/steamapps/libraryfolders.vdf");
        QVERIFY(QFile::exists(targetLibraryFolders));
        QVERIFY(QFile::exists(steamappsLibraryFolders));
        for (const QString &path : {targetLibraryFolders, steamappsLibraryFolders}) {
            QFile shared(path);
            QVERIFY(shared.open(QIODevice::ReadOnly));
            const QByteArray content = shared.readAll();
            // Paths are the sandbox view: the real player home never appears in the config
            QVERIFY(content.contains((libsRoot + QStringLiteral("/1")).toUtf8()));
            QVERIFY(!content.contains(playerHome.toUtf8()));
        }
        QVERIFY(manager.cleanupLibrarySharing(player));
        for (const QString &path : {targetLibraryFolders, steamappsLibraryFolders}) {
            QFile restored(path);
            QVERIFY(restored.open(QIODevice::ReadOnly));
            QCOMPARE(restored.readAll(), QByteArray("\"libraryfolders\"\n{\n}\n"));
        }
        QVERIFY(sourceSentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sourceSentinel.readAll(), QByteArray("source remains private\n"));
    }

    void testFinalizeMirrorsDesktopCompatToolsForSharedGames()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString steamRoot = home.path() + QStringLiteral("/.steam/steam");
        const QString externalLibrary = home.path() + QStringLiteral("/external library");
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/steamapps")));
        QVERIFY(QDir().mkpath(externalLibrary + QStringLiteral("/steamapps")));
        const auto writeFile = [](const QString &path, const QByteArray &content) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
        };
        QVERIFY(writeFile(steamRoot + QStringLiteral("/config/libraryfolders.vdf"),
                          "\"libraryfolders\"\n{\n  \"0\"\n  {\n    \"path\"\t\"" + steamRoot.toUtf8()
                              + "\"\n  }\n  \"1\"\n  {\n    \"path\"\t\"" + externalLibrary.toUtf8()
                              + "\"\n  }\n}\n"));
        QVERIFY(writeFile(steamRoot + QStringLiteral("/steamapps/appmanifest_49520.acf"), "\"AppState\"\n{\n}\n"));
        QVERIFY(writeFile(steamRoot + QStringLiteral("/steamapps/appmanifest_345180.acf"), "\"AppState\"\n{\n}\n"));
        QVERIFY(writeFile(externalLibrary + QStringLiteral("/steamapps/appmanifest_220.acf"), "\"AppState\"\n{\n}\n"));
        // A tool installed only in the desktop user's compatibilitytools.d does not exist for players
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/compatibilitytools.d/GE-Proton9-1")));
        // The global default ("0") applies to shared games without their own entry; games outside the shared
        // libraries are never mirrored
        QVERIFY(writeFile(steamRoot + QStringLiteral("/config/config.vdf"),
                          "\"InstallConfigStore\"\n{\n\"Software\"\n{\n\"Valve\"\n{\n\"Steam\"\n{\n"
                          "\"CompatToolMapping\"\n{\n"
                          "\"0\"\n{\n\"name\" \"proton_9\"\n\"config\" \"\"\n\"priority\" \"75\"\n}\n"
                          "\"49520\"\n{\n\"name\" \"proton_9\"\n\"config\" \"\"\n\"priority\" \"250\"\n}\n"
                          "\"999\"\n{\n\"name\" \"proton_8\"\n\"config\" \"\"\n\"priority\" \"250\"\n}\n"
                          "\"345180\"\n{\n\"name\" \"GE-Proton9-1\"\n\"config\" \"\"\n\"priority\" \"250\"\n}\n"
                          "}\n}\n}\n}\n}\n"));

        const QString player = QStringLiteral("player");
        const QString playerSteam = PlayerStore::homeDir(player) + QStringLiteral("/.local/share/Steam");
        QVERIFY(QDir().mkpath(playerSteam + QStringLiteral("/config")));
        QVERIFY(QDir().mkpath(playerSteam + QStringLiteral("/userdata/76561198000000000/config")));
        QVERIFY(writeFile(playerSteam + QStringLiteral("/steam.sh"), "#!/bin/sh\n"));
        // The player already runs 345180 on another tool and has its own unrelated mapping
        const QString playerConfig = playerSteam + QStringLiteral("/config/config.vdf");
        QVERIFY(writeFile(playerConfig,
                          "\"InstallConfigStore\"\n{\n\"Software\"\n{\n\"Valve\"\n{\n\"Steam\"\n{\n"
                          "\"CompatToolMapping\"\n{\n"
                          "\"345180\"\n{\n\"name\" \"proton_8\"\n\"config\" \"\"\n\"priority\" \"250\"\n}\n"
                          "\"777\"\n{\n\"name\" \"proton_7\"\n\"config\" \"\"\n\"priority\" \"250\"\n}\n"
                          "}\n}\n}\n}\n}\n"));

        SteamConfigManager manager;
        manager.setShareLibraryEnabled(true);
        QVERIFY(manager.isSteamDetected());
        DataDirectory marker;
        marker.path = steamRoot;
        marker.mode = QStringLiteral("overlay");
        QList<SandboxMount> mounts;
        QVERIFY(manager.prepareDataDir(marker, player, mounts));

        QVERIFY(manager.finalizeDataDir(marker, player));

        const auto readMapping = [&]() {
            QFile file(playerConfig);
            return file.open(QIODevice::ReadOnly) ? SteamCompatToolMapping::read(file.readAll())
                                                  : std::optional<QMap<QString, SteamCompatToolMapping::Tool>>();
        };
        auto mapping = readMapping();
        QVERIFY(mapping.has_value());
        QVERIFY(mapping->contains(QStringLiteral("49520")));
        QCOMPARE(mapping->value(QStringLiteral("49520")).name, QStringLiteral("proton_9"));
        QCOMPARE(mapping->value(QStringLiteral("49520")).priority, QStringLiteral("250"));
        // The desktop-only tool of 345180 is dropped
        QVERIFY(!mapping->contains(QStringLiteral("345180")));
        // 220 has no entry of its own and takes the desktop's global default
        QVERIFY(mapping->contains(QStringLiteral("220")));
        QCOMPARE(mapping->value(QStringLiteral("220")).name, QStringLiteral("proton_9"));
        QCOMPARE(mapping->value(QStringLiteral("220")).priority, QStringLiteral("75"));
        QVERIFY(!mapping->contains(QStringLiteral("0")));
        QVERIFY(!mapping->contains(QStringLiteral("999")));
        QVERIFY(mapping->contains(QStringLiteral("777")));

        const auto setHostMapping = [&](const QByteArray &entries) {
            return writeFile(steamRoot + QStringLiteral("/config/config.vdf"),
                             "\"InstallConfigStore\"\n{\n\"Software\"\n{\n\"Valve\"\n{\n\"Steam\"\n{\n"
                             "\"CompatToolMapping\"\n{\n"
                                 + entries + "}\n}\n}\n}\n}\n");
        };
        const QByteArray perApp = "\"49520\"\n{\n\"name\" \"proton_9\"\n\"config\" \"\"\n\"priority\" \"250\"\n}\n";

        const auto toolOf = [&](const QString &appId) {
            auto current = readMapping();
            return current && current->contains(appId) ? current->value(appId) : SteamCompatToolMapping::Tool();
        };

        // Without a global default on the desktop, games without their own entry lose the player's entry
        QVERIFY(setHostMapping(perApp));
        QVERIFY(manager.finalizeDataDir(marker, player));
        QCOMPARE(toolOf(QStringLiteral("49520")).name, QStringLiteral("proton_9"));
        QCOMPARE(toolOf(QStringLiteral("345180")).name, QString());
        QCOMPARE(toolOf(QStringLiteral("220")).name, QString());

        // The tool the desktop's Steam picked on its own (kept only in its compat_log.txt) is set for the player,
        // unless it is "native" or exists only for the desktop user; the newest log line per game wins
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/logs")));
        QVERIFY(writeFile(steamRoot + QStringLiteral("/logs/compat_log.txt"),
                          "[2026-11-01 10:00:00] Recording non-user mapping for 220 at priority 85 to tool proton-8.0\n"
                          "[2026-11-02 10:00:00] Recording non-user mapping for 220 at priority 85 to tool proton-stable\n"
                          "[2026-11-02 10:00:00] Recording non-user mapping for 345180 at priority 85 to tool GE-Proton9-1\n"
                          "[2026-11-02 10:00:00] Recording non-user mapping for 49520 at priority 85 to tool native\n"
                          "[2026-11-02 10:00:00] Recording non-user mapping for 999 at priority 75 to tool native\n"));
        QVERIFY(manager.finalizeDataDir(marker, player));
        QCOMPARE(toolOf(QStringLiteral("220")).name, QStringLiteral("proton-stable"));
        QCOMPARE(toolOf(QStringLiteral("220")).priority, QStringLiteral("250"));
        QCOMPARE(toolOf(QStringLiteral("345180")).name, QString());
        QCOMPARE(toolOf(QStringLiteral("49520")).name, QStringLiteral("proton_9"));
        QVERIFY(QFile::remove(steamRoot + QStringLiteral("/logs/compat_log.txt")));

        // A global default that only exists for the desktop user is not mirrored
        QVERIFY(setHostMapping("\"0\"\n{\n\"name\" \"GE-Proton9-1\"\n\"config\" \"\"\n\"priority\" \"75\"\n}\n"
                               + perApp));
        QVERIFY(manager.finalizeDataDir(marker, player));
        QCOMPARE(toolOf(QStringLiteral("49520")).name, QStringLiteral("proton_9"));
        QCOMPARE(toolOf(QStringLiteral("345180")).name, QString());
        QCOMPARE(toolOf(QStringLiteral("220")).name, QString());

        // A global default alone is mirrored to every shared game
        QVERIFY(setHostMapping("\"0\"\n{\n\"name\" \"proton_9\"\n\"config\" \"\"\n\"priority\" \"75\"\n}\n"));
        QVERIFY(manager.finalizeDataDir(marker, player));
        for (const QString &appId : {QStringLiteral("49520"), QStringLiteral("345180"), QStringLiteral("220")}) {
            QCOMPARE(toolOf(appId).name, QStringLiteral("proton_9"));
            QCOMPARE(toolOf(appId).priority, QStringLiteral("75"));
        }

        // An unreadable desktop mapping must not turn into removals for the player
        QFile before(playerConfig);
        QVERIFY(before.open(QIODevice::ReadOnly));
        const QByteArray beforeBytes = before.readAll();
        before.close();
        QVERIFY(writeFile(steamRoot + QStringLiteral("/config/config.vdf"), "\"InstallConfigStore\"\n{\n"));
        QVERIFY(manager.finalizeDataDir(marker, player));
        QFile after(playerConfig);
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), beforeBytes);
    }

    void testCleanupDoesNotCreateSteamDirectoryForUnbootstrappedPlayer()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString playerHome = PlayerStore::homeDir(QStringLiteral("player"));
        QVERIFY(QDir().mkpath(playerHome));
        SteamConfigManager manager;

        QVERIFY(manager.cleanupLibrarySharing(QStringLiteral("player")));
        QVERIFY(!QFileInfo::exists(playerHome + QStringLiteral("/.steam")));
        QVERIFY(!QFileInfo::exists(playerHome + QStringLiteral("/.local/share/Steam")));
    }

    void testBootstrapDetectionAndSteamIdUseRealPlayerHome()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString player = QStringLiteral("player");
        const QString playerHome = PlayerStore::homeDir(player);
        const QString playerSteam = playerHome + QStringLiteral("/.local/share/Steam");
        SteamConfigManager manager;

        QVERIFY(!manager.isSteamBootstrapped(player));
        QVERIFY(!manager.isSteamBootstrapped(QStringLiteral("../escape")));

        // userdata alone is not a finished bootstrap
        QVERIFY(QDir().mkpath(playerSteam + QStringLiteral("/userdata/76561198000000001/config")));
        QVERIFY(!manager.isSteamBootstrapped(player));
        QCOMPARE(manager.getTargetSteamUserId(player), QStringLiteral("76561198000000001"));

        QVERIFY(QDir().mkpath(playerSteam + QStringLiteral("/ubuntu12_32")));
        QFile binary(playerSteam + QStringLiteral("/ubuntu12_32/steam"));
        QVERIFY(binary.open(QIODevice::WriteOnly));
        binary.close();
        QVERIFY(manager.isSteamBootstrapped(player));

        const SteamPaths paths = manager.getTargetSteamPaths(player);
        QVERIFY(paths.valid);
        QCOMPARE(paths.steamRoot, playerSteam);
        QCOMPARE(paths.libraryFoldersVdf, playerSteam + QStringLiteral("/config/libraryfolders.vdf"));
        QCOMPARE(paths.shortcutsVdf,
                 playerSteam + QStringLiteral("/userdata/76561198000000001/config/shortcuts.vdf"));
    }

    void testSyncShortcutsWritesDirectlyIntoPlayerHome()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString desktopSteam = home.path() + QStringLiteral("/.local/share/Steam");
        QVERIFY(QDir().mkpath(desktopSteam + QStringLiteral("/userdata/123/config")));
        const QByteArray data = makeShortcutDocument("Direct Copy", 9);
        QFile source(desktopSteam + QStringLiteral("/userdata/123/config/shortcuts.vdf"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(data), qint64(data.size()));
        source.close();

        const QString player = QStringLiteral("player");
        const QString playerSteam = PlayerStore::homeDir(player) + QStringLiteral("/.local/share/Steam");
        QVERIFY(QDir().mkpath(playerSteam + QStringLiteral("/userdata/456/config")));
        QFile steamSh(playerSteam + QStringLiteral("/steam.sh"));
        QVERIFY(steamSh.open(QIODevice::WriteOnly));
        steamSh.close();

        SteamConfigManager manager;
        QVERIFY(manager.sourceAccountAvailable());
        QVERIFY(manager.syncShortcutsToUser(player));

        QFile target(playerSteam + QStringLiteral("/userdata/456/config/shortcuts.vdf"));
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), data);
    }

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

    void testShortcutDirectoriesExcludeRelativeAndSystemPaths()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard configGuard("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());

        const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
        const QString gameDir = home.path() + QStringLiteral("/games/Game");
        const QString iconDir = home.path() + QStringLiteral("/icons");
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/userdata/123/config")));
        QVERIFY(QDir().mkpath(gameDir));
        QVERIFY(QDir().mkpath(iconDir));

        // A relative "./" start dir is dropped; the home-based exe and icon directories are kept.
        const QByteArray data = makeShortcutDocument("Mixed", 7, (gameDir + QStringLiteral("/run.sh")).toUtf8(),
                                                     "\"./\"", (iconDir + QStringLiteral("/icon.png")).toUtf8());
        QFile shortcuts(steamRoot + QStringLiteral("/userdata/123/config/shortcuts.vdf"));
        QVERIFY(shortcuts.open(QIODevice::WriteOnly));
        QCOMPARE(shortcuts.write(data), qint64(data.size()));
        shortcuts.close();

        SteamConfigManager manager;
        QVERIFY(manager.sourceAccountAvailable());
        manager.loadShortcuts();
        QCOMPARE(manager.shortcutCount(), 1);
        QStringList dirs = manager.extractShortcutDirectories();
        dirs.sort();
        QStringList expected{gameDir, iconDir};
        expected.sort();
        QCOMPARE(dirs, expected);

        // A system binary, the filesystem root and a relative icon leave nothing to mount.
        const QByteArray system = makeShortcutDocument("System", 8, "/usr/bin/true", "/", "relative/icon.png");
        QVERIFY(shortcuts.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(shortcuts.write(system), qint64(system.size()));
        shortcuts.close();
        manager.loadShortcuts();
        QCOMPARE(manager.shortcutCount(), 1);
        QVERIFY(manager.extractShortcutDirectories().isEmpty());
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
        QVERIFY(!manager.sourceAccountAvailable());
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
