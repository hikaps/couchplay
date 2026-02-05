// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "HeroicConfigManager.h"
#include "Logging.h"

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
    
    Q_EMIT heroicPathsChanged();
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
