// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

struct SteamShortcut;

namespace SteamShortcutsVdf {

constexpr qsizetype MaxDocumentSize = 16 * 1024 * 1024;

bool decode(const QByteArray &bytes, QList<SteamShortcut> *shortcuts, QString *errorMessage = nullptr);
bool upsert(const QByteArray &bytes,
            const SteamShortcut &shortcut,
            QByteArray *result,
            QString *errorMessage = nullptr);
bool withoutProfiles(const QByteArray &bytes, QByteArray *result, QString *errorMessage = nullptr);
bool isProfileShortcut(const SteamShortcut &shortcut);

QByteArray emptyDocument();

} // namespace SteamShortcutsVdf
