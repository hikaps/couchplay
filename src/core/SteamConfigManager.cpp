// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"
#include <algorithm>
#include "PresetManager.h"
#include "../dbus/CouchPlayHelperClient.h"
#include "Logging.h"
#include "UserLookup.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <utility>
#include <QRegularExpression>
#include <QStandardPaths>

#include <KConfigGroup>
#include <KSharedConfig>

#include <pwd.h>
#include <unistd.h>

SteamConfigManager::SteamConfigManager(QObject *parent)
    : QObject(parent)
{
    // Get current user's home directory
    const char *home = getenv("HOME");
    if (home) {
        m_userHome = QString::fromLocal8Bit(home);
    } else {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            m_userHome = QString::fromLocal8Bit(pw->pw_dir);
        }
    }

    // Auto-detect Steam on construction
    detectSteamPaths();

    // Load settings from config
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Steam"));
    m_syncShortcutsEnabled = group.readEntry(QStringLiteral("SyncShortcutsEnabled"), false);
    m_shareLibraryEnabled = group.readEntry(QStringLiteral("ShareLibraryEnabled"), false);
}

void SteamConfigManager::setHelperClient(CouchPlayHelperClient *client)
{
    if (m_helperClient != client) {
        m_helperClient = client;
        Q_EMIT helperClientChanged();
    }
}

void SteamConfigManager::setSyncShortcutsEnabled(bool enabled)
{
    if (m_syncShortcutsEnabled != enabled) {
        m_syncShortcutsEnabled = enabled;

        // Persist to config
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        KConfigGroup group = config->group(QStringLiteral("Steam"));
        group.writeEntry(QStringLiteral("SyncShortcutsEnabled"), enabled);
        config->sync();

        Q_EMIT syncShortcutsEnabledChanged();
    }
}

void SteamConfigManager::setShareLibraryEnabled(bool enabled)
{
    if (m_shareLibraryEnabled != enabled) {
        m_shareLibraryEnabled = enabled;
        
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        KConfigGroup group = config->group(QStringLiteral("Steam"));
        group.writeEntry(QStringLiteral("ShareLibraryEnabled"), enabled);
        config->sync();
        
        Q_EMIT shareLibraryEnabledChanged();
    }
}

void SteamConfigManager::detectSteamPaths()
{
    m_steamPaths = SteamPaths();

    // Check common Steam locations
    QStringList possibleRoots = {
        m_userHome + QStringLiteral("/.steam/steam"),
        m_userHome + QStringLiteral("/.local/share/Steam"),
        m_userHome + QStringLiteral("/.var/app/com.valvesoftware.Steam/.steam/steam"), // Flatpak
        m_userHome + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam"),
    };

    for (const QString &root : possibleRoots) {
        QString configDir = root + QStringLiteral("/config");
        QString libraryVdf = configDir + QStringLiteral("/libraryfolders.vdf");

        if (QFile::exists(libraryVdf)) {
            m_steamPaths.steamRoot = root;
            m_steamPaths.configDir = configDir;
            m_steamPaths.libraryFoldersVdf = libraryVdf;

            // Find userdata directory (contains Steam user ID subdirectories)
            QString userDataBase = root + QStringLiteral("/userdata");
            QDir userDataDir(userDataBase);
            if (userDataDir.exists()) {
                // Get first numeric subdirectory (Steam user ID)
                QStringList entries = userDataDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QString &entry : entries) {
                    bool ok;
                    entry.toULongLong(&ok);
                    if (ok) {
                        m_steamPaths.userDataDir = userDataBase + QStringLiteral("/") + entry;
                        m_steamPaths.shortcutsVdf = m_steamPaths.userDataDir + QStringLiteral("/config/shortcuts.vdf");
                        break;
                    }
                }
            }

            m_steamPaths.valid = true;
            qDebug() << "SteamConfigManager: Detected Steam at" << root;
            break;
        }
    }

    if (!m_steamPaths.valid) {
        qWarning() << "SteamConfigManager: Steam installation not found";
    }

    Q_EMIT steamPathsChanged();
}

QString SteamConfigManager::getSteamUserId() const
{
    if (m_steamPaths.userDataDir.isEmpty()) {
        return QString();
    }

    // Extract user ID from path (last component)
    return QFileInfo(m_steamPaths.userDataDir).fileName();
}

QString SteamConfigManager::getTargetSteamUserId(const QString &username) const
{
    // Use helper to get Steam ID since we can't read other users' home directories
    if (m_helperClient && m_helperClient->isAvailable()) {
        QString steamId = m_helperClient->getUserSteamId(username);
        if (!steamId.isEmpty()) {
            qDebug() << "SteamConfigManager: Got Steam ID" << steamId << "for user" << username << "via helper";
            return steamId;
        }
        // Helper returned empty - Steam not installed for this user or helper error
        qWarning() << "SteamConfigManager: Helper could not find Steam ID for" << username;
        return QString();
    }

    // Fallback: try to read directly (will only work for current user)
    qDebug() << "SteamConfigManager: Helper not available, trying direct access for" << username;

    const UserIdentity id = resolveUserIdentity(username, m_helperClient);
    if (!id.valid) {
        qWarning() << "SteamConfigManager: User not found:" << username;
        return QString();
    }

    QString targetHome = id.home;

    // Check for Steam userdata in common locations
    QStringList possibleRoots = {
        targetHome + QStringLiteral("/.steam/steam/userdata"),
        targetHome + QStringLiteral("/.local/share/Steam/userdata"),
    };

    for (const QString &userDataBase : possibleRoots) {
        QDir userDataDir(userDataBase);
        if (!userDataDir.exists()) {
            continue;
        }

        // Find first numeric directory (Steam user ID)
        QStringList entries = userDataDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            bool ok;
            entry.toULongLong(&ok);
            if (ok) {
                qDebug() << "SteamConfigManager: Found Steam ID" << entry << "for user" << username;
                return entry;
            }
        }
    }

    qWarning() << "SteamConfigManager: Steam userdata not found for" << username;
    return QString();
}

