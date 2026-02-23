// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "HeroicConfigManager.h"
#include "Logging.h"
#include "../dbus/CouchPlayHelperClient.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

#include <pwd.h>
#include <unistd.h>

HeroicConfigManager::HeroicConfigManager(QObject *parent)
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
    
    // Auto-detect Heroic on construction
    detectHeroicPaths();
}

void HeroicConfigManager::detectHeroicPaths()
{
    m_heroicPaths = HeroicPaths();
    
    // Check for Flatpak installation first
    QString flatpakPath = m_userHome + QStringLiteral("/.var/app/com.heroicgameslauncher.hgl/config/heroic");
    if (QDir(flatpakPath).exists()) {
        m_heroicPaths.heroicRoot = flatpakPath;
        m_heroicPaths.isFlatpak = true;
        qDebug() << "HeroicConfigManager: Detected Flatpak Heroic at" << flatpakPath;
    }
    // Check for native installation
    else {
        QString nativePath = m_userHome + QStringLiteral("/.config/heroic");
        if (QDir(nativePath).exists()) {
            m_heroicPaths.heroicRoot = nativePath;
            m_heroicPaths.isFlatpak = false;
            qDebug() << "HeroicConfigManager: Detected native Heroic at" << nativePath;
        }
    }
    
    if (m_heroicPaths.heroicRoot.isEmpty()) {
        qDebug() << "HeroicConfigManager: Heroic not detected";
        return;
    }
    
    // Set up all paths
    m_heroicPaths.configJson = m_heroicPaths.heroicRoot + QStringLiteral("/config.json");
    m_heroicPaths.gamesConfig = m_heroicPaths.heroicRoot + QStringLiteral("/GamesConfig");
    m_heroicPaths.toolsPath = m_heroicPaths.heroicRoot + QStringLiteral("/tools");
    
    // Legendary config (Epic Games)
    // Check for nested legendary config first (Flatpak structure)
    QString legendaryNested = m_heroicPaths.heroicRoot + QStringLiteral("/legendaryConfig/legendary/installed.json");
    if (QFile::exists(legendaryNested)) {
        m_heroicPaths.legendaryInstalled = legendaryNested;
    } else {
        // Try ~/.config/legendary for native installation
        QString legendaryStandalone = m_userHome + QStringLiteral("/.config/legendary/installed.json");
        if (QFile::exists(legendaryStandalone)) {
            m_heroicPaths.legendaryInstalled = legendaryStandalone;
        }
    }
    
    // GOG config
    m_heroicPaths.gogInstalled = m_heroicPaths.heroicRoot + QStringLiteral("/gog_store/installed.json");
    
    // Nile config (Amazon Games)
    m_heroicPaths.nileInstalled = m_heroicPaths.heroicRoot + QStringLiteral("/nile_config/installed.json");
    
    // Sideload (manually added games)
    m_heroicPaths.sideloadLibrary = m_heroicPaths.heroicRoot + QStringLiteral("/sideload_apps/library.json");
    
    // Verify at least config.json exists
    if (QFile::exists(m_heroicPaths.configJson)) {
        m_heroicPaths.valid = true;
        loadHeroicConfig();
        qDebug() << "HeroicConfigManager: Heroic installation valid";
    } else {
        qWarning() << "HeroicConfigManager: config.json not found, marking as invalid";
    }

    // Set shortcuts directory (standard XDG location where Heroic puts .desktop files)
    m_heroicPaths.shortcutsDir = m_userHome + QStringLiteral("/.local/share/applications");
    
    Q_EMIT heroicPathsChanged();
}

void HeroicConfigManager::setHelperClient(CouchPlayHelperClient *client)
{
    if (m_helperClient != client) {
        m_helperClient = client;
        Q_EMIT helperClientChanged();
    }
}

