// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <qqmlintegration.h>

/**
 * HeroicPaths - Detected Heroic installation paths
 */
struct HeroicPaths {
    Q_GADGET
    Q_PROPERTY(QString heroicRoot MEMBER heroicRoot)
    Q_PROPERTY(QString configJson MEMBER configJson)
    Q_PROPERTY(QString legendaryInstalled MEMBER legendaryInstalled)
    Q_PROPERTY(QString gogInstalled MEMBER gogInstalled)
    Q_PROPERTY(QString nileInstalled MEMBER nileInstalled)
    Q_PROPERTY(QString sideloadLibrary MEMBER sideloadLibrary)
    Q_PROPERTY(QString gamesConfig MEMBER gamesConfig)
    Q_PROPERTY(QString toolsPath MEMBER toolsPath)
    Q_PROPERTY(bool isFlatpak MEMBER isFlatpak)
    Q_PROPERTY(bool valid MEMBER valid)

public:
    QString heroicRoot;           // ~/.config/heroic or Flatpak equivalent
    QString configJson;           // config.json path
    QString legendaryInstalled;   // legendaryConfig/legendary/installed.json
    QString gogInstalled;         // gog_store/installed.json
    QString nileInstalled;        // nile_config/installed.json (Amazon)
    QString sideloadLibrary;      // sideload_apps/library.json (manually added)
    QString gamesConfig;          // GamesConfig/ directory
    QString toolsPath;            // tools/ directory
    bool isFlatpak = false;
    bool valid = false;
};

Q_DECLARE_METATYPE(HeroicPaths)

/**
 * HeroicGame - Represents an installed game from any Heroic backend
 */
struct HeroicGame {
    Q_GADGET
    Q_PROPERTY(QString appName MEMBER appName)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString installPath MEMBER installPath)
    Q_PROPERTY(QString executable MEMBER executable)
    Q_PROPERTY(QString runner MEMBER runner)
    Q_PROPERTY(qint64 installSize MEMBER installSize)

public:
    QString appName;              // Internal ID (e.g., "Egret")
    QString title;                // Display name
    QString installPath;          // Full path to game installation
    QString executable;           // Relative path to executable
    QString runner;               // "legendary", "gog", "nile"
    qint64 installSize = 0;
};

Q_DECLARE_METATYPE(HeroicGame)

/**
 * HeroicConfigManager - Manages Heroic Games Launcher integration
 * 
 * Handles detection of Heroic installation (native or Flatpak),
 * parsing of game configurations from Epic (Legendary), GOG, and Amazon (Nile),
 * and extraction of paths for ACL setup.
 */
class HeroicConfigManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    
    Q_PROPERTY(HeroicPaths heroicPaths READ heroicPaths NOTIFY heroicPathsChanged)
    Q_PROPERTY(bool heroicDetected READ isHeroicDetected NOTIFY heroicPathsChanged)
    Q_PROPERTY(bool isFlatpak READ isFlatpak NOTIFY heroicPathsChanged)
    Q_PROPERTY(int gameCount READ gameCount NOTIFY gamesLoaded)
    Q_PROPERTY(QString heroicCommand READ heroicCommand NOTIFY heroicPathsChanged)

public:
    explicit HeroicConfigManager(QObject *parent = nullptr);
    ~HeroicConfigManager() override = default;

    /**
     * @brief Detect Heroic installation paths
     * Checks for native installation and Flatpak
     */
    Q_INVOKABLE void detectHeroicPaths();

    /**
     * @brief Check if Heroic installation was detected
     */
    bool isHeroicDetected() const { return m_heroicPaths.valid; }

    /**
     * @brief Check if detected installation is Flatpak
     */
    bool isFlatpak() const { return m_heroicPaths.isFlatpak; }

    /**
     * @brief Get the command to launch Heroic
     * @return "heroic" for native, "flatpak run com.heroicgameslauncher.hgl" for Flatpak
     */
    QString heroicCommand() const;

    /**
     * @brief Get detected Heroic paths
     */
    HeroicPaths heroicPaths() const { return m_heroicPaths; }

    /**
     * @brief Get Heroic config root directory
     */
    QString configPath() const { return m_heroicPaths.heroicRoot; }

    /**
     * @brief Load installed games from all backends
     */
    Q_INVOKABLE void loadGames();

    /**
     * @brief Get list of installed games
     */
    QList<HeroicGame> installedGames() const { return m_games; }

    /**
     * @brief Get number of installed games
     */
    int gameCount() const { return m_games.size(); }

    /**
     * @brief Get installed games as QVariantList for QML
     */
    Q_INVOKABLE QVariantList gamesAsVariant() const;

    /**
     * @brief Extract unique game installation directories
     * Used for setting ACLs on game folders
     * @return List of unique directory paths
     */
    Q_INVOKABLE QStringList extractGameDirectories() const;

    /**
     * @brief Get default install path from Heroic config
     */
    QString defaultInstallPath() const;

Q_SIGNALS:
    void heroicPathsChanged();
    void gamesLoaded();
    void errorOccurred(const QString &message);

private:
    QList<HeroicGame> parseLegendaryGames();
    QList<HeroicGame> parseGogGames();
    QList<HeroicGame> parseNileGames();
    QList<HeroicGame> parseSideloadGames();
    
    /**
     * @brief Read Heroic's config.json for default settings
     */
    void loadHeroicConfig();

    HeroicPaths m_heroicPaths;
    QList<HeroicGame> m_games;
    QString m_userHome;
    QString m_defaultInstallPath;
};
