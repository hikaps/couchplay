// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QString>

// "source|alias" wire format shared by the GUI (SessionRunner) and the
// privileged helper (MountSharedDirectories). '\' and '|' inside either field
// are escaped so legal paths like /mnt/Game|Saves round-trip instead of being
// split into a wrong source and alias. Legacy unescaped specs (plain source,
// optional plain alias) decode unchanged; a lone '\' that is not part of an
// escape sequence is kept literally for those legacy paths.
inline QString escapeMountSpecField(const QString &field)
{
    QString escaped = field;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    return escaped;
}

inline QString encodeMountSpec(const QString &source, const QString &alias)
{
    // Always emit the separator: "source|" (empty alias) stays byte-identical
    // to the legacy wire form
    return escapeMountSpecField(source) + QLatin1Char('|') + escapeMountSpecField(alias);
}

inline bool decodeMountSpec(const QString &spec, QString &source, QString &alias)
{
    source.clear();
    alias.clear();

    QString current;
    bool inAlias = false;
    for (int i = 0; i < spec.size(); ++i) {
        const QChar c = spec.at(i);
        if (c == QLatin1Char('\\') && i + 1 < spec.size()) {
            const QChar next = spec.at(i + 1);
            if (next == QLatin1Char('\\') || next == QLatin1Char('|')) {
                current += next;
                ++i; // consume the escaped character
                continue;
            }
            current += c; // lone backslash: literal (legacy path)
        } else if (c == QLatin1Char('|')) {
            if (inAlias) {
                return false; // more than one unescaped separator
            }
            source = current;
            current.clear();
            inAlias = true;
        } else {
            current += c;
        }
    }

    if (inAlias) {
        alias = current;
    } else {
        source = current;
    }
    return true;
}
