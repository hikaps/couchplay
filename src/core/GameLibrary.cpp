// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "GameLibrary.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

GameLibrary::GameLibrary(QObject *parent)
    : QObject(parent)
{
    loadGames();
}

GameLibrary::~GameLibrary() = default;

void GameLibrary::refresh()
{
    loadGames();
    Q_EMIT gamesChanged();
}

QString GameLibrary::gamesConfigPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) 
           + QStringLiteral("/games.json");
}

void GameLibrary::loadGames()
{
    m_games.clear();

    QFile file(gamesConfigPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return; // No games file yet
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isArray()) {
        return;
    }

    QJsonArray array = doc.array();
    for (const QJsonValue &val : array) {
        QJsonObject obj = val.toObject();
        GameInfo game;
        game.name = obj[QStringLiteral("name")].toString();
        game.command = obj[QStringLiteral("command")].toString();
        game.iconPath = obj[QStringLiteral("iconPath")].toString();
        m_games.append(game);
    }
}

void GameLibrary::saveGames()
{
    QJsonArray array;
    for (const auto &game : m_games) {
        QJsonObject obj;
        obj[QStringLiteral("name")] = game.name;
        obj[QStringLiteral("command")] = game.command;
        obj[QStringLiteral("iconPath")] = game.iconPath;
        array.append(obj);
    }

    QJsonDocument doc(array);

    // Ensure directory exists
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));

    QFile file(gamesConfigPath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();
    }
}

bool GameLibrary::addGame(const QString &name, const QString &command, const QString &iconPath)
{
    if (name.isEmpty() || command.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Name and command are required"));
        return false;
    }

    // Check for duplicates
    for (const auto &game : m_games) {
        if (game.name == name) {
            Q_EMIT errorOccurred(QStringLiteral("Game already exists"));
            return false;
        }
    }

    GameInfo game;
    game.name = name;
    game.command = command;
    game.iconPath = iconPath;
    m_games.append(game);

    saveGames();
    Q_EMIT gamesChanged();
    return true;
}

bool GameLibrary::removeGame(const QString &name)
{
    for (int i = 0; i < m_games.size(); ++i) {
        if (m_games[i].name == name) {
            m_games.removeAt(i);
            saveGames();
            Q_EMIT gamesChanged();
            return true;
        }
    }

    Q_EMIT errorOccurred(QStringLiteral("Game not found"));
    return false;
}

bool GameLibrary::createDesktopShortcut(const QString &gameName, const QString &profileName)
{
    // Find the game
    QString gameCommand;
    QString iconPath;
    for (const auto &game : m_games) {
        if (game.name == gameName) {
            gameCommand = game.command;
            iconPath = game.iconPath;
            break;
        }
    }

    if (gameCommand.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Game not found"));
        return false;
    }

    // Create desktop file
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                          + QStringLiteral("/couchplay-%1-%2.desktop").arg(profileName, gameName);

    // Sanitize filename
    desktopPath.replace(QLatin1Char(' '), QLatin1Char('-'));

    QString desktopContent = QStringLiteral(
        "[Desktop Entry]\n"
        "Name=CouchPlay: %1 (%2)\n"
        "Comment=Launch %1 with CouchPlay profile %2\n"
        "Exec=couchplay --profile \"%2\" --game \"%3\"\n"
        "Icon=%4\n"
        "Type=Application\n"
        "Categories=Game;\n"
        "Terminal=false\n"
    ).arg(gameName, profileName, gameCommand, 
          iconPath.isEmpty() ? QStringLiteral("io.github.hikaps.couchplay") : iconPath);

    QFile file(desktopPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to create desktop shortcut"));
        return false;
    }

    file.write(desktopContent.toUtf8());
    file.close();

    // Make executable
    file.setPermissions(file.permissions() | QFileDevice::ExeUser);

    return true;
}

QVariantList GameLibrary::getSteamGames() const
{
    QVariantList games;

    // Try to find Steam library folders
    QStringList steamPaths = {
        QDir::homePath() + QStringLiteral("/.steam/steam"),
        QDir::homePath() + QStringLiteral("/.local/share/Steam"),
        QStringLiteral("/run/media/mmcblk0p1/steamapps") // Steam Deck SD card
    };

    for (const QString &steamPath : steamPaths) {
        QString appsPath = steamPath + QStringLiteral("/steamapps");
        QDir appsDir(appsPath);

        if (!appsDir.exists()) {
            continue;
        }

        // Read appmanifest files
        QStringList manifests = appsDir.entryList({QStringLiteral("appmanifest_*.acf")}, QDir::Files);

        for (const QString &manifest : manifests) {
            QFile file(appsDir.absoluteFilePath(manifest));
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }

            QString content = QString::fromUtf8(file.readAll());
            file.close();

            // Simple parsing of VDF format
            QString appId;
            QString name;

            QRegularExpression appIdRegex(QStringLiteral("\"appid\"\\s+\"(\\d+)\""));
            QRegularExpression nameRegex(QStringLiteral("\"name\"\\s+\"([^\"]+)\""));

            QRegularExpressionMatch appIdMatch = appIdRegex.match(content);
            QRegularExpressionMatch nameMatch = nameRegex.match(content);

            if (appIdMatch.hasMatch() && nameMatch.hasMatch()) {
                appId = appIdMatch.captured(1);
                name = nameMatch.captured(1);

                QVariantMap game;
                game[QStringLiteral("name")] = name;
                game[QStringLiteral("appId")] = appId;
                game[QStringLiteral("command")] = QStringLiteral("steam://rungameid/%1").arg(appId);
                games.append(game);
            }
        }
    }

    return games;
}

QVariantList GameLibrary::gamesAsVariant() const
{
    QVariantList list;
    for (const auto &game : m_games) {
        QVariantMap map;
        map[QStringLiteral("name")] = game.name;
        map[QStringLiteral("command")] = game.command;
        map[QStringLiteral("iconPath")] = game.iconPath;
        list.append(map);
    }
    return list;
}