void SteamConfigManager::loadShortcuts()
{
    m_shortcuts.clear();

    if (!m_steamPaths.valid || m_steamPaths.shortcutsVdf.isEmpty()) {
        qCWarning(couchplaySteam) << "Cannot load shortcuts - Steam not detected";
        Q_EMIT shortcutsLoaded();
        return;
    }

    // Try shortcuts.vdf first, then fall back to backup files
    QString sourceFile = m_steamPaths.shortcutsVdf;
    if (!QFile::exists(sourceFile)) {
        QString configDir = QFileInfo(sourceFile).absolutePath();
        QString backup = configDir + QStringLiteral("/shortcuts.backup");
        QString firstBackup = configDir + QStringLiteral("/shortcuts.firstbackup");

        if (QFile::exists(backup)) {
            sourceFile = backup;
            qCDebug(couchplaySteam) << "shortcuts.vdf not found, using shortcuts.backup";
        } else if (QFile::exists(firstBackup)) {
            sourceFile = firstBackup;
            qCDebug(couchplaySteam) << "shortcuts.vdf not found, using shortcuts.firstbackup";
        } else {
            qCDebug(couchplaySteam) << "No shortcuts file found (checked .vdf, .backup, .firstbackup)";
            Q_EMIT shortcutsLoaded();
            return;
        }
    }
    QFile shortcutFile(sourceFile);
    if (!shortcutFile.open(QIODevice::ReadOnly)) {
        qCWarning(couchplaySteam) << "Failed to read shortcuts:" << sourceFile;
        Q_EMIT shortcutsLoaded();
        return;
    }
    const QByteArray shortcutBytes = shortcutFile.readAll();
    QString parseError;
    if (!SteamShortcutsVdf::decode(shortcutBytes, &m_shortcuts, &parseError)) {
        qCWarning(couchplaySteam) << "Failed to parse shortcuts:" << parseError;
        m_shortcuts.clear();
        Q_EMIT shortcutsLoaded();
        return;
    }
    qCDebug(couchplaySteam) << "Loaded" << m_shortcuts.size() << "shortcuts from" << sourceFile;

    Q_EMIT shortcutsLoaded();
}

QList<SteamGame> SteamConfigManager::parseInstalledGames() const
{
    QList<SteamGame> games;
    static const QRegularExpression appIdPattern(QStringLiteral("\"appid\"\\s+\"(\\d+)\""));
    static const QRegularExpression namePattern(QStringLiteral("\"name\"\\s+\"([^\"]*)\""));
    static const QRegularExpression installDirPattern(QStringLiteral("\"installdir\"\\s+\"([^\"]*)\""));

    for (const SteamLibraryFolder &library : m_libraries) {
        for (const quint32 appId : library.appIds) {
            if (appId == 0) {
                continue;
            }
            const QString manifestPath = QDir(library.path).filePath(
                QStringLiteral("steamapps/appmanifest_%1.acf").arg(appId));
            QFile manifest(manifestPath);
            if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }
            const QString content = QString::fromUtf8(manifest.readAll());
            manifest.close();

            const auto appIdMatch = appIdPattern.match(content);
            const auto nameMatch = namePattern.match(content);
            const auto installDirMatch = installDirPattern.match(content);
            if (!appIdMatch.hasMatch() || !nameMatch.hasMatch() || !installDirMatch.hasMatch()) {
                continue;
            }
            bool ok = false;
            const quint32 parsedId = appIdMatch.captured(1).toUInt(&ok);
            if (!ok || parsedId == 0 || parsedId != appId) {
                continue;
            }

            SteamGame game;
            game.id = QString::number(parsedId);
            game.title = nameMatch.captured(1);
            game.installPath = QDir(library.path).filePath(
                QStringLiteral("steamapps/common/%1").arg(installDirMatch.captured(1)));
            game.source = QStringLiteral("native");
            games.append(game);
        }
    }
    return games;
}

void SteamConfigManager::loadGames()
{
    m_games.clear();
    if (!m_steamPaths.valid) {
        Q_EMIT gamesLoaded();
        return;
    }

    loadLibraryFolders();
    loadShortcuts();
    m_games = parseInstalledGames();

    QSet<QString> keys;
    for (const SteamShortcut &shortcut : std::as_const(m_shortcuts)) {
        if (SteamShortcutsVdf::isProfileShortcut(shortcut) || shortcut.appId == 0 || shortcut.appName.isEmpty()) {
            continue;
        }
        const quint64 shortcutId = (static_cast<quint64>(shortcut.appId) << 32) | 0x02000000ULL;
        SteamGame game;
        game.id = QString::number(shortcutId);
        game.title = shortcut.appName;
        game.installPath = shortcut.startDir;
        game.source = QStringLiteral("shortcut");
        const QString key = game.source + QLatin1Char(':') + game.id;
        if (!keys.contains(key)) {
            keys.insert(key);
            m_games.append(game);
        }
    }

    std::sort(m_games.begin(), m_games.end(), [](const SteamGame &left, const SteamGame &right) {
        return QString::compare(left.title, right.title, Qt::CaseInsensitive) < 0;
    });
    Q_EMIT gamesLoaded();
}