int HeroicConfigManager::generateShortcuts()
{
    if (!m_heroicPaths.valid) {
        qWarning() << "HeroicConfigManager: Heroic not detected, cannot generate shortcuts";
        return 0;
    }

    QDir shortcutsDir(m_heroicPaths.shortcutsDir);
    if (!shortcutsDir.exists()) {
        shortcutsDir.mkpath(QStringLiteral("."));
    }

    int generated = 0;
    QString launchCmd = m_heroicPaths.isFlatpak 
        ? QStringLiteral("flatpak run com.heroicgameslauncher.hgl --no-gui")
        : QStringLiteral("heroic --no-gui");

    for (const HeroicGame &game : m_games) {
        QString runner = game.runner;
        if (runner.isEmpty()) {
            runner = QStringLiteral("sideload");
        }

        QString desktopContent = QStringLiteral(
            "[Desktop Entry]\n"
            "Version=1.0\n"
            "Type=Application\n"
            "Name=%1\n"
            "Exec=%2 \"heroic://launch/%3/%4\"\n"
            "Icon=applications-games\n"
            "Categories=Game;\n"
            "StartupNotify=true\n"
        ).arg(game.title, launchCmd, runner, game.appName);

        QString filename = QStringLiteral("heroic-%1.desktop").arg(game.appName);
        QString filePath = shortcutsDir.absoluteFilePath(filename);

        QFile file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(desktopContent.toUtf8());
            file.close();
            generated++;
            qDebug() << "HeroicConfigManager: Generated shortcut" << filename;
        } else {
            qWarning() << "HeroicConfigManager: Failed to write shortcut" << filePath;
        }
    }

    qDebug() << "HeroicConfigManager: Generated" << generated << "shortcuts";
    return generated;
}

bool HeroicConfigManager::syncShortcutsToUser(const QString &targetUsername)
{
    qDebug() << "HeroicConfigManager: Syncing shortcuts to" << targetUsername;

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qWarning() << "HeroicConfigManager: Helper not available, cannot sync shortcuts";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Helper not available"));
        return false;
    }

    if (!m_heroicPaths.valid) {
        qWarning() << "HeroicConfigManager: Heroic not detected, cannot sync shortcuts";
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Heroic not detected"));
        return false;
    }

    QDir sourceDir(m_heroicPaths.shortcutsDir);
    if (!sourceDir.exists()) {
        sourceDir.mkpath(QStringLiteral("."));
    }

    if (m_games.isEmpty()) {
        loadGames();
    }
    
    int generated = generateShortcuts();
    qDebug() << "HeroicConfigManager: Generated" << generated << "shortcuts from Heroic games";

    QStringList filters;
    filters << QStringLiteral("heroic-*.desktop");
    QStringList shortcuts = sourceDir.entryList(filters, QDir::Files);
    
    if (shortcuts.isEmpty()) {
        qDebug() << "HeroicConfigManager: No shortcuts to sync";
        return true;
    }

    struct passwd *pw = getpwnam(targetUsername.toLocal8Bit().constData());
    if (!pw) {
        qWarning() << "HeroicConfigManager: User not found:" << targetUsername;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("User not found"));
        return false;
    }
    QString targetHome = QString::fromLocal8Bit(pw->pw_dir);
    QString targetDir = targetHome + QStringLiteral("/.local/share/applications");

    if (!m_helperClient->createUserDirectory(targetDir, targetUsername)) {
        qWarning() << "HeroicConfigManager: Failed to create target directory:" << targetDir;
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Failed to create target directory"));
        return false;
    }

    bool allSucceeded = true;
    for (const QString &filename : shortcuts) {
        QString sourcePath = sourceDir.absoluteFilePath(filename);
        QString targetPath = targetDir + QLatin1Char('/') + filename;

        qDebug() << "HeroicConfigManager: Copying" << filename << "to" << targetUsername;
        
        if (!m_helperClient->copyFileToUser(sourcePath, targetPath, targetUsername)) {
            qWarning() << "HeroicConfigManager: Failed to copy" << filename;
            allSucceeded = false;
        }
    }

    if (allSucceeded) {
        Q_EMIT syncCompleted(targetUsername);
    } else {
        Q_EMIT syncFailed(targetUsername, QStringLiteral("Failed to sync some shortcuts"));
    }

    return allSucceeded;
}

bool HeroicConfigManager::syncConfigToUser(const QString &targetUsername)
{
    qDebug() << "HeroicConfigManager: Syncing library to" << targetUsername;

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qWarning() << "HeroicConfigManager: Helper not available, cannot sync config";
        return false;
    }

    if (!m_heroicPaths.valid) {
        qWarning() << "HeroicConfigManager: Heroic not detected, cannot sync config";
        return false;
    }

    struct passwd *pw = getpwnam(targetUsername.toLocal8Bit().constData());
    if (!pw) {
        qWarning() << "HeroicConfigManager: User not found:" << targetUsername;
        return false;
    }
    QString targetHome = QString::fromLocal8Bit(pw->pw_dir);
    
    QString targetHeroicRoot;
    if (m_heroicPaths.isFlatpak) {
        targetHeroicRoot = targetHome + QStringLiteral("/.var/app/com.heroicgameslauncher.hgl/config/heroic");
    } else {
        targetHeroicRoot = targetHome + QStringLiteral("/.config/heroic");
    }

    auto copyFile = [this, &targetUsername, &targetHeroicRoot](const QString &sourcePath, const QString &relTargetPath) {
        if (!QFile::exists(sourcePath)) {
            return;
        }
        QString targetPath = targetHeroicRoot + QLatin1Char('/') + relTargetPath;
        qDebug() << "HeroicConfigManager: Copying" << relTargetPath;
        m_helperClient->copyFileToUser(sourcePath, targetPath, targetUsername);
    };

    copyFile(m_heroicPaths.sideloadLibrary, QStringLiteral("sideload_apps/library.json"));
    
    if (!m_heroicPaths.legendaryInstalled.isEmpty()) {
        copyFile(m_heroicPaths.legendaryInstalled, QStringLiteral("legendaryConfig/legendary/installed.json"));
    }
    
    copyFile(m_heroicPaths.gogInstalled, QStringLiteral("gog_store/installed.json"));
    copyFile(m_heroicPaths.nileInstalled, QStringLiteral("nile_config/installed.json"));

    qDebug() << "HeroicConfigManager: Library sync completed for" << targetUsername;
    return true;
}

