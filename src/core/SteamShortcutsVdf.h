// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QString>

struct SteamShortcut;

namespace SteamShortcutsVdf {

constexpr qsizetype MaxDocumentSize = 16 * 1024 * 1024;

bool upsert(const QByteArray &bytes,
            const SteamShortcut &shortcut,
            QByteArray *result,
            QString *errorMessage = nullptr);

QByteArray emptyDocument();

} // namespace SteamShortcutsVdf