QVariantList SteamConfigManager::gamesAsVariant() const
{
    QVariantList list;
    for (const SteamGame &game : m_games) {
        list.append(QVariantMap{{QStringLiteral("launcherId"), QStringLiteral("steam")},
                                {QStringLiteral("backend"), game.source},
                                {QStringLiteral("gameId"), game.id},
                                {QStringLiteral("title"), game.title},
                                {QStringLiteral("installPath"), game.installPath},
                                {QStringLiteral("source"), game.source}});
    }
    return list;
}

QVariantList SteamConfigManager::shortcutsAsVariant() const
{
    QVariantList list;
    for (const SteamShortcut &sc : m_shortcuts) {
        QVariantMap map;
        map[QStringLiteral("appId")] = sc.appId;
        map[QStringLiteral("appName")] = sc.appName;
        map[QStringLiteral("exe")] = sc.exe;
        map[QStringLiteral("startDir")] = sc.startDir;
        map[QStringLiteral("icon")] = sc.icon;
        map[QStringLiteral("launchOptions")] = sc.launchOptions;
        list.append(map);
    }
    return list;
}

QStringList SteamConfigManager::extractShortcutDirectories() const
{
    QSet<QString> dirs;

    for (const SteamShortcut &sc : m_shortcuts) {
        if (SteamShortcutsVdf::isProfileShortcut(sc)) {
            continue;
        }
        if (!sc.exe.isEmpty()) {
            QString exePath = sc.exe;
            // Remove quotes if present
            if (exePath.startsWith(QLatin1Char('"')) && exePath.endsWith(QLatin1Char('"'))) {
                exePath = exePath.mid(1, exePath.length() - 2);
            }
            QString exeDir = QFileInfo(exePath).absolutePath();
            if (!exeDir.isEmpty() && QDir(exeDir).exists()) {
                dirs.insert(exeDir);
            }
        }

        // Extract startDir
        if (!sc.startDir.isEmpty()) {
            QString startDir = sc.startDir;
            // Remove quotes if present
            if (startDir.startsWith(QLatin1Char('"')) && startDir.endsWith(QLatin1Char('"'))) {
                startDir = startDir.mid(1, startDir.length() - 2);
            }
            if (QDir(startDir).exists()) {
                dirs.insert(startDir);
            }
        }

        // Extract icon directory
        if (!sc.icon.isEmpty()) {
            QString iconPath = sc.icon;
            // Remove quotes if present
            if (iconPath.startsWith(QLatin1Char('"')) && iconPath.endsWith(QLatin1Char('"'))) {
                iconPath = iconPath.mid(1, iconPath.length() - 2);
            }
            QString iconDir = QFileInfo(iconPath).absolutePath();
            if (!iconDir.isEmpty() && QDir(iconDir).exists()) {
                dirs.insert(iconDir);
            }
        }
    }

    // Remove empty strings
    dirs.remove(QString());

    return dirs.values();
}

