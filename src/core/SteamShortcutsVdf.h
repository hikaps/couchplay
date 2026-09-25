// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QString>

struct SteamShortcut;

namespace SteamShortcutsVdf {

bool upsert(const QByteArray &bytes,
            const SteamShortcut &shortcut,
            QByteArray *result,
            QString *errorMessage = nullptr);

QByteArray emptyDocument();

} // namespace SteamShortcutsVdf
