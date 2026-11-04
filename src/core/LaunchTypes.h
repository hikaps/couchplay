// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>

struct GameSelection
{
    Q_GADGET
    Q_PROPERTY(QString launcherId MEMBER launcherId)
    Q_PROPERTY(QString backend MEMBER backend)
    Q_PROPERTY(QString gameId MEMBER gameId)
    Q_PROPERTY(QString title MEMBER title)

public:
    QString launcherId;
    QString backend;
    QString gameId;
    QString title;

    bool isEmpty() const
    {
        return gameId.isEmpty();
    }

    QVariantMap toVariant() const
    {
        return {
            {QStringLiteral("launcherId"), launcherId},
            {QStringLiteral("backend"), backend},
            {QStringLiteral("gameId"), gameId},
            {QStringLiteral("title"), title},
        };
    }

    static GameSelection fromVariant(const QVariantMap &map)
    {
        GameSelection selection;
        selection.launcherId = map.value(QStringLiteral("launcherId")).toString();
        selection.backend = map.value(QStringLiteral("backend")).toString();
        selection.gameId = map.value(QStringLiteral("gameId")).toString();
        selection.title = map.value(QStringLiteral("title")).toString();
        return selection;
    }
};

Q_DECLARE_METATYPE(GameSelection)

struct LaunchCommand
{
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QString errorMessage;

    bool isValid() const
    {
        return !program.isEmpty() && errorMessage.isEmpty();
    }
};

Q_DECLARE_METATYPE(LaunchCommand)