bool SteamConfigManager::syncShortcutsToUser(const QString &targetUsername, std::function<bool()> shouldContinue)
{
    qCDebug(couchplaySteam) << "syncShortcutsToUser called for" << targetUsername;
    if (shouldContinue && !shouldContinue()) {
        return false;
    }

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Helper not available";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Helper not available"));
        return false;
    }

    if (!m_steamPaths.valid || m_steamPaths.shortcutsVdf.isEmpty()) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Steam not detected";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Steam not detected"));
        return false;
    }

    const QString sourceFile = m_steamPaths.shortcutsVdf;
    if (!QFile::exists(sourceFile)) {
        qCDebug(couchplaySteam) << "No shortcuts.vdf to sync";
        return true;
    }

    qCDebug(couchplaySteam) << "Source file:" << sourceFile;
    const bool targetSteamBootstrapped = m_helperClient->isSteamBootstrapped(targetUsername);
    if (shouldContinue && !shouldContinue()) {
        return false;
    }
    if (!targetSteamBootstrapped) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Steam not set up for user" << targetUsername;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Steam not set up for user (run Steam once first)"));
        return false;
    }

    const SteamPaths targetPaths = getTargetSteamPaths(targetUsername);
    if (shouldContinue && !shouldContinue()) {
        return false;
    }
    if (!targetPaths.valid || targetPaths.shortcutsVdf.isEmpty() || targetPaths.userDataDir.isEmpty()) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Could not resolve target Steam paths for"
                                   << targetUsername;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Could not resolve target Steam paths"));
        return false;
    }
    const QString targetSteamId = QFileInfo(targetPaths.userDataDir).fileName();
    bool targetSteamIdIsNumeric = !targetSteamId.isEmpty() && targetSteamId.size() <= 20;
    for (const QChar ch : targetSteamId) {
        targetSteamIdIsNumeric = targetSteamIdIsNumeric && ch.unicode() >= '0' && ch.unicode() <= '9';
    }
    if (!targetSteamIdIsNumeric) {
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Could not resolve target Steam account"));
        return false;
    }
    const QString targetVdf = targetPaths.shortcutsVdf;

    QFile sourceFileHandle(sourceFile);
    if (!sourceFileHandle.open(QIODevice::ReadOnly)) {
        qCWarning(couchplaySteam) << "Failed to open source file:" << sourceFile;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Failed to open source shortcuts.vdf"));
        return false;
    }

    const QByteArray sourceVdfData = sourceFileHandle.readAll();
    sourceFileHandle.close();
    if (shouldContinue && !shouldContinue()) {
        return false;
    }

    QByteArray targetVdfData;
    QString helperError;
    if (!m_helperClient->readSteamShortcutsForUser(targetUsername,
                                                  targetSteamId,
                                                  &targetVdfData,
                                                  shouldContinue,
                                                  &helperError)) {
        if (shouldContinue && !shouldContinue()) {
            return false;
        }
        const QString message = helperError.isEmpty()
            ? QStringLiteral("Failed to read target shortcuts.vdf")
            : QStringLiteral("Failed to read target shortcuts.vdf: %1").arg(helperError);
        qCWarning(couchplaySteam) << message;
        Q_EMIT syncFailed(targetUsername, message);
        return false;
    }
    if (shouldContinue && !shouldContinue()) {
        return false;
    }

    const QByteArray expectedDigest = targetVdfData.isEmpty()
        ? QByteArrayLiteral("missing")
        : QCryptographicHash::hash(targetVdfData, QCryptographicHash::Sha256).toHex();
    if (targetVdfData.isEmpty()) {
        targetVdfData = SteamShortcutsVdf::emptyDocument();
    }

    QByteArray vdfData;
    QString mergeError;
    if (!SteamShortcutsVdf::mergePreservingProfiles(sourceVdfData, targetVdfData, &vdfData, &mergeError)) {
        qCWarning(couchplaySteam) << "Failed to preserve target CouchPlay shortcuts:" << mergeError;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Invalid source or target shortcuts.vdf"));
        return false;
    }
    if (shouldContinue && !shouldContinue()) {
        return false;
    }

    qCDebug(couchplaySteam) << "Read" << vdfData.size() << "bytes from source, conditionally writing to" << targetVdf;
    helperError.clear();
    const bool success = m_helperClient->writeSteamShortcutsForUser(targetUsername,
                                                                    targetSteamId,
                                                                    expectedDigest,
                                                                    vdfData,
                                                                    &helperError);
    if (success) {
        qCDebug(couchplaySteam) << "Synced shortcuts to" << targetUsername;
        Q_EMIT syncCompleted(targetUsername);
        return true;
    }
    if (shouldContinue && !shouldContinue()) {
        return false;
    }

    const QString message = helperError.isEmpty()
        ? QStringLiteral("Failed to write shortcuts.vdf")
        : QStringLiteral("Failed to write shortcuts.vdf: %1").arg(helperError);
    qCWarning(couchplaySteam) << "syncShortcutsToUser failed -" << message;
    Q_EMIT syncFailed(targetUsername, message);
    return false;
}

// Get target Steam paths for a user (uses target user's Steam ID)
SteamPaths SteamConfigManager::getTargetSteamPaths(const QString &username) const
{
    SteamPaths paths;

    // Get target user's home directory
    const UserIdentity id = resolveUserIdentity(username, m_helperClient);
    if (!id.valid) {
        qWarning() << "SteamConfigManager: User not found:" << username;
        return paths;
    }

    QString targetHome = id.home;
    // In a Flatpak build the GUI cannot inspect another user's host home.
    // Ask the helper to resolve the existing root there before trying any
    // process-local filesystem checks.
    if (m_helperClient && m_helperClient->isAvailable()) {
        const QString helperRoot = m_helperClient->getUserSteamRoot(username);
        if (!helperRoot.isEmpty()) {
            paths.steamRoot = helperRoot;
            paths.configDir = helperRoot + QStringLiteral("/config");
            paths.libraryFoldersVdf = paths.configDir + QStringLiteral("/libraryfolders.vdf");

            const QString targetSteamId = getTargetSteamUserId(username);
            if (!targetSteamId.isEmpty()) {
                paths.userDataDir = helperRoot + QStringLiteral("/userdata/") + targetSteamId;
                paths.shortcutsVdf = paths.userDataDir + QStringLiteral("/config/shortcuts.vdf");
            }
            paths.valid = true;
            return paths;
        }
    }

    // Check for Steam in common locations relative to target home
    // Prefer Steam's real data directory. On standard installations
    // ~/.steam/steam is often a symlink to ~/.local/share/Steam; retaining
    // that spelling would make secure helper writes reject the target.
    const QString canonicalHome = QFileInfo(targetHome).canonicalFilePath();
    const QStringList possibleRoots = {
        targetHome + QStringLiteral("/.local/share/Steam"),
        targetHome + QStringLiteral("/.steam/steam"),
    };

    for (const QString &candidateRoot : possibleRoots) {
        QString root = candidateRoot;
        const QString canonicalRoot = QFileInfo(candidateRoot).canonicalFilePath();
        if (!canonicalHome.isEmpty() && !canonicalRoot.isEmpty()) {
            if (canonicalRoot != canonicalHome && !canonicalRoot.startsWith(canonicalHome + QLatin1Char('/'))) {
                continue;
            }
            // Keep the user's home spelling in the path sent to the helper,
            // while removing any symlink below that home.
            root = targetHome + canonicalRoot.mid(canonicalHome.length());
        }

        QString configDir = root + QStringLiteral("/config");

        // For target user, check if Steam exists
        QDir steamDir(root);
        if (steamDir.exists()) {
            paths.steamRoot = root;
            paths.configDir = configDir;
            paths.libraryFoldersVdf = configDir + QStringLiteral("/libraryfolders.vdf");

            // For userdata, use TARGET user's Steam ID (not compositor's)
            QString targetSteamId = getTargetSteamUserId(username);
            if (!targetSteamId.isEmpty()) {
                paths.userDataDir = root + QStringLiteral("/userdata/") + targetSteamId;
                paths.shortcutsVdf = paths.userDataDir + QStringLiteral("/config/shortcuts.vdf");
            }

            paths.valid = true;
            break;
        }
    }

    // Default to ~/.steam/steam if nothing found
    if (!paths.valid) {
        paths.steamRoot = targetHome + QStringLiteral("/.steam/steam");
        paths.configDir = paths.steamRoot + QStringLiteral("/config");
        paths.libraryFoldersVdf = paths.configDir + QStringLiteral("/libraryfolders.vdf");

        QString targetSteamId = getTargetSteamUserId(username);
        if (!targetSteamId.isEmpty()) {
            paths.userDataDir = paths.steamRoot + QStringLiteral("/userdata/") + targetSteamId;
            paths.shortcutsVdf = paths.userDataDir + QStringLiteral("/config/shortcuts.vdf");
        }

        paths.valid = true; // Will be created by helper
    }

    return paths;
}


