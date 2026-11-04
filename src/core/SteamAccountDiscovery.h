// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct SteamAccount {
    QString root;
    QString accountId;
    QString kind;
};

QStringList steamRootCandidates(const QString &home);
QList<SteamAccount> discoverSteamAccounts(const QString &home);