QString HeroicConfigManager::heroicCommand() const
{
    if (!m_heroicPaths.valid) {
        return QStringLiteral("heroic");
    }
    
    if (m_heroicPaths.isFlatpak) {
        return QStringLiteral("flatpak run com.heroicgameslauncher.hgl");
    } else {
        return QStringLiteral("heroic");
    }
}

void HeroicConfigManager::loadHeroicConfig()
{
    if (!QFile::exists(m_heroicPaths.configJson)) {
        return;
    }
    
    QFile file(m_heroicPaths.configJson);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "HeroicConfigManager: Failed to open config.json";
        return;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "HeroicConfigManager: Failed to parse config.json:" << error.errorString();
        return;
    }
    
    QJsonObject root = doc.object();
    QJsonObject defaultSettings = root[QStringLiteral("defaultSettings")].toObject();
    
    // Extract default paths
    m_defaultInstallPath = defaultSettings[QStringLiteral("defaultInstallPath")].toString();
    
    qDebug() << "HeroicConfigManager: Default install path:" << m_defaultInstallPath;
}

QString HeroicConfigManager::defaultInstallPath() const
{
    return m_defaultInstallPath;
}



void HeroicConfigManager::loadGames()
{
    m_games.clear();
    
    if (!m_heroicPaths.valid) {
        qDebug() << "HeroicConfigManager: Cannot load games - Heroic not detected";
        Q_EMIT gamesLoaded();
        return;
    }
    
    // Load games from all backends
    m_games.append(parseLegendaryGames());
    m_games.append(parseGogGames());
    m_games.append(parseNileGames());
    m_games.append(parseSideloadGames());
    
    qDebug() << "HeroicConfigManager: Loaded" << m_games.size() << "total games";
    Q_EMIT gamesLoaded();
}

QList<HeroicGame> HeroicConfigManager::parseLegendaryGames()
{
    QList<HeroicGame> games;
    
    if (!QFile::exists(m_heroicPaths.legendaryInstalled)) {
        qDebug() << "HeroicConfigManager: Legendary installed.json not found";
        return games;
    }
    
    QFile file(m_heroicPaths.legendaryInstalled);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "HeroicConfigManager: Failed to open Legendary installed.json";
        return games;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "HeroicConfigManager: Failed to parse Legendary installed.json:" << error.errorString();
        return games;
    }
    
    QJsonObject root = doc.object();
    
    // Iterate over all games in the installed.json
    for (auto it = root.begin(); it != root.end(); ++it) {
        QString appName = it.key();
        QJsonObject gameObj = it.value().toObject();
        
        HeroicGame game;
        game.appName = appName;
        game.title = gameObj[QStringLiteral("title")].toString();
        game.installPath = gameObj[QStringLiteral("install_path")].toString();
        game.executable = gameObj[QStringLiteral("executable")].toString();
        game.installSize = gameObj[QStringLiteral("install_size")].toVariant().toLongLong();
        game.runner = QStringLiteral("legendary");
        
        games.append(game);
    }
    
    qDebug() << "HeroicConfigManager: Loaded" << games.size() << "Legendary games";
    return games;
}

