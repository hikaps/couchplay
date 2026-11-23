// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include "LaunchTypes.h"

#include <QString>
#include <QStringList>

struct CommandLineRequest
{
    QString profileName;
    bool start = false;
    bool exitAfterSession = false;
    bool skipPairingCheck = false;
    GameSelection game;
    int quickPlayers = 0;

    bool requested() const
    {
        return !profileName.isEmpty() || quickPlayers > 0;
    }
};

class CommandLineOptions
{
public:
    static CommandLineRequest parse(const QStringList &arguments, QString *errorMessage = nullptr);
};
