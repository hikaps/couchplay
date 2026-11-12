// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVariantMap>

#include <optional>

// Per-game compatibility tool choice (Proton version or Steam Linux Runtime) stored in
// InstallConfigStore/Software/Valve/Steam/CompatToolMapping of Steam's text config/config.vdf.
// The choice decides which platform's depots Steam installs, so players sharing the
// desktop's library must use the desktop's choice or their Steam re-downloads the game.
namespace SteamCompatToolMapping
{

struct Tool
{
    QString name;
    QString config;
    QString priority;

    bool operator==(const Tool &other) const = default;
};

// App ID -> tool; std::nullopt removes the app's entry
using Changes = QMap<QString, std::optional<Tool>>;

// Per-app entries of a config.vdf; empty when it has no mapping, std::nullopt when it is not parseable
std::optional<QMap<QString, Tool>> read(const QByteArray &configVdf);

// Sets or removes each changed app's entry and leaves every other byte untouched;
// std::nullopt when the document is not a parseable config.vdf with a Steam section
std::optional<QByteArray> apply(const QByteArray &configVdf, const Changes &changes);

// D-Bus form: app ID -> QStringList{name, config, priority}, or an empty list to remove
QVariantMap toVariantMap(const Changes &changes);
// std::nullopt for non-numeric app IDs, malformed tools or control characters
std::optional<Changes> fromVariantMap(const QVariantMap &map);

}