void SteamConfigManager::loadLibraryFolders()
{
    m_libraries.clear();
    
    if (!m_steamPaths.valid || m_steamPaths.libraryFoldersVdf.isEmpty()) {
        qCWarning(couchplaySteam) << "Cannot load library folders - Steam not detected";
        Q_EMIT librariesLoaded();
        return;
    }
    
    if (!QFile::exists(m_steamPaths.libraryFoldersVdf)) {
        qCDebug(couchplaySteam) << "libraryfolders.vdf not found at" << m_steamPaths.libraryFoldersVdf;
        Q_EMIT librariesLoaded();
        return;
    }
    
    m_libraries = parseLibraryFoldersVdf(m_steamPaths.libraryFoldersVdf);
    qCDebug(couchplaySteam) << "Loaded" << m_libraries.size() << "library folders";
    
    Q_EMIT librariesLoaded();
}

QVariantList SteamConfigManager::librariesAsVariant() const
{
    QVariantList list;
    for (const SteamLibraryFolder &folder : m_libraries) {
        QVariantMap map;
        map[QStringLiteral("path")] = folder.path;
        map[QStringLiteral("label")] = folder.label;
        map[QStringLiteral("totalSize")] = folder.totalSize;
        
        QVariantList appIdList;
        for (quint32 appId : folder.appIds) {
            appIdList.append(appId);
        }
        map[QStringLiteral("appIds")] = appIdList;
        
        list.append(map);
    }
    return list;
}

QList<SteamLibraryFolder> SteamConfigManager::parseLibraryFoldersVdf(const QString &path)
{
    QList<SteamLibraryFolder> result;
    
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(couchplaySteam) << "Failed to open libraryfolders.vdf:" << path;
        return result;
    }
    
    QString content = QString::fromUtf8(file.readAll());
    file.close();
    
    if (content.isEmpty()) {
        return result;
    }
    
    int pos = 0;
    
    auto skipWhitespace = [&]() {
        while (pos < content.size() && content[pos].isSpace()) {
            pos++;
        }
    };
    
    auto readToken = [&]() -> QString {
        skipWhitespace();
        if (pos >= content.size()) {
            return QString();
        }
        
        QChar c = content[pos];
        if (c == QLatin1Char('{') || c == QLatin1Char('}')) {
            pos++;
            return QString(c);
        }
        if (c == QLatin1Char('"')) {
            pos++;
            QString token;
            while (pos < content.size() && content[pos] != QLatin1Char('"')) {
                token += content[pos];
                pos++;
            }
            if (pos < content.size()) {
                pos++;
            }
            return token;
        }
        
        QString token;
        while (pos < content.size() && !content[pos].isSpace()
               && content[pos] != QLatin1Char('"')
               && content[pos] != QLatin1Char('{')
               && content[pos] != QLatin1Char('}')) {
            token += content[pos];
            pos++;
        }
        return token;
    };
    
    QString rootKey = readToken();
    if (rootKey != QStringLiteral("libraryfolders")) {
        qCWarning(couchplaySteam) << "Unexpected root key in libraryfolders.vdf:" << rootKey;
        return result;
    }
    
    QString openBrace = readToken();
    if (openBrace != QStringLiteral("{")) {
        qCWarning(couchplaySteam) << "Expected { after libraryfolders key";
        return result;
    }
    
    while (pos < content.size()) {
        skipWhitespace();
        if (pos >= content.size()) {
            break;
        }
        
        QString indexKey = readToken();
        if (indexKey.isEmpty() || indexKey == QStringLiteral("}")) {
            break;
        }
        
        QString entryBrace = readToken();
        if (entryBrace != QStringLiteral("{")) {
            break;
        }
        
        SteamLibraryFolder folder;
        
        while (pos < content.size()) {
            skipWhitespace();
            if (pos >= content.size()) {
                break;
            }
            
            QString key = readToken();
            if (key.isEmpty() || key == QStringLiteral("}")) {
                break;
            }
            
            if (key == QStringLiteral("apps")) {
                QString appsBrace = readToken();
                if (appsBrace != QStringLiteral("{")) {
                    break;
                }
                
                while (pos < content.size()) {
                    QString appIdStr = readToken();
                    if (appIdStr.isEmpty() || appIdStr == QStringLiteral("}")) {
                        break;
                    }
                    
                    bool ok;
                    quint32 appId = appIdStr.toUInt(&ok);
                    if (ok) {
                        folder.appIds.append(appId);
                    }
                    
                    readToken();
                }
                continue;
            }
            
            QString value = readToken();
            
            if (key == QStringLiteral("path")) {
                folder.path = value;
            } else if (key == QStringLiteral("label")) {
                folder.label = value;
            } else if (key == QStringLiteral("totalsize")) {
                folder.totalSize = value.toULongLong();
            }
        }
        
        if (!folder.path.isEmpty()) {
            result.append(folder);
        }
    }
    
    return result;
}

