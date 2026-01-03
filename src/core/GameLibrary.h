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
     * @param command Launch command (steam://rungameid/XXX or executable path)
     * @param iconPath Optional icon path
     * @return true if added successfully
     */
    Q_INVOKABLE bool addGame(const QString &name, const QString &command, const QString &iconPath = QString());

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
        QString command;
        QString iconPath;
    };

    QList<GameInfo> m_games;
};
