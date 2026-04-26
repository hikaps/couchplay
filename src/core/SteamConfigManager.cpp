// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SteamConfigManager.h"
#include "../dbus/CouchPlayHelperClient.h"
#include "Logging.h"

#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
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

    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    if (!pw) {
        qWarning() << "SteamConfigManager: User not found:" << username;
        return QString();
    }

    QString targetHome = QString::fromLocal8Bit(pw->pw_dir);

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

    m_shortcuts = parseShortcutsVdf(sourceFile);
    qCDebug(couchplaySteam) << "Loaded" << m_shortcuts.size() << "shortcuts from" << sourceFile;

    Q_EMIT shortcutsLoaded();
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
        // Extract exe directory
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

bool SteamConfigManager::syncShortcutsToUser(const QString &targetUsername)
{
    qCDebug(couchplaySteam) << "syncShortcutsToUser called for" << targetUsername;

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Helper not available";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Helper not available"));
        return false;
    }

    // Get source shortcuts.vdf path
    if (!m_steamPaths.valid || m_steamPaths.shortcutsVdf.isEmpty()) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Steam not detected";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Steam not detected"));
        return false;
    }

    QString sourceFile = m_steamPaths.shortcutsVdf;
    if (!QFile::exists(sourceFile)) {
        qCDebug(couchplaySteam) << "No shortcuts.vdf to sync";
        return true; // Not an error, just nothing to do
    }

    qCDebug(couchplaySteam) << "Source file:" << sourceFile;

    // Get target user's Steam ID
    QString targetSteamId = getTargetSteamUserId(targetUsername);
    if (targetSteamId.isEmpty()) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Steam not set up for user" << targetUsername;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Steam not set up for user (run Steam once first)"));
        return false;
    }
    qCDebug(couchplaySteam) << "Target Steam ID:" << targetSteamId;

    // Get target user's home
    struct passwd *pw = getpwnam(targetUsername.toLocal8Bit().constData());
    if (!pw) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - User not found:" << targetUsername;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("User not found"));
        return false;
    }
    QString targetHome = QString::fromLocal8Bit(pw->pw_dir);
    qCDebug(couchplaySteam) << "Target home:" << targetHome;

    // Target path uses TARGET user's Steam ID (not compositor's)
    // Check for Steam installation location
    QString targetSteamRoot;
    QStringList possibleRoots = {
        targetHome + QStringLiteral("/.steam/steam"),
        targetHome + QStringLiteral("/.local/share/Steam"),
    };
    for (const QString &root : possibleRoots) {
        if (QDir(root).exists()) {
            targetSteamRoot = root;
            break;
        }
    }
    if (targetSteamRoot.isEmpty()) {
        targetSteamRoot = targetHome + QStringLiteral("/.steam/steam");
    }

    QString targetConfigDir =
        targetSteamRoot + QStringLiteral("/userdata/") + targetSteamId + QStringLiteral("/config");
    QString targetVdf = targetConfigDir + QStringLiteral("/shortcuts.vdf");

    // Safety: ensure target path is under the user's home directory
    if (!targetVdf.startsWith(targetHome + QLatin1Char('/'))) {
        qCWarning(couchplaySteam) << "syncShortcutsToUser: Target path" << targetVdf << "is not under user's home"
                                  << targetHome;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Target path is not under user's home directory"));
        return false;
    }

    // Direct byte copy - preserves exact Steam format including all end markers
    // This is the preferred approach as it avoids any serialization differences
    QFile sourceFileHandle(sourceFile);
    if (!sourceFileHandle.open(QIODevice::ReadOnly)) {
        qCWarning(couchplaySteam) << "Failed to open source file:" << sourceFile;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Failed to open source shortcuts.vdf"));
        return false;
    }

    QByteArray vdfData = sourceFileHandle.readAll();
    sourceFileHandle.close();

    qCDebug(couchplaySteam) << "Read" << vdfData.size() << "bytes from source, writing directly to" << targetVdf;

    // Write directly to target user via helper (avoids PrivateTmp issues)
    bool success = m_helperClient->writeFileToUser(vdfData, targetVdf, targetUsername);

    if (success) {
        qCDebug(couchplaySteam) << "Synced shortcuts to" << targetUsername;
        Q_EMIT syncCompleted(targetUsername);
    } else {
        qCWarning(couchplaySteam) << "syncShortcutsToUser failed - Failed to write shortcuts.vdf to" << targetVdf;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Failed to write shortcuts.vdf"));
    }

    return success;
}