QString SteamConfigManager::generateLibraryFoldersVdf(const QList<SteamLibraryFolder> &libraries)
{
    QString vdf;
    vdf += QStringLiteral("\"libraryfolders\"\n{\n");
    
    for (int i = 0; i < libraries.size(); ++i) {
        const SteamLibraryFolder &library = libraries[i];
        vdf += QStringLiteral("\t\"%1\"\n\t{\n").arg(i);
        vdf += QStringLiteral("\t\t\"path\"\t\t\"%1\"\n").arg(library.path);
        vdf += QStringLiteral("\t\t\"label\"\t\t\"%1\"\n").arg(library.label);
        vdf += QStringLiteral("\t\t\"contentid\"\t\t\"0\"\n");
        vdf += QStringLiteral("\t\t\"totalsize\"\t\t\"%1\"\n").arg(library.totalSize);
        
        if (!library.appIds.isEmpty()) {
            vdf += QStringLiteral("\t\t\"apps\"\n\t\t{\n");
            for (quint32 appId : library.appIds) {
                vdf += QStringLiteral("\t\t\t\"%1\"\t\t\"%1\"\n").arg(appId);
            }
            vdf += QStringLiteral("\t\t}\n");
        }
        
        vdf += QStringLiteral("\t}\n");
    }
    
    vdf += QStringLiteral("}\n");
    return vdf;
}

bool SteamConfigManager::captureLibraryFoldersSnapshot(const QString &username)
{
    if (m_libraryFoldersSnapshots.contains(username)) return true;
    if (!m_helperClient || !m_helperClient->isAvailable()) return false;
    LibraryFoldersSnapshot snapshot;
    if (!m_helperClient->readSteamLibraryFoldersForUser(username, &snapshot.content, &snapshot.existed)) {
        qCWarning(couchplaySteam) << "Could not snapshot target libraryfolders.vdf for" << username;
        return false;
    }
    m_libraryFoldersSnapshots.insert(username, std::move(snapshot));
    return true;
}

bool SteamConfigManager::shareLibraryToUser(const QString &targetUsername)
{
    qCDebug(couchplaySteam) << "shareLibraryToUser called for" << targetUsername;
    
    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - Helper not available";
        return false;
    }
    
    if (!m_steamPaths.valid) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - Steam not detected";
        return false;
    }
    
    if (m_libraries.isEmpty()) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - No libraries parsed, call loadLibraryFolders() first";
        return false;
    }
    
    SteamPaths targetPaths = getTargetSteamPaths(targetUsername);
    if (!targetPaths.valid) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - Could not resolve target Steam paths for" << targetUsername;
        return false;
    }
    
    const UserIdentity id = resolveUserIdentity(targetUsername, m_helperClient);
    if (!id.valid) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - User not found:" << targetUsername;
        return false;
    }
    QString targetHome = id.home;
    
    if (m_helperClient && m_helperClient->isAvailable()
        && !m_helperClient->isSteamBootstrapped(targetUsername)) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - Target user" << targetUsername
                                   << "has not completed Steam setup. Launch Steam once first.";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Target user has not set up Steam"));
        return false;
    }
    QString targetSteamId = getTargetSteamUserId(targetUsername);
    if (targetSteamId.isEmpty()) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - Target user" << targetUsername
                                   << "has not set up Steam (no userdata found). Launch Steam once first.";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Target user has not set up Steam"));
        return false;
    }
    
    if (!captureLibraryFoldersSnapshot(targetUsername)) {
        return false;
    }
    QList<SteamLibraryFolder> targetLibraries;
    bool anyFailure = false;
    
    for (int i = 0; i < m_libraries.size(); ++i) {
        const SteamLibraryFolder &library = m_libraries[i];
        const QString sourceCommon = library.path + QStringLiteral("/steamapps/common");
        const QString sourceSteamApps = library.path + QStringLiteral("/steamapps");
        const QString targetLibPath =
            targetHome + QStringLiteral("/.couchplay/steam-libs/") + QString::number(i);
        const QString targetSteamApps = targetLibPath + QStringLiteral("/steamapps");
        const QString targetAlias =
            QStringLiteral(".couchplay/steam-libs/%1/steamapps/common").arg(QString::number(i));

        qCDebug(couchplaySteam) << "Setting ACL on library content" << sourceCommon << "for" << targetUsername;
        const bool parentAclOk = m_helperClient->setPathAclWithParents(sourceCommon, targetUsername);
        const bool contentAclOk = m_helperClient->setDirectoryAcl(sourceCommon, targetUsername, true);
        if (!parentAclOk || !contentAclOk) {
            qCWarning(couchplaySteam) << "Failed to set recursive ACL on" << sourceCommon;
            anyFailure = true;
        }

        qCDebug(couchplaySteam) << "Mounting library content" << sourceCommon << "at" << targetAlias
                                << "for" << targetUsername;
        if (!m_helperClient->setupOverlayMount(targetUsername, sourceCommon, targetAlias)) {
            qCWarning(couchplaySteam) << "Failed to mount library content" << sourceCommon;
            anyFailure = true;
        }

        QDir sourceSteamAppsDir(sourceSteamApps);
        const QStringList manifests = sourceSteamAppsDir.entryList({QStringLiteral("appmanifest_*.acf")}, QDir::Files);

        for (const QString &manifest : manifests) {
            const QString manifestPath = sourceSteamApps + QLatin1Char('/') + manifest;
            QFile manifestFile(manifestPath);
            if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QByteArray content = manifestFile.readAll();
                manifestFile.close();

                const QString targetManifestPath = targetSteamApps + QLatin1Char('/') + manifest;
                if (!m_helperClient->writeFileToUser(content, targetManifestPath, targetUsername)) {
                    qCWarning(couchplaySteam) << "Failed to write manifest" << manifest << "for" << targetUsername;
                    anyFailure = true;
                }
            }
        }

        // Target path + source metadata (app IDs, label, size) so Steam recognizes installed games
        SteamLibraryFolder targetLib;
        targetLib.path = targetLibPath;
        targetLib.label = library.label;
        targetLib.totalSize = library.totalSize;
        targetLib.appIds = library.appIds;
        targetLibraries.append(targetLib);
    }

    // Preserve the player's own Steam installation as the default library.
    SteamLibraryFolder ownRoot;
    ownRoot.path = targetPaths.steamRoot;
    targetLibraries.prepend(ownRoot);
    
    QString vdfContent = generateLibraryFoldersVdf(targetLibraries);
    
    qCDebug(couchplaySteam) << "Writing libraryfolders.vdf to" << targetPaths.libraryFoldersVdf;
    if (!m_helperClient->writeFileToUser(vdfContent.toUtf8(), targetPaths.libraryFoldersVdf, targetUsername)) {
        qCWarning(couchplaySteam) << "Failed to write libraryfolders.vdf for" << targetUsername;
        return false;
    }
    
    if (anyFailure) {
        qCWarning(couchplaySteam) << "Partially failed sharing Steam library to" << targetUsername;
    } else {
        qCDebug(couchplaySteam) << "Successfully shared" << m_libraries.size() << "Steam libraries to" << targetUsername;
    }
    
    return !anyFailure;
}

