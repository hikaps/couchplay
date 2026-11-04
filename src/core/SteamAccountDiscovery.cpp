// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SteamAccountDiscovery.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {
constexpr auto SteamFlatpakId = "com.valvesoftware.Steam";

bool isSteamFlatpakRoot(const QString &path)
{
    return path.contains(QStringLiteral("/.var/app/") + QString::fromLatin1(SteamFlatpakId) + QLatin1Char('/'));
}
} // namespace

QStringList steamRootCandidates(const QString &home)
{
    return {
        home + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.local/share/Steam"),
        home + QStringLiteral("/.var/app/") + QString::fromLatin1(SteamFlatpakId)
            + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.var/app/") + QString::fromLatin1(SteamFlatpakId)
            + QStringLiteral("/.local/share/Steam"),
        home + QStringLiteral("/.var/app/") + QString::fromLatin1(SteamFlatpakId)
            + QStringLiteral("/data/Steam"),
    };
}

QList<SteamAccount> discoverSteamAccounts(const QString &home)
{
    QList<SteamAccount> accounts;
    QSet<QString> rootsSeen;
    static const QRegularExpression accountIdExpression(QStringLiteral("\\A[0-9]+\\z"));

    for (const QString &candidate : steamRootCandidates(home)) {
        const QFileInfo rootInfo(candidate);
        if (!rootInfo.exists() || !rootInfo.isDir()) {
            continue;
        }
        const QString root = rootInfo.canonicalFilePath();
        if (root.isEmpty() || rootsSeen.contains(root)) {
            continue;
        }
        rootsSeen.insert(root);

        const QString userdataPath = root + QStringLiteral("/userdata");
        const QFileInfo userdataInfo(userdataPath);
        if (!userdataInfo.exists() || !userdataInfo.isDir() || userdataInfo.isSymLink()) {
            continue;
        }

        const bool flatpak = isSteamFlatpakRoot(root) || isSteamFlatpakRoot(candidate);
        const QFileInfoList entries = QDir(userdataPath).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (entry.isSymLink() || !accountIdExpression.match(entry.fileName()).hasMatch()) {
                continue;
            }
            bool ok = false;
            const quint64 accountId = entry.fileName().toULongLong(&ok);
            if (!ok || accountId == 0) {
                continue;
            }
            accounts.append({root, entry.fileName(), flatpak ? QStringLiteral("flatpak") : QStringLiteral("native")});
        }
    }

    std::sort(accounts.begin(), accounts.end(), [](const SteamAccount &left, const SteamAccount &right) {
        return left.root == right.root ? left.accountId < right.accountId : left.root < right.root;
    });
    return accounts;
}