// Get target Steam paths for a user (uses target user's Steam ID)
SteamPaths SteamConfigManager::getTargetSteamPaths(const QString &username) const
{
    SteamPaths paths;

    // Get target user's home directory
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    if (!pw) {
        qWarning() << "SteamConfigManager: User not found:" << username;
        return paths;
    }

    QString targetHome = QString::fromLocal8Bit(pw->pw_dir);

    // Check for Steam in common locations relative to target home
    QStringList possibleRoots = {
        targetHome + QStringLiteral("/.steam/steam"),
        targetHome + QStringLiteral("/.local/share/Steam"),
    };

    for (const QString &root : possibleRoots) {
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

// ============================================================================
// Binary VDF Parsing
// ============================================================================

// Binary VDF type markers
constexpr char VDF_TYPE_OBJECT = 0x00;
constexpr char VDF_TYPE_STRING = 0x01;
constexpr char VDF_TYPE_INT32 = 0x02;
constexpr char VDF_TYPE_END = 0x08;

QList<SteamShortcut> SteamConfigManager::parseShortcutsVdf(const QString &path)
{
    QList<SteamShortcut> result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "SteamConfigManager: Failed to open" << path;
        return result;
    }

    QByteArray data = file.readAll();
    file.close();

    if (data.isEmpty()) {
        return result;
    }

    // Parse binary VDF
    int pos = 0;

    // Read root object marker and "shortcuts" key
    if (pos >= data.size() || data[pos] != VDF_TYPE_OBJECT) {
        qWarning() << "SteamConfigManager: Invalid VDF format - expected object marker";
        return result;
    }
    pos++;

    // Read "shortcuts" string
    QString rootKey;
    while (pos < data.size() && data[pos] != '\0') {
        rootKey += QChar::fromLatin1(data[pos]);
        pos++;
    }
    pos++; // Skip null terminator

    if (rootKey != QStringLiteral("shortcuts")) {
        qWarning() << "SteamConfigManager: Unexpected root key:" << rootKey;
        return result;
    }

    // Now parse each shortcut (numbered 0, 1, 2, ...)
    while (pos < data.size()) {
        if (data[pos] == VDF_TYPE_END) {
            break; // End of shortcuts object
        }

        if (data[pos] != VDF_TYPE_OBJECT) {
            break; // Unexpected type
        }
        pos++;

        // Read shortcut index key
        QString indexKey;
        while (pos < data.size() && data[pos] != '\0') {
            indexKey += QChar::fromLatin1(data[pos]);
            pos++;
        }
        pos++; // Skip null terminator

        SteamShortcut shortcut;

        // Parse shortcut properties
        while (pos < data.size()) {
            char type = data[pos];
            if (type == VDF_TYPE_END) {
                pos++;
                break; // End of this shortcut
            }
            pos++;

            // Read key
            QString key;
            while (pos < data.size() && data[pos] != '\0') {
                key += QChar::fromLatin1(data[pos]);
                pos++;
            }
            pos++; // Skip null terminator

            if (type == VDF_TYPE_STRING) {
                QString value;
                while (pos < data.size() && data[pos] != '\0') {
                    value += QChar::fromLatin1(data[pos]);
                    pos++;
                }
                pos++; // Skip null terminator

                if (key == QStringLiteral("AppName")) {
                    shortcut.appName = value;
                } else if (key == QStringLiteral("exe") || key == QStringLiteral("Exe")) {
                    shortcut.exe = value;
                } else if (key == QStringLiteral("StartDir")) {
                    shortcut.startDir = value;
                } else if (key == QStringLiteral("icon")) {
                    shortcut.icon = value;
                } else if (key == QStringLiteral("ShortcutPath")) {
                    shortcut.shortcutPath = value;
                } else if (key == QStringLiteral("LaunchOptions")) {
                    shortcut.launchOptions = value;
                } else if (key == QStringLiteral("DevkitGameID")) {
                    shortcut.devkitGameId = value;
                } else if (key == QStringLiteral("FlatpakAppID")) {
                    shortcut.flatpakAppId = value;
                } else if (key == QStringLiteral("sortas")) {
                    shortcut.sortAs = value;
                }
            } else if (type == VDF_TYPE_INT32) {
                if (pos + 4 > data.size())
                    break;
                quint32 value = 0;
                value |= static_cast<quint8>(data[pos++]);
                value |= static_cast<quint8>(data[pos++]) << 8;
                value |= static_cast<quint8>(data[pos++]) << 16;
                value |= static_cast<quint8>(data[pos++]) << 24;

                if (key == QStringLiteral("appid") || key == QStringLiteral("AppId")) {
                    shortcut.appId = value;
                } else if (key == QStringLiteral("IsHidden")) {
                    shortcut.isHidden = (value != 0);
                } else if (key == QStringLiteral("AllowDesktopConfig")) {
                    shortcut.allowDesktopConfig = (value != 0);
                } else if (key == QStringLiteral("AllowOverlay")) {
                    shortcut.allowOverlay = (value != 0);
                } else if (key == QStringLiteral("OpenVR")) {
                    shortcut.openVR = (value != 0);
                } else if (key == QStringLiteral("Devkit")) {
                    shortcut.devkit = (value != 0);
                } else if (key == QStringLiteral("DevkitOverrideAppID")) {
                    shortcut.devkitOverrideAppId = value;
                } else if (key == QStringLiteral("LastPlayTime")) {
                    shortcut.lastPlayTime = value;
                }
            } else if (type == VDF_TYPE_OBJECT) {
                // Handle nested objects like "tags"
                if (key == QStringLiteral("tags")) {
                    // Skip tags for now - just consume until end marker
                    while (pos < data.size() && data[pos] != VDF_TYPE_END) {
                        // Skip type marker
                        pos++;
                        // Skip key
                        while (pos < data.size() && data[pos] != '\0')
                            pos++;
                        pos++;
                        // Skip value based on type (assume string)
                        while (pos < data.size() && data[pos] != '\0')
                            pos++;
                        pos++;
                    }
                    if (pos < data.size())
                        pos++; // Skip end marker
                }
            }
        }

        result.append(shortcut);
    }

    return result;
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
    
    struct passwd *pw = getpwnam(targetUsername.toLocal8Bit().constData());
    if (!pw) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - User not found:" << targetUsername;
        return false;
    }
    QString targetHome = QString::fromLocal8Bit(pw->pw_dir);
    
    QString targetSteamId = getTargetSteamUserId(targetUsername);
    if (targetSteamId.isEmpty()) {
        qCWarning(couchplaySteam) << "shareLibraryToUser failed - Target user" << targetUsername
                                   << "has not set up Steam (no userdata found). Launch Steam once first.";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Target user has not set up Steam"));
        return false;
    }
    
    QStringList targetLibraryPaths;
    QList<SteamLibraryFolder> targetLibraries;
    bool anyFailure = false;
    
    for (int i = 0; i < m_libraries.size(); ++i) {
        const SteamLibraryFolder &library = m_libraries[i];
        
        QString sourceCommon = library.path + QStringLiteral("/steamapps/common");
        QString sourceSteamApps = library.path + QStringLiteral("/steamapps");
        
        QString targetLibPath;
        if (i == 0) {
            // Library 0 reuses the target's Steam root as the default install location
            targetLibPath = targetPaths.steamRoot;
        } else {
            targetLibPath = targetHome + QStringLiteral("/.couchplay/steam-libs/") + QString::number(i);
        }
        
        QString targetSteamApps = targetLibPath + QStringLiteral("/steamapps");
        
        qCDebug(couchplaySteam) << "Setting ACL on library" << library.path << "for" << targetUsername;
        if (!m_helperClient->setPathAclWithParents(library.path, targetUsername)) {
            qCWarning(couchplaySteam) << "Failed to set ACL on" << library.path;
            anyFailure = true;
        }
        
        // Mount spec "source|alias": empty alias (i==0) mounts at natural path,
        // explicit alias (i>0) mounts into .couchplay/steam-libs/<i>/steamapps/common
        QString mountSpec;
        if (i == 0) {
            mountSpec = sourceCommon + QStringLiteral("|");
        } else {
            mountSpec = sourceCommon + QStringLiteral("|/.couchplay/steam-libs/") + QString::number(i) + QStringLiteral("/steamapps/common");
        }
        
        qCDebug(couchplaySteam) << "Mounting" << mountSpec << "for" << targetUsername;
        int mountResult = m_helperClient->mountSharedDirectories(targetUsername, getuid(), {mountSpec});
        if (mountResult <= 0) {
            qCWarning(couchplaySteam) << "Failed to mount library" << i << "for" << targetUsername;
            anyFailure = true;
        }
        
        QDir sourceSteamAppsDir(sourceSteamApps);
        QStringList manifests = sourceSteamAppsDir.entryList({QStringLiteral("appmanifest_*.acf")}, QDir::Files);
        
        for (const QString &manifest : manifests) {
            QString manifestPath = sourceSteamApps + QLatin1Char('/') + manifest;
            QFile manifestFile(manifestPath);
            if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QByteArray content = manifestFile.readAll();
                manifestFile.close();
                
                QString targetManifestPath = targetSteamApps + QLatin1Char('/') + manifest;
                if (!m_helperClient->writeFileToUser(content, targetManifestPath, targetUsername)) {
                    qCWarning(couchplaySteam) << "Failed to write manifest" << manifest << "for" << targetUsername;
                    anyFailure = true;
                }
            }
        }
        
        targetLibraryPaths.append(targetLibPath);
        
        // Target path + source metadata (app IDs, label, size) so Steam recognizes installed games
        SteamLibraryFolder targetLib;
        targetLib.path = targetLibPath;
        targetLib.label = library.label;
        targetLib.totalSize = library.totalSize;
        targetLib.appIds = library.appIds;
        targetLibraries.append(targetLib);
    }
    
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

void SteamConfigManager::cleanupLibrarySharing(const QString &targetUsername)
{
    if (!m_helperClient || !m_helperClient->isAvailable()) {
        return;
    }

    SteamPaths targetPaths = getTargetSteamPaths(targetUsername);
    if (!targetPaths.valid) {
        return;
    }

    // Restore minimal libraryfolders.vdf — clears shared library entries
    // so Steam doesn't reference bind-mounted paths that no longer exist.
    QString emptyVdf = QStringLiteral("\"libraryfolders\"\n{\n}\n");
    m_helperClient->writeFileToUser(emptyVdf.toUtf8(), targetPaths.libraryFoldersVdf, targetUsername);

    qCDebug(couchplaySteam) << "Cleaned up library sharing for" << targetUsername;
}