bool SteamConfigManager::cleanupLibrarySharing(const QString &targetUsername)
{
    if (!m_helperClient || !m_helperClient->isAvailable()) {
        return false;
    }

    auto snapshot = m_libraryFoldersSnapshots.constFind(targetUsername);
    if (snapshot == m_libraryFoldersSnapshots.cend()) return true;
    if (!m_helperClient->restoreSteamLibraryFoldersForUser(targetUsername,
                                                           snapshot->existed,
                                                           snapshot->content)) {
        qCWarning(couchplaySteam) << "Failed to restore target libraryfolders.vdf for" << targetUsername;
        return false;
    }
    m_libraryFoldersSnapshots.remove(targetUsername);
    qCDebug(couchplaySteam) << "Restored target libraryfolders.vdf for" << targetUsername;
    return true;
}

bool SteamConfigManager::prepareDataDir(const DataDirectory &dir,
                                        const QString &username,
                                        std::function<bool()> shouldContinue)
{
    const auto canContinue = [&shouldContinue] { return !shouldContinue || shouldContinue(); };
    if (!canContinue()) {
        return false;
    }

    // Library sharing: overlay mode on steamRoot
    if (dir.mode == QStringLiteral("overlay") && !m_steamPaths.steamRoot.isEmpty()
        && dir.path == m_steamPaths.steamRoot) {
        if (!m_steamPaths.valid) {
            qCWarning(couchplaySteam) << "prepareDataDir: Steam not detected";
            return false;
        }

        if (m_libraries.isEmpty()) {
            loadLibraryFolders();
        }
        if (!canContinue()) {
            return false;
        }
        if (m_libraries.isEmpty()) {
            qCWarning(couchplaySteam) << "prepareDataDir: No Steam libraries loaded";
            return false;
        }
        if (!m_helperClient || !m_helperClient->isAvailable()) {
            qCWarning(couchplaySteam) << "prepareDataDir: Helper not available";
            return false;
        }

        bool anyFailure = false;
        for (const SteamLibraryFolder &library : m_libraries) {
            if (!canContinue()) {
                return false;
            }
            const QString sourceCommon = library.path + QStringLiteral("/steamapps/common");
            qCDebug(couchplaySteam) << "prepareDataDir: Setting ACL on" << sourceCommon << "for" << username;
            const bool parentAclOk = m_helperClient->setPathAclWithParents(sourceCommon, username);
            if (!canContinue()) {
                return false;
            }
            const bool contentAclOk = m_helperClient->setDirectoryAcl(sourceCommon, username, true);
            if (!canContinue()) {
                return false;
            }
            if (!parentAclOk || !contentAclOk) {
                qCWarning(couchplaySteam) << "prepareDataDir: Failed to set recursive ACL on" << sourceCommon;
                anyFailure = true;
            }
        }

        // Mount only game content. The library root can contain the
        // compositor's Steam account, userdata, and configuration, none of
        // which should be exposed to a player.
        bool mountedAnyLibrary = false;
        const auto rollbackMounts = [&] {
            if (mountedAnyLibrary && m_helperClient && m_helperClient->isAvailable()) {
                m_helperClient->unmountAllSharedDirectories();
                mountedAnyLibrary = false;
            }
        };
        for (int i = 0; i < m_libraries.size(); ++i) {
            if (!canContinue()) {
                rollbackMounts();
                return false;
            }
            const QString sourceCommon = m_libraries[i].path + QStringLiteral("/steamapps/common");
            const QString alias =
                QStringLiteral(".couchplay/steam-libs/%1/steamapps/common").arg(QString::number(i));
            qCDebug(couchplaySteam) << "prepareDataDir: Overlaying" << sourceCommon << "at" << alias << "for" << username;
            const bool mounted = m_helperClient->setupOverlayMount(username, sourceCommon, alias);
            if (mounted) {
                mountedAnyLibrary = true;
            }
            if (!canContinue()) {
                rollbackMounts();
                return false;
            }
            if (!mounted) {
                qCWarning(couchplaySteam) << "prepareDataDir: Failed to mount library content" << sourceCommon;
                anyFailure = true;
            }
        }
        return !anyFailure;
    }

    return true;
}