QList<HeroicGame> HeroicConfigManager::parseGogGames()
{
    QList<HeroicGame> games;
    
    if (!QFile::exists(m_heroicPaths.gogInstalled)) {
        qDebug() << "HeroicConfigManager: GOG installed.json not found";
        return games;
    }
    
    QFile file(m_heroicPaths.gogInstalled);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "HeroicConfigManager: Failed to open GOG installed.json";
        return games;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "HeroicConfigManager: Failed to parse GOG installed.json:" << error.errorString();
        return games;
    }
    
    QJsonObject root = doc.object();
    QJsonArray installedArray = root[QStringLiteral("installed")].toArray();
    
    for (const QJsonValue &value : installedArray) {
        QJsonObject gameObj = value.toObject();
        
        HeroicGame game;
        game.appName = gameObj[QStringLiteral("appName")].toString();
        game.title = gameObj[QStringLiteral("title")].toString();
        game.installPath = gameObj[QStringLiteral("install_path")].toString();
        game.executable = gameObj[QStringLiteral("executable")].toString();
        game.installSize = gameObj[QStringLiteral("install_size")].toVariant().toLongLong();
        game.runner = QStringLiteral("gog");
        games.append(game);
    }
    
    qDebug() << "HeroicConfigManager: Loaded" << games.size() << "GOG games";
    return games;
}

QList<HeroicGame> HeroicConfigManager::parseNileGames()
{
    QList<HeroicGame> games;
    
    if (!QFile::exists(m_heroicPaths.nileInstalled)) {
        qDebug() << "HeroicConfigManager: Nile installed.json not found (Amazon Games)";
        return games;
    }
    
    QFile file(m_heroicPaths.nileInstalled);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "HeroicConfigManager: Failed to open Nile installed.json";
        return games;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "HeroicConfigManager: Failed to parse Nile installed.json:" << error.errorString();
        return games;
    }
    
    QJsonObject root = doc.object();
    QJsonArray installedArray = root[QStringLiteral("installed")].toArray();
    
    for (const QJsonValue &value : installedArray) {
        QJsonObject gameObj = value.toObject();
        
        HeroicGame game;
        game.appName = gameObj[QStringLiteral("id")].toString();
        game.title = gameObj[QStringLiteral("title")].toString();
        game.installPath = gameObj[QStringLiteral("install_path")].toString();
        game.executable = gameObj[QStringLiteral("executable")].toString();
        game.installSize = gameObj[QStringLiteral("install_size")].toVariant().toLongLong();
        game.runner = QStringLiteral("nile");
        games.append(game);
    }
    
    qDebug() << "HeroicConfigManager: Loaded" << games.size() << "Nile (Amazon) games";
    return games;
}

QVariantList HeroicConfigManager::gamesAsVariant() const
{
    QVariantList list;
    for (const HeroicGame &game : m_games) {
        QVariantMap map;
        map[QStringLiteral("appName")] = game.appName;
        map[QStringLiteral("title")] = game.title;
        map[QStringLiteral("installPath")] = game.installPath;
        map[QStringLiteral("executable")] = game.executable;
        map[QStringLiteral("runner")] = game.runner;
        map[QStringLiteral("installSize")] = game.installSize;
        list.append(map);
    }
    return list;
}

QStringList HeroicConfigManager::extractGameDirectories() const
{
    QSet<QString> dirs;
    
    for (const HeroicGame &game : m_games) {
        if (!game.installPath.isEmpty() && QDir(game.installPath).exists()) {
            dirs.insert(game.installPath);
        }
    }
    
    // Remove empty strings
    dirs.remove(QString());
    
    QStringList result = dirs.values();
    qDebug() << "HeroicConfigManager: Extracted" << result.size() << "game directories";
    return result;
}



QList<HeroicGame> HeroicConfigManager::parseSideloadGames()
{
    QList<HeroicGame> games;
    
    if (!QFile::exists(m_heroicPaths.sideloadLibrary)) {
        qDebug() << "HeroicConfigManager: Sideload library.json not found";
        return games;
    }
    
    QFile file(m_heroicPaths.sideloadLibrary);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "HeroicConfigManager: Failed to open sideload library.json";
        return games;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "HeroicConfigManager: Failed to parse sideload library.json:" << error.errorString();
        return games;
    }
    
    QJsonObject root = doc.object();
    QJsonArray gamesArray = root[QStringLiteral("games")].toArray();
    
    for (const QJsonValue &value : gamesArray) {
        QJsonObject gameObj = value.toObject();
        
        HeroicGame game;
        game.appName = gameObj[QStringLiteral("app_name")].toString();
        game.title = gameObj[QStringLiteral("title")].toString();
        game.runner = QStringLiteral("sideload");
        
        QJsonObject installObj = gameObj[QStringLiteral("install")].toObject();
        game.executable = installObj[QStringLiteral("executable")].toString();
        game.installPath = gameObj[QStringLiteral("folder_name")].toString();
        games.append(game);
    }
    
    qDebug() << "HeroicConfigManager: Loaded" << games.size() << "sideload games";
    return games;
}
