// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>
#include <qqmlintegration.h>

/**
 * @brief Manages game library and desktop shortcuts
 */
class GameLibrary : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList games READ gamesAsVariant NOTIFY gamesChanged)

public:
    explicit GameLibrary(QObject *parent = nullptr);
    ~GameLibrary() override;

    /**
     * @brief Refresh the game list
     */
    Q_INVOKABLE void refresh();

    /**
     * @brief Add a game to the library
     * @param name Display name
     * @param executablePath Path to game executable (.exe for Windows games, binary for native)
     * @param protonPath Path to Proton installation (required for Windows games)
     * @param prefixPath Path to Wine/Proton prefix directory
     * @param iconPath Optional icon path
     * @param steamAppId Steam App ID (for Steam launch mode)
     * @param launchMode Launch mode: "direct", "steam", "legendary", "custom"
     * @return true if added successfully
     */
    Q_INVOKABLE bool addGame(const QString &name, 
                             const QString &executablePath, 
                             const QString &protonPath = QString(),
                             const QString &prefixPath = QString(),
                             const QString &iconPath = QString(),
                             const QString &steamAppId = QString(),
                             const QString &launchMode = QStringLiteral("direct"));

    /**
     * @brief Remove a game from the library
     */
    Q_INVOKABLE bool removeGame(const QString &name);

    /**
     * @brief Create a desktop shortcut for a game with a specific profile
     */
    Q_INVOKABLE bool createDesktopShortcut(const QString &gameName, const QString &profileName);

    /**
     * @brief Get Steam games from Steam library (if Steam is installed)
     */
    Q_INVOKABLE QVariantList getSteamGames() const;

    QVariantList gamesAsVariant() const;

Q_SIGNALS:
    void gamesChanged();
    void errorOccurred(const QString &message);

private:
    void loadGames();
    void saveGames();
    QString gamesConfigPath() const;

    struct GameInfo {
        QString name;
        QString executablePath;  // Path to .exe or native binary
        QString protonPath;      // Path to Proton (empty for native games)
        QString prefixPath;      // Wine/Proton prefix directory
        QString iconPath;
        QString steamAppId;      // Steam App ID (e.g., "1293160" for It Takes Two)
        QString launchMode;      // "direct", "steam", "legendary", "custom"
    };

    QList<GameInfo> m_games;
};