bool SteamConfigManager::finalizeDataDir(const DataDirectory &dir, const QString &username)
{
    // Library sharing: overlay mode on steamRoot
    if (dir.mode == QStringLiteral("overlay") && !m_steamPaths.steamRoot.isEmpty()
        && dir.path == m_steamPaths.steamRoot) {
        if (!m_helperClient || !m_helperClient->isAvailable()) {
            return false;
        }
        if (m_libraries.isEmpty()) {
            qCWarning(couchplaySteam) << "finalizeDataDir: No libraries loaded";
            return false;
        }

        SteamPaths targetPaths = getTargetSteamPaths(username);
        if (!targetPaths.valid) {
            qCWarning(couchplaySteam) << "finalizeDataDir: Could not resolve target paths for" << username;
            return false;
        }

        // Resolve through the helper like every other identity lookup here:
        // the Flatpak sandbox cannot see CouchPlay-created host accounts, so a
        // process-local getpwnam() would abort finalization even though
        // preparation already mounted everything
        const UserIdentity targetIdentity = resolveUserIdentity(username, m_helperClient);
        if (!targetIdentity.valid) {
            qCWarning(couchplaySteam) << "finalizeDataDir: Could not resolve target user:" << username;
            return false;
        }
        QString targetHome = targetIdentity.home;
        QString targetSteamId = getTargetSteamUserId(username);
        if (targetSteamId.isEmpty()) {
            qCWarning(couchplaySteam) << "finalizeDataDir: Target user has not set up Steam:" << username;
            return false;
        }
        if (!captureLibraryFoldersSnapshot(username)) {
            return false;
        }
        QList<SteamLibraryFolder> targetLibraries;
        bool anyFailure = false;

        for (int i = 0; i < m_libraries.size(); ++i) {
            const SteamLibraryFolder &library = m_libraries[i];
            QString sourceSteamApps = library.path + QStringLiteral("/steamapps");

            // Every library lives at its alias mount under the player's home —
            // matches prepareDataDir's mounts and keeps the player's own Steam
            // root (and thus their identity and userdata) out of the sharing path
            QString targetLibPath = targetHome + QStringLiteral("/.couchplay/steam-libs/") + QString::number(i);
            QString targetSteamApps = targetLibPath + QStringLiteral("/steamapps");

            QDir sourceSteamAppsDir(sourceSteamApps);
            QStringList manifests =
                sourceSteamAppsDir.entryList({QStringLiteral("appmanifest_*.acf")}, QDir::Files);

            for (const QString &manifest : manifests) {
                QString manifestPath = sourceSteamApps + QLatin1Char('/') + manifest;
                QFile manifestFile(manifestPath);
                if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QByteArray content = manifestFile.readAll();
                    manifestFile.close();

                    QString targetManifestPath = targetSteamApps + QLatin1Char('/') + manifest;
                    if (!m_helperClient->writeFileToUser(content, targetManifestPath, username)) {
                        qCWarning(couchplaySteam) << "finalizeDataDir: Failed to write manifest" << manifest;
                        anyFailure = true;
                    }
                }
            }

            SteamLibraryFolder targetLib;
            targetLib.path = targetLibPath;
            targetLib.label = library.label;
            targetLib.totalSize = library.totalSize;
            targetLib.appIds = library.appIds;
            targetLibraries.append(targetLib);
        }

        // Keep the player's own Steam root as the first library so their own
        // installed content and default install location survive sharing
        SteamLibraryFolder ownRoot;
        ownRoot.path = targetPaths.steamRoot;
        targetLibraries.prepend(ownRoot);

        QString vdfContent = generateLibraryFoldersVdf(targetLibraries);
        if (!m_helperClient->writeFileToUser(vdfContent.toUtf8(), targetPaths.libraryFoldersVdf, username)) {
            qCWarning(couchplaySteam) << "finalizeDataDir: Failed to write libraryfolders.vdf for" << username;
            return false;
        }

        if (!anyFailure) {
            qCDebug(couchplaySteam) << "finalizeDataDir: Shared" << m_libraries.size() << "libraries to" << username;
        }
        return !anyFailure;
    }

    return true;
}
